/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dato DUO "Brains 2" firmware, ported from brains2/apps/duo/main.cpp.
 *
 * The structure of the original is kept: a single control loop that alternates
 * between a ~90 Hz "frame" (repaint the panel, emit parameter CCs) and a fast
 * pass that services MIDI, pitch glide, drums, the sequencer and the panel
 * inputs. Audio no longer runs from an interrupt driven by the codec DMA, but
 * from its own thread; see dsp/audio_out.cpp.
 */

#include "compat/duo_compat.h"

#include "platform/audio_path.h"
#include "platform/keys.h"
#include "platform/panel_leds.h"
#include "platform/pins.h"
#include "platform/power.h"
#include "platform/usb_midi.h"

#include "compat/lib/midi_wrapper.h"
#include "compat/lib/sync.h"

#include "dsp/audio_out.h"
#include "dsp/voice.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo, CONFIG_DUO_LOG_LEVEL);

#include "globals.h"

/* Serial number reported over sysex; read from the SoC's unique ID at boot. */
static uint32_t SIM_UIDH;
static uint32_t SIM_UIDMH;
#define SIM_UIDML 0
#define SIM_UIDL 0

#define MIDI_SYSEX_DATA_TYPE byte
#include "shared/duo/MidiFunctions.h"

void midi_set_channel(uint8_t channel)
{
	if (channel > 0 && channel <= 16) {
		MIDI_CHANNEL = channel;
	}
}

uint8_t midi_get_channel()
{
	return MIDI_CHANNEL;
}

#include "compat/lib/elapsedMillis.h"
#include "shared/duo/TempoHandler.h"

TempoHandler tempo_handler;

#include "shared/duo/Sequencer.h"

static void midi_handle_clock()
{
	tempo_handler.midi_clock_received();
}

/*
 * Switches the keyboard to another scale. The pattern in the sequencer moves
 * with it: each step keeps its place in the scale, so what was played on the
 * fourth key still plays the fourth key's note. Steps holding notes from
 * outside the old scale (entered over MIDI) are left as they are.
 */
static void change_scale(uint8_t index)
{
	const duo::Scale &from = duo::current_scale();

	if (!duo::select_scale(index)) {
		return;
	}

	for (uint8_t i = 0; i < Sequencer::NUM_STEPS; i++) {
		sequencer.set_step_note(i, duo::scale_remap(from, duo::current_scale(),
							    sequencer.get_step_note(i)));
	}
}

#include "duo_drums.h"
#include "held_notes.h"

/*
 * Duophonic mode, off unless picked: hold double speed while switching on, or
 * send CC 127 (Poly Mode On; CC 126, Mono Mode On, leaves it). While the
 * sequencer is stopped and two or more notes are held, the lowest plays on the
 * pulse oscillator and the highest on the saw, instead of the last one played
 * on both. They share the filter and the envelopes, so as on the classic
 * duophonic synths a new note retriggers both. The sequencer itself, and what
 * the DUO sends over MIDI, stay monophonic.
 */
static bool duophonic;
static duo::HeldNotes held_notes;

/*
 * CC 3, which the MIDI specification leaves undefined and the DUO does not
 * send, selects the scale: values 0-15 the first, 16-31 the second, and so on,
 * so it works from a knob as well as from a sequencer.
 */
static const uint8_t SCALE_SELECT_CC = 3;

/*
 * CC 14 and 15, likewise undefined and never sent, pick the kick and hat pads'
 * sounds the way holding a pad and pressing a key does: values 0-12 the first
 * key's sound, 13-25 the second's, and so on up to the tenth.
 */
static const uint8_t KICK_SOUND_CC = 14;
static const uint8_t HAT_SOUND_CC = 15;

/* Channel mode messages from the MIDI specification. */
static const uint8_t ALL_NOTES_OFF_CC = 123;
static const uint8_t MONO_MODE_ON_CC = 126;
static const uint8_t POLY_MODE_ON_CC = 127;

