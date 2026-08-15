/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drum pads, ported from brains2/apps/duo/duo-firmware/src/DrumSynth.h and
 * brains2/apps/duo/boards/DUO_BRAINS_2.1/drums.h.
 *
 * Each drum has three touch segments; which of them are held selects the
 * velocity, so the pads behave like a crude position-sensitive trigger.
 */

#pragma once

#include "dsp/voice.h"
#include "platform/pins.h"

static bool kick_playing = 0;
static bool hat_playing = 0;

static void kick_noteon(uint8_t velocity)
{
	duo::voice.kick_note_on(velocity);
	kick_playing = 1;
}

static void kick_noteoff()
{
	duo::voice.kick_note_off();
	kick_playing = 0;
}

static void hat_noteon(uint8_t velocity)
{
	duo::voice.hat_note_on(velocity);
	hat_playing = 1;
}

static void hat_noteoff()
{
	duo::voice.hat_note_off();
	hat_playing = 0;
}

namespace Drums {

static bool hat_l = 0;
static bool hat_m = 0;
static bool hat_r = 0;
static bool kick_l = 0;
static bool kick_m = 0;
static bool kick_r = 0;

static inline void init()
{
	/* Voice-side setup happens in duo::Voice::init(). */
}

static inline void update()
{
	static unsigned long update_time;

	if (millis() - update_time <= 10) {
		return;
	}

	update_time = millis();

	hat_l = pinRead(HAT_PAD_L_PIN);
	hat_m = pinRead(HAT_PAD_M_PIN);
	hat_r = pinRead(HAT_PAD_R_PIN);
	kick_l = pinRead(KICK_PAD_L_PIN);
	kick_m = pinRead(KICK_PAD_M_PIN);
	kick_r = pinRead(KICK_PAD_R_PIN);

	if (hat_playing) {
		if (!hat_l && !hat_r && !hat_m) {
			hat_noteoff();
		}
	} else {
		if (hat_l && hat_m) {
			hat_noteon(31);
		} else if (hat_l) {
			hat_noteon(0);
		} else if (hat_r && hat_m) {
			hat_noteon(95);
		} else if (hat_r) {
			hat_noteon(127);
		} else if (hat_m) {
			hat_noteon(64);
		}
	}

	if (kick_playing) {
		if (!kick_l && !kick_r && !kick_m) {
			kick_noteoff();
		}
	} else {
		if (kick_l && kick_m) {
			kick_noteon(31);
		} else if (kick_l) {
			kick_noteon(0);
		} else if (kick_r && kick_m) {
			kick_noteon(95);
		} else if (kick_r) {
			kick_noteon(127);
		} else if (kick_m) {
			kick_noteon(63);
		}
	}
}

} /* namespace Drums */
