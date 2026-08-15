/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel lighting, ported from brains2/apps/duo/duo-firmware/src/Leds.h.
 *
 * One behavioural difference from the legacy firmware: blend() there is a stub
 * in the FastLED adapter that returns its first argument unchanged, so the
 * current-step colour never actually crossfades. Here it performs the real
 * crossfade the code was written for.
 */

#pragma once

#include "dsp/voice.h"
#include "platform/FastLED.h"
#include "platform/panel_leds.h"

#define LED_WHITE CRGB(230, 255, 150)

#define leds(A) physical_leds[led_order[A]]

CRGB physical_leds[NUM_LEDS];
#define led_play physical_leds[0]

static const int SK6812_BRIGHTNESS = 32;
static const int SK6805_BRIGHTNESS = 140;

#define CORRECTION_SK6812 0xFFF1E0
#define CORRECTION_SK6805 0xFFD3E0

/* The black keys have assigned colours; the white keys are shown in grey. */
static const CRGB COLORS[] = {
	0x444444, 0xFF0001, 0x444444, 0xFFDD00, 0x444444, 0x444444,
	0x11FF00, 0x444444, 0x0033DD, 0x444444, 0xFF00FF, 0x444444,
	0x444444, 0xFF2209, 0x444444, 0x99FF00, 0x444444, 0x444444,
	0x00EE22, 0x444444, 0x0099CC, 0x444444, 0xBB33BB, 0x444444,
};

static void led_init()
{
	FastLED.addLeds<0, 0, 0>(physical_leds, NUM_LEDS);

	FastLED.setBrightness(SK6805_BRIGHTNESS);
	FastLED.setCorrection(CORRECTION_SK6812);

	FastLED.clear();
	physical_leds[NUM_LEDS - 1] = CRGB(0xff6805);
	FastLED.show();

	FastLED.clear();
	FastLED.show();

	/*
	 * The 400 ms of delay in this startup animation is deliberate: it also
	 * prevents an audible pop as the amplifier comes up.
	 */
	physical_leds[MIDI_CHANNEL + 8] = COLORS[SCALE[MIDI_CHANNEL - 1] % 24];

	FastLED.show();
	delay(100);
	FastLED.show();
	delay(100);
	FastLED.show();
	delay(100);

	for (uint16_t i = 0; i < 10; i++) {
		write_env_led(i * 8 / 255.0f);
		write_filter_led(i * 8);
		write_osc_led(i * 8);

		physical_leds[i + 9] = COLORS[SCALE[i] % 24];
		delay(20);
		FastLED.show();
	}
}

/* Fades the panel down and parks it in a low power state. */
static void led_deinit()
{
	for (int i = 32; i > 1; i = (i * 7) >> 3) {
		FastLED.setBrightness(i);
		FastLED.show();
		delay(20);
	}

	blank_env_led();
	blank_filter_led();
	blank_osc_led();
	FastLED.clear();
	FastLED.show();
}

/* Repaints the panel from the sequencer state. */
static void led_update()
{
	for (uint16_t i = 0; i < 10; i++) {
		physical_leds[i + 9] = COLORS[SCALE[i] % 24];
	}

	for (int l = 0; l < Sequencer::NUM_STEPS; l++) {
		if (sequencer.get_step_enabled(l)) {
			leds(l) = COLORS[sequencer.get_step_note(l) % 24];
		} else {
			leds(l) = CRGB::Black;
		}
	}

	const auto cur_seq_step = sequencer.cur_step_index();

	if (sequencer.gate_active()) {
		leds(cur_seq_step) = LED_WHITE;
	}

	if (sequencer.is_running()) {
		led_play = LED_WHITE;
	} else {
		if (sequencer.note_playing()) {
			leds(Sequencer::wrapped_step(cur_seq_step)) = LED_WHITE;
		} else {
			const unsigned step_ticks = Sequencer::TICKS_PER_STEP;
			const uint32_t seq_clock = sequencer.get_clock() + step_ticks;
			const uint32_t fade_val = (seq_clock % step_ticks) * 16;
			const bool fade_play = (seq_clock % (2 * step_ticks)) < step_ticks;

			/* Alternate between fading the play button and the current step. */
			if (fade_play) {
				led_play = LED_WHITE;
				led_play.fadeLightBy(fade_val);
			} else {
				led_play = CRGB::Black;

				if (sequencer.get_step_enabled(cur_seq_step)) {
					leds(cur_seq_step) = blend(
						LED_WHITE,
						COLORS[sequencer.get_step_note(cur_seq_step) % 24],
						fade_val);
				} else {
					leds(cur_seq_step) = LED_WHITE;
					leds(cur_seq_step).fadeLightBy(fade_val);
				}
			}
		}
	}

	write_env_led(duo::voice.peak());
	write_filter_led(synth.filter);
	write_osc_led(synth.pulseWidth);
}