static void midi_handle_control_change(uint8_t channel, uint8_t number, uint8_t value)
{
	if (channel == MIDI_CHANNEL) {
		switch (number) {
		case SCALE_SELECT_CC:
			change_scale(value / (128 / duo::SCALE_COUNT));
			return;
		case KICK_SOUND_CC:
			Drums::select_kick_sound(value * duo::PAD_SOUNDS / 128);
			return;
		case HAT_SOUND_CC:
			Drums::select_hat_sound(value * duo::PAD_SOUNDS / 128);
			return;
		case MONO_MODE_ON_CC:
		case POLY_MODE_ON_CC:
			duophonic = number == POLY_MODE_ON_CC;
			/* The specification has both act as All Notes Off as well. */
			held_notes.release_all();
			midi_handle_cc(channel, ALL_NOTES_OFF_CC, 0);
			return;
		case ALL_NOTES_OFF_CC:
			/* The sequencer lets go of every note; so does duophonic mode. */
			held_notes.release_all();
			break;
		}
	}

	midi_handle_cc(channel, number, value);
}

/* MIDI notes count towards duophonic mode's pair, as keys do. */
static void midi_hold_note(byte channel, byte note, byte velocity)
{
	held_notes.press(note);
	midi_note_on(channel, note, velocity);
}

static void midi_let_go_of_note(byte channel, byte note, byte velocity)
{
	held_notes.release(note);
	midi_note_off(channel, note, velocity);
}

/*
 * Sysex commands only this port answers, under the DUO's own header
 * (F0 7D 64 <command> F7). They start at 0x40, clear of the legacy commands,
 * which run from 0x01 to 0x0C.
 */
static const uint8_t SYSEX_AUDIO_LOAD = 0x40;

/*
 * Replies F0 7D 64 40, then the CPU clock in MHz, the audio block size in
 * frames, the mean and worst time to render a block in CPU cycles, the number
 * of blocks those cover and the underruns since start-up, each as five 7-bit
 * bytes, most significant first, then F7. Each reading starts a new window,
 * so the figures cover the time since the previous one.
 * tools/updater/audio_load.py reads it.
 */
static void midi_print_audio_load()
{
	struct audio_load load;

	audio_out_read_load(&load);

	const uint32_t values[] = {load.cpu_mhz,           load.block_frames,
				   load.render_avg_cycles, load.render_max_cycles,
				   load.blocks,            load.underruns};
	uint8_t sysex[4 + 5 * ARRAY_SIZE(values) + 1] = {0xf0, SYSEX_DATO_ID, SYSEX_DUO_ID,
							  SYSEX_AUDIO_LOAD};
	uint8_t *p = &sysex[4];

	for (const uint32_t value : values) {
		for (int shift = 28; shift >= 0; shift -= 7) {
			*p++ = (value >> shift) & 0x7f;
		}
	}
	*p = 0xf7;

	MIDI::sendSysEx(sizeof(sysex), sysex);
}

static void midi_handle_port_sysex(byte *data, unsigned length)
{
	if (length >= 4 && data[1] == SYSEX_DATO_ID && data[2] == SYSEX_DUO_ID &&
	    data[3] == SYSEX_AUDIO_LOAD) {
		midi_print_audio_load();
		return;
	}

	midi_handle_sysex(data, length);
}

static void midi_init()
{
	MIDI::init(MIDI::Callbacks{.note_on = midi_hold_note,
				   .note_off = midi_let_go_of_note,
				   .clock = midi_handle_clock,
				   .start = sequencer_start_from_MIDI,
				   .cont = sequencer_start_from_MIDI,
				   .stop = sequencer_stop,
				   .cc = midi_handle_control_change,
				   .sysex = midi_handle_port_sysex});
}

/* One more LED than the panel physically has, for the production loopback test. */
static const int NUM_LEDS = 19 + 1;
static const int led_order[NUM_LEDS] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
					11, 12, 13, 14, 15, 16, 17, 18, 19, 20};

#include "duo_leds.h"
#include "shared/duo/Pitch.h"

#include "duo_synth.h"

static uint8_t note_is_playing = 0;

