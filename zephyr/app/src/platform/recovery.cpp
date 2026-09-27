/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * A way into the serial downloader that does not rely on the firmware working.
 *
 * Normally the updater reboots the DUO into the i.MX RT boot ROM's USB serial
 * downloader with a sysex message. That needs USB MIDI up and the firmware
 * running; if a build faults during start-up, hangs, or never enumerates, the
 * sysex never arrives and there would be no way to flash a fix short of
 * opening the case to reach the boot mode pads.
 *
 * So: hold both arrow buttons while the DUO switches on. This check runs as
 * soon as the GPIO drivers are up, before the drivers for USB, audio, the ADC,
 * PWM and the LED strip initialise, and reads the key matrix directly through
 * GPIO, so it only depends on the SoC being alive. If both
 * keys stay down for half a second, the three indicator LEDs light and the DUO
 * reboots into the serial downloader, where `uv run update_firmware.py` finds
 * it. When the keys are not held, boot carries on without delay.
 */

#include "keys.h"
#include "power.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#define KEYS_NODE DT_NODELABEL(keys)
#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec rows[] = {
	DT_FOREACH_PROP_ELEM_SEP(KEYS_NODE, row_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static const struct gpio_dt_spec cols[] = {
	DT_FOREACH_PROP_ELEM_SEP(KEYS_NODE, col_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

BUILD_ASSERT(ARRAY_SIZE(rows) == KEY_ROWS && ARRAY_SIZE(cols) == KEY_COLS,
	     "recovery scan and key map disagree on the matrix geometry");

#if DT_NODE_HAS_PROP(USER_NODE, recovery_led_gpios)
static const struct gpio_dt_spec leds[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, recovery_led_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};
#endif

static constexpr KeyPosition RECOVERY_KEYS[] = {
	key_position(BTN_DOWN),
	key_position(BTN_UP),
};

static_assert(RECOVERY_KEYS[0].row < KEY_ROWS && RECOVERY_KEYS[1].row < KEY_ROWS,
	      "recovery keys missing from the key map");

/* Matches the gpio-kbd-matrix driver's default settle time. */
static const uint32_t SETTLE_US = 50;
static const uint32_t HOLD_MS = 500;
static const uint32_t POLL_MS = 10;

static bool key_down(const KeyPosition &key)
{
	const struct gpio_dt_spec *col = &cols[key.col];

	/* Drive only this column; the others float, as the matrix driver leaves them. */
	gpio_pin_configure_dt(col, GPIO_OUTPUT_ACTIVE);
	k_busy_wait(SETTLE_US);
	const bool down = gpio_pin_get_dt(&rows[key.row]) > 0;
	gpio_pin_configure_dt(col, GPIO_INPUT);

	return down;
}

static bool recovery_keys_down()
{
	for (const KeyPosition &key : RECOVERY_KEYS) {
		if (!key_down(key)) {
			return false;
		}
	}

	return true;
}

static int duo_recovery_check()
{
	for (const struct gpio_dt_spec &gpio : rows) {
		if (!gpio_is_ready_dt(&gpio) || gpio_pin_configure_dt(&gpio, GPIO_INPUT) < 0) {
			return 0;
		}
	}

	for (const struct gpio_dt_spec &gpio : cols) {
		if (!gpio_is_ready_dt(&gpio) || gpio_pin_configure_dt(&gpio, GPIO_INPUT) < 0) {
			return 0;
		}
	}

	/* Give the row pull-ups time to charge the lines before the first look. */
	k_busy_wait(1000);

	for (uint32_t held = 0; held < HOLD_MS; held += POLL_MS) {
		if (!recovery_keys_down()) {
			return 0;
		}
		k_busy_wait(POLL_MS * 1000);
	}

#if DT_NODE_HAS_PROP(USER_NODE, recovery_led_gpios)
	for (const struct gpio_dt_spec &led : leds) {
		if (gpio_is_ready_dt(&led)) {
			gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
		}
	}
#endif

	power_enter_rom_bootloader();

	return 0;
}

/*
 * Straight after the GPIO drivers, and ahead of everything that could fail or
 * hang on a DUO. SYS_INIT() cannot take arithmetic on other priorities, so the
 * ordering is checked here instead.
 */
#define RECOVERY_INIT_PRIORITY 41

BUILD_ASSERT(CONFIG_GPIO_INIT_PRIORITY < RECOVERY_INIT_PRIORITY,
	     "the recovery check needs the GPIO drivers");
BUILD_ASSERT(CONFIG_ADC_INIT_PRIORITY > RECOVERY_INIT_PRIORITY &&
		     CONFIG_PWM_INIT_PRIORITY > RECOVERY_INIT_PRIORITY &&
		     CONFIG_I2S_INIT_PRIORITY > RECOVERY_INIT_PRIORITY &&
		     CONFIG_KERNEL_INIT_PRIORITY_DEVICE > RECOVERY_INIT_PRIORITY &&
		     CONFIG_LED_STRIP_INIT_PRIORITY > RECOVERY_INIT_PRIORITY &&
		     CONFIG_USBD_THREAD_INIT_PRIO > RECOVERY_INIT_PRIORITY,
	     "the recovery check must run before the drivers it guards against");

SYS_INIT(duo_recovery_check, POST_KERNEL, RECOVERY_INIT_PRIORITY);
