/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from the analogWrite() macros in
 * brains2/core/boards/DUO_BRAINS_2.1/led_pins.h. Those write a duty cycle
 * where 0 is fully lit and 255 is off, because the LEDs sink through the pin.
 * The devicetree marks these PWM channels inverted, so the duty computed by
 * the original expressions is converted to a brightness percentage here.
 */

#include "panel_leds.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_panel_leds, CONFIG_DUO_LOG_LEVEL);

static const struct device *const pwm_leds = DEVICE_DT_GET(DT_NODELABEL(panel_pwm_leds));

enum {
	LED_OSC = DT_NODE_CHILD_IDX(DT_NODELABEL(osc_led)),
	LED_FILTER = DT_NODE_CHILD_IDX(DT_NODELABEL(filter_led)),
	LED_ENV = DT_NODE_CHILD_IDX(DT_NODELABEL(env_led)),
};

static void write_duty(uint32_t led, uint32_t duty)
{
	if (duty > 255) {
		duty = 255;
	}

	led_set_brightness(pwm_leds, led, (uint8_t)(((255 - duty) * 100) / 255));
}

void write_env_led(float peak)
{
	if (peak < 0.0f) {
		peak = 0.0f;
	} else if (peak > 1.0f) {
		peak = 1.0f;
	}

	write_duty(LED_ENV, 254 - (uint32_t)(peak * 254.0f));
}

void write_filter_led(int filter)
{
	if (filter < 0) {
		filter = 0;
	}

	write_duty(LED_FILTER, 254 - (uint32_t)((filter * filter) / 4128));
}

void write_osc_led(int pulse_width)
{
	if (pulse_width < 0) {
		pulse_width = 0;
	}

	write_duty(LED_OSC, (uint32_t)(pulse_width / 4.03f) + 1);
}

void blank_env_led(void)
{
	write_duty(LED_ENV, 255);
}

void blank_filter_led(void)
{
	write_duty(LED_FILTER, 255);
}

void blank_osc_led(void)
{
	write_duty(LED_OSC, 255);
}

int panel_leds_init(void)
{
	if (!device_is_ready(pwm_leds)) {
		LOG_ERR("panel PWM LEDs not ready");
		return -ENODEV;
	}

	blank_env_led();
	blank_filter_led();
	blank_osc_led();

	return 0;
}