/*
 * The note each keyboard key is holding, so that letting go releases the note
 * that was pressed even if the scale changed in between. Keys never play note
 * 0, so that marks a key that holds nothing.
 */
static const uint8_t NO_NOTE = 0;
static uint8_t key_note[duo::KEYBOARD_KEYS];

/*
 * In duophonic mode, with the sequencer stopped and two or more notes held,
 * moves the pulse oscillator to the lowest held note and gives the highest to
 * the saw. Otherwise the pulse keeps `pulse_note` and the saw follows it.
 */
static void pick_duophonic_notes(uint8_t *pulse_note)
{
	uint8_t lowest;
	uint8_t highest;

	if (!duophonic || sequencer.is_running() || !held_notes.outer_pair(&lowest, &highest)) {
		second_note = NO_SECOND_NOTE;
		return;
	}

	if (second_note == NO_SECOND_NOTE) {
		/* With GLIDE on, the saw glides into its note from where it was. */
		second_note_frequency = osc_saw_frequency;
	}

	*pulse_note = lowest + transpose;
	second_note = highest + transpose;
}

void note_on(uint8_t midi_note, uint8_t velocity, bool enabled)
{
	/* The ACCENT button forces full velocity. */
	if (synth.accent) {
		velocity = 127;
	}

	note_is_playing = midi_note;

	if (enabled) {
		uint8_t pulse_note = midi_note;

		pick_duophonic_notes(&pulse_note);

		/* Velocity sets how far the filter envelope opens the filter. */
		duo::voice.set_filter_env_amount(velocity / 127.0f);
		osc_pulse_midi_note = pulse_note;
		osc_pulse_target_frequency = (int)midi_note_to_frequency(pulse_note);
		duo::voice.set_saw_frequency(second_note != NO_SECOND_NOTE
						     ? second_note_saw_frequency()
						     : detune(osc_pulse_midi_note, detune_amount));

		/* MIDI out stays monophonic: the note just played, as before. */
		MIDI::sendNoteOn(midi_note, velocity, MIDI_CHANNEL);
		duo::voice.note_on();
	} else {
		leds(sequencer.cur_step_index() % Sequencer::NUM_STEPS) = LED_WHITE;
	}
}

void note_off()
{
	if (note_is_playing) {
		MIDI::sendNoteOff(note_is_playing, 0, MIDI_CHANNEL);
		duo::voice.note_off();
		note_is_playing = 0;
	}
}

static void pots_read()
{
	synth.speed = potRead(TEMPO_POT);
	synth.gateLength = potRead(GATE_POT);

	synth.resonance = potRead(FILTER_RES_POT);
	synth.release = potRead(AMP_ENV_POT);
	synth.amplitude = potRead(AMP_POT);
	synth.filter = potRead(FILTER_FREQ_POT);
	synth.detune = potRead(OSC_DETUNE_POT);
	synth.pulseWidth = potRead(OSC_PW_POT);

	synth.glide = pinRead(GLIDE_PIN);
	synth.crush = pinRead(BITC_PIN);
	synth.accent = pinRead(ACCENT_PIN);
	synth.delay = pinRead(DELAY_PIN);
}

/* Not reachable in developer mode, where a long press enters the bootloader. */
__maybe_unused static void power_off()
{
	MIDI::sendControlChange(123, 0, MIDI_CHANNEL);

	duo::voice.pop_suppressor_fade_out(20);
	k_msleep(30);

	Audio::amp_disable();
	Audio::headphone_disable();

	audio_out_stop();
	usb_midi_disconnect();
	led_deinit();
	power_flag = false;

	/* Wait for the power button to be let go of before arming the wake-up. */
	while (keys_powerbutton_pressed()) {
		k_msleep(10);
	}

	delay(100);
}

bool is_power_on()
{
	return power_flag;
}

/*
 * Holding a drum pad turns the keyboard into a choice of sounds for that pad,
 * one per key. The new sound plays straight away, so it can be heard while
 * the pad is still held.
 */
static const uint8_t DRUM_AUDITION_VELOCITY = 95;

