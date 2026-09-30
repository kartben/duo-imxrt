/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel-to-voice parameter mapping, ported from
 * brains2/apps/duo/duo-firmware/src/Synth.h. The patch itself now lives in
 * dsp/voice.h; what remains here is the mapping from 10 bit pot readings to
 * synth parameters, which is unchanged.
 */

#pragma once

#include "dsp/voice.h"

/* Output gains, from brains2/core/boards/DUO_BRAINS_2.1/board_audio_output.h. */
static const float HEADPHONE_GAIN = 2.0f;
static const float SPEAKER_GAIN = 2.0f;

static const float HEADPHONE_KICK_GAIN = 0.5f;
static const float HEADPHONE_HAT_GAIN = 0.5f;
static const float HEADPHONE_DELAY_GAIN = 1.0f;
static const float HEADPHONE_MAIN_GAIN = 0.8f;

static const float SPEAKER_MAIN_GAIN = 0.8f;
static const float SPEAKER_DELAY_GAIN = 1.0f;
static const float SPEAKER_KICK_GAIN = 0.8f;
static const float SPEAKER_HAT_GAIN = 1.0f;

float MAIN_GAIN = 0.8f;
float DELAY_GAIN = 1.0f;
float KICK_GAIN = 1.0f;
float HAT_GAIN = 1.2f;

static void audio_init()
{
	duo::voice.init();
	duo::voice.set_preamp_gains(HEADPHONE_GAIN, SPEAKER_GAIN);
	duo::voice.set_output_gains(MAIN_GAIN, DELAY_GAIN, KICK_GAIN, HAT_GAIN);
}

static void audio_volume(int volume)
{
	static const int LOW_VOLUME_THRESHOLD = 4;

	if (volume < LOW_VOLUME_THRESHOLD) {
		duo::voice.set_output_gains(0.0f, 0.0f, 0.0f, 0.0f);
	} else {
		duo::voice.set_output_gains(volume / (1023.0f / MAIN_GAIN),
					    volume / (1023.0f / DELAY_GAIN),
					    (volume + 512) / (2048.0f / KICK_GAIN),
					    (volume + 512) / (2048.0f / HAT_GAIN));
	}
}

/*
 * Duophonic mode's second note, which the saw oscillator plays in place of
 * following the pulse oscillator at the DETUNE interval; NO_SECOND_NOTE the
 * rest of the time. note_on() in main.cpp sets it.
 */
static const int NO_SECOND_NOTE = -1;
static int second_note = NO_SECOND_NOTE;
static float second_note_frequency;

/*
 * The saw normally sits well under the pulse (0.4 against its 0.5, in a mixer
 * weighted 0.2 to 0.4). Playing a note of its own, it is brought up to about
 * 3 dB below the pulse, so both notes carry.
 */
static const float DUOPHONIC_SAW_AMPLITUDE = 1.2f;

/* The saw's frequency for the second note, gliding to it the way pitch_update() does. */
static float second_note_saw_frequency()
{
	static uint32_t glide_time;

	/* An octave down: synth_update() plays the pulse an octave down too. */
	const float target = midi_note_to_frequency(second_note - 12);

	if (synth.glide) {
		if (millis() - glide_time > 10) {
			second_note_frequency += (target - second_note_frequency) * 0.3f;
			glide_time = millis();
		}
	} else {
		second_note_frequency = target;
	}

	return second_note_frequency;
}

static void synth_update()
{
	float osc_saw_amplitude = 0.4f;
	float saw_frequency = osc_saw_frequency;

	if (synth.detune > 850) {
		osc_saw_amplitude = map(synth.detune, 850, 1023, 400, 0) / 1000.0f;
	} else if (synth.detune < 200) {
		osc_saw_amplitude = map(synth.detune, 0, 200, 200, 400) / 1000.0f;
	}

	if (second_note != NO_SECOND_NOTE) {
		saw_frequency = second_note_saw_frequency();
		osc_saw_amplitude = DUOPHONIC_SAW_AMPLITUDE;
	}

	float bitcrusher_samplerate = HIGH_SAMPLE_RATE;

	if (synth.crush) {
		bitcrusher_samplerate = LOW_SAMPLE_RATE;
	}

	const float osc_pulse_pulseWidth = map(synth.pulseWidth, 0, 1023, 500, 950) / 1000.0f;
	float filter_resonance = map(synth.resonance, 0, 1023, 70, 320) / 100.0f;

	if (synth.accent) {
		filter_resonance = 4.0f;
	}

	duo::voice.set_saw_frequency(saw_frequency);
	duo::voice.set_saw_amplitude(osc_saw_amplitude);

	duo::voice.set_pulse_frequency(osc_pulse_frequency / 2);
	duo::voice.set_pulse_width(osc_pulse_pulseWidth);

	duo::voice.set_filter_frequency((synth.filter / 2) + 30);
	duo::voice.set_filter_resonance(filter_resonance);

	duo::voice.set_amp_release((float)(((synth.release * synth.release) >> 11) + 30));

	duo::voice.set_crush_sample_rate(bitcrusher_samplerate);

	audio_volume(synth.amplitude);
}
