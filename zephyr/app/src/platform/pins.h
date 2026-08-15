/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same interface as brains2/core/lib/pins.h: everything on the panel is
 * addressed as either a "pot" (0..1023) or a "pin" (boolean).
 */

#pragma once

#include <cstdint>

enum Pin {
	GLIDE_PIN,
	DELAY_PIN,
	ACCENT_PIN,
	BITC_PIN,
	HP_DETECT_PIN,
	SYNC_DETECT_PIN,
	HAT_PAD_L_PIN,
	HAT_PAD_M_PIN,
	HAT_PAD_R_PIN,
	KICK_PAD_L_PIN,
	KICK_PAD_M_PIN,
	KICK_PAD_R_PIN,
};

enum Pot {
	FILTER_RES_POT,
	TEMPO_POT,
	GATE_POT,
	AMP_POT,
	FILTER_FREQ_POT,
	OSC_DETUNE_POT,
	OSC_PW_POT,
	AMP_ENV_POT,
};

int pins_init();
uint32_t potRead(const Pot pot);
bool pinRead(const Pin pin);
bool headphone_jack_detected();