static void pick_drum_sound(uint8_t key)
{
	if (Drums::kick_pad_held()) {
		Drums::select_kick_sound(key);
		duo::voice.kick_note_on(DRUM_AUDITION_VELOCITY);
	}

	if (Drums::hat_pad_held()) {
		Drums::select_hat_sound(key);
		duo::voice.hat_note_on(DRUM_AUDITION_VELOCITY);
	}
}

static void process_key(const Button k, const KeyState state)
{
	switch (state) {
	case PRESSED:
		if (k <= KEYB_9 && k >= KEYB_0) {
			if (in_setup) {
				midi_set_channel((k - KEYB_0) + 1);
			} else if (Drums::kick_pad_held() || Drums::hat_pad_held()) {
				/* The key picks a sound and holds no note to release. */
				pick_drum_sound(k - KEYB_0);
			} else {
				key_note[k - KEYB_0] = SCALE[k - KEYB_0];
				/* Before the sequencer, whose note_on() looks at it. */
				held_notes.press(key_note[k - KEYB_0]);
				keyboard_set_note(key_note[k - KEYB_0]);
			}
		} else if (k <= STEP_8 && k >= STEP_1) {
			const uint8_t step = k - STEP_1;

			if (in_setup) {
				/* Held at power-on, a step button picks the scale. */
				change_scale(step);
			} else if (!sequencer.toggle_step(step)) {
				leds(step) = CRGB::Black;
			}
		} else if (k == BTN_SEQ2) {
			if (in_setup) {
				/* Held at power-on, double speed turns duophonic mode on. */
				duophonic = true;
			} else {
				if (!sequencer.is_running()) {
					sequencer.advance();
				}
				double_speed = true;
			}
		} else if (k == BTN_DOWN) {
			transpose--;
			if (transpose < -12) {
				transpose = -24;
			}
		} else if (k == BTN_UP) {
			transpose++;
			if (transpose > 12) {
				transpose = 24;
			}
		} else if (k == BTN_SEQ1) {
			if (sequencer.is_running()) {
				random_flag = true;
			} else {
				sequencer_randomize_step_offset(sequencer);
			}
		} else if (k == SEQ_START) {
			sequencer_toggle_start();
		}
		break;
	case HOLD:
		if (k <= KEYB_9 && k >= KEYB_0) {
			if (in_setup) {
				midi_set_channel((k - KEYB_0) + 1);
			}
		} else if (k == SEQ_START) {
#ifdef CONFIG_DUO_DEV_MODE
			sequencer_stop();
			FastLED.clear();
			FastLED.show();
			delay(1);
			physical_leds[0] = CRGB::Blue;
			FastLED.show();
			dfu_flag = 1;
#else
			power_off();
#endif
		}
		break;
	case RELEASED:
		if (k <= KEYB_9 && k >= KEYB_0) {
			if (key_note[k - KEYB_0] != NO_NOTE) {
				held_notes.release(key_note[k - KEYB_0]);
				keyboard_unset_note(key_note[k - KEYB_0]);
				key_note[k - KEYB_0] = NO_NOTE;
			}
		} else if (k == BTN_SEQ2) {
			double_speed = false;
		} else if (k == BTN_DOWN || k == BTN_UP) {
			if (transpose < -12) {
				transpose = -12;
			}
			if (transpose > 12) {
				transpose = 12;
			}
		} else if (k == BTN_SEQ1) {
			random_flag = false;
		} else if (k == SEQ_START) {
#ifdef CONFIG_DUO_DEV_MODE
			if (dfu_flag == 1) {
				enter_dfu();
			}
#endif
		}
		break;
	case IDLE:
		/*
		 * Not reported by keys.cpp: a key goes back to idle when it is
		 * released, and the release above has already let go of its
		 * note. (The legacy handler released note number `k - KEYB_0`
		 * here, which is not a note any key plays.)
		 */
		break;
	}
}

static void panel_scan()
{
	/* The DELAY switch is edge-triggered: it fades the delay in and out. */
	if (pinRead(DELAY_PIN) && synth.delay == true) {
		synth.delay = false;
		duo::voice.set_delay_enabled(false);
	} else if (!pinRead(DELAY_PIN) && synth.delay == false) {
		synth.delay = true;
		duo::voice.set_delay_enabled(true);
	}

	keys_scan();
}

