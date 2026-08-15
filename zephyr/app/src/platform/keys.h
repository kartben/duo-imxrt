/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

/*
 * Button identifiers, unchanged from brains2/apps/duo/buttons.h so that the
 * key handling in main.cpp keeps working on ordering (KEYB_0..KEYB_9,
 * STEP_1..STEP_8).
 */
enum Button : uint8_t {
	DUMMY_KEY,
	KEYB_0, KEYB_1, KEYB_2, KEYB_3, KEYB_4,
	KEYB_5, KEYB_6, KEYB_7, KEYB_8, KEYB_9,
	STEP_1, STEP_2, STEP_3, STEP_4,
	STEP_5, STEP_6, STEP_7, STEP_8,
	BTN_DOWN, BTN_UP,
	BTN_SEQ1, BTN_SEQ2,
	SEQ_START,
	NO_KEY,
};

/* Key lifecycle, matching the Keypad library's state machine. */
enum KeyState : uint8_t {
	IDLE,
	PRESSED,
	HOLD,
	RELEASED,
};

typedef void (*KeyHandler)(Button key, KeyState state);

int keys_init(KeyHandler handler);

/*
 * Drains pending key events, invoking the handler for each state change.
 * Must be called from the main loop, as the handler drives the sequencer.
 */
void keys_scan(void);

/* True while the play/power button is held down. */
bool keys_powerbutton_pressed(void);
