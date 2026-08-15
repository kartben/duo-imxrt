/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speaker amplifier and headphone driver enables, ported from
 * brains2/core/boards/DUO_BRAINS_2.1/audio.cpp.
 */

#include "audio_path.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_audio_path, CONFIG_DUO_LOG_LEVEL);

#define AUDIO_PATH_NODE DT_NODELABEL(audio_path)

BUILD_ASSERT(DT_NODE_HAS_STATUS(AUDIO_PATH_NODE, okay), "audio_path node is missing");

static const struct gpio_dt_spec hp_enable_gpio =
	GPIO_DT_SPEC_GET(AUDIO_PATH_NODE, headphone_enable_gpios);
static const struct gpio_dt_spec amp_mute_gpio =
	GPIO_DT_SPEC_GET(AUDIO_PATH_NODE, amp_mute_gpios);

namespace Audio {

void amp_init(void)
{
	if (!gpio_is_ready_dt(&hp_enable_gpio) || !gpio_is_ready_dt(&amp_mute_gpio)) {
		LOG_ERR("audio path GPIOs not ready");
		return;
	}

	gpio_pin_configure_dt(&hp_enable_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&amp_mute_gpio, GPIO_OUTPUT_INACTIVE);
}

void amp_enable(void)
{
	gpio_pin_configure_dt(&amp_mute_gpio, GPIO_OUTPUT_INACTIVE);
}

void amp_disable(void)
{
	/*
	 * Assert shutdown, then let go of the line: the amplifier holds itself
	 * muted through its own pull-up, and releasing the pin avoids leaking
	 * current into it while the DUO is powered down.
	 */
	gpio_pin_configure_dt(&amp_mute_gpio, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&amp_mute_gpio, GPIO_INPUT);
}

void headphone_enable(void)
{
	gpio_pin_configure_dt(&hp_enable_gpio, GPIO_OUTPUT_ACTIVE);
}

void headphone_disable(void)
{
	gpio_pin_configure_dt(&hp_enable_gpio, GPIO_OUTPUT_INACTIVE);
}

} /* namespace Audio */
