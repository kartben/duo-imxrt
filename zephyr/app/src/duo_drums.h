/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drum pads, ported from brains2/apps/duo/duo-firmware/src/DrumSynth.h and
 * brains2/apps/duo/boards/DUO_BRAINS_2.1/drums.h.
 *
 * Each drum has three touch segments; which of them are held selects the
 * velocity, so the pads behave like a crude position-sensitive trigger.
 *
 * Each pad plays one of ten sounds (drum_kits.h), picked by holding the pad
 * and pressing a key. The first is the synthesised drum the legacy firmware
 * plays, and is where both pads start.
 */

#pragma once

#include "drum_kits.h"
#include "dsp/voice.h"
#include "platform/pins.h"

static bool kick_playing = 0;
static bool hat_playing = 0;

/* When each pad was touched, for as long as it stays held. */
static unsigned long kick_touched_at;
static unsigned long hat_touched_at;

static void kick_noteon(uint8_t velocity)
{
	duo::voice.kick_note_on(velocity);
	kick_playing = 1;
	kick_touched_at = millis();
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
	hat_touched_at = millis();
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

/* The sound each pad plays, as the index of the key that picked it. */
static uint8_t kick_sound = 0;
static uint8_t hat_sound = 0;

static inline void select_kick_sound(uint8_t index)
{
	if (index >= duo::PAD_SOUNDS) {
		return;
	}

	kick_sound = index;
	duo::voice.set_kick_sample(duo::KICK_PAD_SOUNDS[index].data,
				   duo::KICK_PAD_SOUNDS[index].length);
}

static inline void select_hat_sound(uint8_t index)
{
	if (index >= duo::PAD_SOUNDS) {
		return;
	}

	hat_sound = index;
	duo::voice.set_hat_sample(duo::HAT_PAD_SOUNDS[index].data, duo::HAT_PAD_SOUNDS[index].length);
}

static inline uint8_t kick_sound_index()
{
	return kick_sound;
}

static inline uint8_t hat_sound_index()
{
	return hat_sound;
}

static inline bool kick_pad_held()
{
	return kick_playing;
}

static inline bool hat_pad_held()
{
	return hat_playing;
}

/* How long each pad has been held, or 0 while it is not. */
static inline unsigned long kick_pad_held_ms()
{
	return kick_playing ? millis() - kick_touched_at : 0;
}

static inline unsigned long hat_pad_held_ms()
{
	return hat_playing ? millis() - hat_touched_at : 0;
}

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