static void headphone_jack_check()
{
	static unsigned long next_jack_check_time = 0;
	const unsigned int jack_check_interval = 200;

	if (millis() <= next_jack_check_time) {
		return;
	}

	next_jack_check_time = millis() + jack_check_interval;

	if (headphone_jack_detected()) {
		MAIN_GAIN = HEADPHONE_MAIN_GAIN;
		DELAY_GAIN = HEADPHONE_DELAY_GAIN;
		KICK_GAIN = HEADPHONE_KICK_GAIN;
		HAT_GAIN = HEADPHONE_HAT_GAIN;
		Audio::headphone_enable();
		Audio::amp_disable();
	} else {
		MAIN_GAIN = SPEAKER_MAIN_GAIN;
		DELAY_GAIN = SPEAKER_DELAY_GAIN;
		KICK_GAIN = SPEAKER_KICK_GAIN;
		HAT_GAIN = SPEAKER_HAT_GAIN;
		Audio::headphone_disable();
		Audio::amp_enable();
	}
}

static void main_loop()
{
	static unsigned long frame_time = millis();
	static const unsigned long frame_interval = 11;

	while (true) {
		if (is_power_on()) {
			/*
			 * The clock is generated by polling, so it runs on every
			 * pass rather than only on the fast ones - otherwise a tick
			 * falling during the frame below is late by however long
			 * the panel repaint takes, and that lands on the MIDI clock
			 * and the sync jack.
			 */
			sequencer_update();

			if (millis() - frame_time > frame_interval) {
				frame_time = millis();
				led_update();
				midi_send_cc();
				FastLED.show();
			} else {
				MIDI::read(MIDI_CHANNEL);
				pitch_update();
				synth_update();
				Drums::update();
				headphone_jack_check();
				pots_read();
				panel_scan();
			}
		} else {
			if (millis() - frame_time > frame_interval) {
				frame_time = millis();
				if (keys_powerbutton_pressed()) {
					power_reset();
				}
			}

			k_msleep(1);
		}
	}
}

static void main_init()
{
	/*
	 * The MIDI channel is meant to be restored from persistent storage;
	 * the legacy firmware has that path commented out too, and defaults
	 * to channel 1.
	 */
	const uint8_t stored_midi_channel = 1;

	midi_set_channel(stored_midi_channel);

	const uint32_t previous_frame_time = millis();

	/*
	 * Order matters: the sequencer and buttons come up first so that a key
	 * held down at power-on can select the MIDI channel, before the LEDs
	 * light up to show which one was chosen.
	 */
	sequencer_init();

	while (millis() - previous_frame_time < 100) {
		panel_scan();
	}

	midi_init();
	led_init();

	Drums::init();
	audio_init();
	audio_out_start();

	Audio::amp_init();
	Audio::headphone_enable();
	Audio::amp_enable();

	in_setup = false;
}

void enter_dfu()
{
	/* Blank the panel and turn the power button teal before rebooting. */
	FastLED.clear();
	for (int i = 0; i < NUM_LEDS; i++) {
		physical_leds[i] = CRGB::Black;
	}
	physical_leds[0] = CRGB::Teal;

	for (int i = 0; i < 4; i++) {
		FastLED.show();
		delay(100);
	}

	power_enter_rom_bootloader();
}

int main(void)
{
	power_read_device_id(&SIM_UIDH, &SIM_UIDMH);

	if (pins_init() < 0) {
		LOG_ERR("panel inputs failed to initialise");
	}

	if (panel_leds_init() < 0) {
		LOG_ERR("panel LEDs failed to initialise");
	}

	if (keys_init(process_key) < 0) {
		LOG_ERR("key matrix failed to initialise");
	}

	Sync::init();

	Audio::amp_init();
	Audio::amp_disable();
	Audio::headphone_disable();

	main_init();
	main_loop();

	return 0;
}
