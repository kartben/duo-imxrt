/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Button matrix, ported from brains2/apps/duo/{buttons,keypad,key}.h.
 *
 * Scanning and debouncing are handled by Zephyr's gpio-kbd-matrix driver,
 * which reports each change as an (INPUT_ABS_X, INPUT_ABS_Y, INPUT_BTN_TOUCH)
 * triple. What is left here is the DUO-specific part: mapping row/column to a
 * button, and the press/hold/release state machine the panel logic expects.
 */

#include "keys.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_keys, CONFIG_DUO_LOG_LEVEL);

#define KEYS_NODE DT_NODELABEL(keys)

BUILD_ASSERT(DT_NODE_HAS_STATUS(KEYS_NODE, okay), "keys node is missing");

static const uint8_t ROWS = DT_PROP_LEN(KEYS_NODE, row_gpios);
static const uint8_t COLS = DT_PROP_LEN(KEYS_NODE, col_gpios);

BUILD_ASSERT(DT_PROP_LEN(KEYS_NODE, row_gpios) == 4, "unexpected matrix geometry");
BUILD_ASSERT(DT_PROP_LEN(KEYS_NODE, col_gpios) == 6, "unexpected matrix geometry");

/* Time a button must be held before it reports HOLD, as in the legacy firmware. */
static const uint32_t HOLD_TIME_MS = 2000;

/* Matrix wiring of the Brains 2.1 panel (SEQ_1_2 layout). */
static const Button keymap[4][6] = {
	{ BTN_SEQ1,  STEP_8,    STEP_1, BTN_SEQ2, STEP_7, STEP_6 },
	{ DUMMY_KEY, SEQ_START, STEP_2, STEP_3,   STEP_4, STEP_5 },
	{ KEYB_0,    BTN_DOWN,  KEYB_2, KEYB_1,   KEYB_4, KEYB_3 },
	{ KEYB_6,    KEYB_5,    KEYB_8, KEYB_7,   BTN_UP, KEYB_9 },
};

/* The play button doubles as the power button. */
static const uint8_t POWER_ROW = 1;
static const uint8_t POWER_COL = 1;

struct key_event {
	uint8_t row;
	uint8_t col;
	bool pressed;
};

K_MSGQ_DEFINE(key_events, sizeof(struct key_event), 32, 4);

static KeyHandler key_handler;
static KeyState key_state[4][6];
static uint32_t key_pressed_at[4][6];

static void input_cb(struct input_event *evt, void *user_data)
{
	static uint8_t row;
	static uint8_t col;
	static struct key_event pending;

	ARG_UNUSED(user_data);

	switch (evt->code) {
	case INPUT_ABS_X:
		col = evt->value;
		break;
	case INPUT_ABS_Y:
		row = evt->value;
		break;
	case INPUT_BTN_TOUCH:
		pending.row = row;
		pending.col = col;
		pending.pressed = evt->value != 0;

		if (k_msgq_put(&key_events, &pending, K_NO_WAIT) < 0) {
			LOG_WRN("key event queue full, dropping r%u c%u", row, col);
		}
		break;
	default:
		break;
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(KEYS_NODE), input_cb, NULL);

static void dispatch(uint8_t row, uint8_t col, KeyState state)
{
	const Button key = keymap[row][col];

	if (key == DUMMY_KEY || key_handler == nullptr) {
		return;
	}

	key_handler(key, state);
}

void keys_scan(void)
{
	struct key_event evt;
	const uint32_t now = k_uptime_get_32();

	while (k_msgq_get(&key_events, &evt, K_NO_WAIT) == 0) {
		if (evt.row >= ROWS || evt.col >= COLS) {
			continue;
		}

		if (evt.pressed) {
			key_state[evt.row][evt.col] = PRESSED;
			key_pressed_at[evt.row][evt.col] = now;
			dispatch(evt.row, evt.col, PRESSED);
		} else {
			key_state[evt.row][evt.col] = IDLE;
			dispatch(evt.row, evt.col, RELEASED);
		}
	}

	for (uint8_t r = 0; r < ROWS; r++) {
		for (uint8_t c = 0; c < COLS; c++) {
			if (key_state[r][c] != PRESSED) {
				continue;
			}

			if ((now - key_pressed_at[r][c]) >= HOLD_TIME_MS) {
				key_state[r][c] = HOLD;
				dispatch(r, c, HOLD);
			}
		}
	}
}

bool keys_powerbutton_pressed(void)
{
	struct key_event evt;

	/*
	 * While powered down nothing else drains the queue, so consume events
	 * here as well rather than letting them pile up.
	 */
	while (k_msgq_get(&key_events, &evt, K_NO_WAIT) == 0) {
		if (evt.row < ROWS && evt.col < COLS) {
			key_state[evt.row][evt.col] = evt.pressed ? PRESSED : IDLE;
		}
	}

	return key_state[POWER_ROW][POWER_COL] != IDLE;
}

int keys_init(KeyHandler handler)
{
	const struct device *const dev = DEVICE_DT_GET(KEYS_NODE);

	if (!device_is_ready(dev)) {
		LOG_ERR("key matrix not ready");
		return -ENODEV;
	}

	key_handler = handler;

	return 0;
}
