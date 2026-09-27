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

static const uint8_t ROWS = KEY_ROWS;
static const uint8_t COLS = KEY_COLS;

BUILD_ASSERT(DT_PROP_LEN(KEYS_NODE, row_gpios) == KEY_ROWS, "unexpected matrix geometry");
BUILD_ASSERT(DT_PROP_LEN(KEYS_NODE, col_gpios) == KEY_COLS, "unexpected matrix geometry");

/* Time a button must be held before it reports HOLD, as in the legacy firmware. */
static const uint32_t HOLD_TIME_MS = 2000;

/* The play button doubles as the power button. */
static constexpr KeyPosition POWER_KEY = key_position(SEQ_START);
static const uint8_t POWER_ROW = POWER_KEY.row;
static const uint8_t POWER_COL = POWER_KEY.col;

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
	const Button key = KEYMAP[row][col];

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
