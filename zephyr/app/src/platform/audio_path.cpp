/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Speaker amplifier and headphone driver enables, ported from
 * brains2/core/boards/DUO_BRAINS_2.1/audio.cpp and, for boards whose amplifier
 * shutdown line is strapped (mute-level-strap), DUO_BRAINS_2.3/audio.cpp.
 */

#include "audio_path.h"
#include "audio_path_pinctrl.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_audio_path, CONFIG_DUO_LOG_LEVEL);

#define AUDIO_PATH_NODE DT_NODELABEL(audio_path)

BUILD_ASSERT(DT_NODE_HAS_STATUS(AUDIO_PATH_NODE, okay), "audio_path node is missing");

#define MUTE_LEVEL_STRAP DT_PROP(AUDIO_PATH_NODE, mute_level_strap)

BUILD_ASSERT(!MUTE_LEVEL_STRAP || DT_PINCTRL_HAS_NAME(AUDIO_PATH_NODE, default),
	     "mute-level-strap needs a default pinctrl state to release the line in");

static const struct gpio_dt_spec hp_enable_gpio =
	GPIO_DT_SPEC_GET(AUDIO_PATH_NODE, headphone_enable_gpios);
static const struct gpio_dt_spec amp_mute_gpio =
	GPIO_DT_SPEC_GET(AUDIO_PATH_NODE, amp_mute_gpios);

static bool initialised;

/* With a strapped shutdown line, the physical level that mutes the amplifier. */
static int mute_level = 1;

namespace Audio {

void amp_init(void)
{
	if (initialised) {
		return;
	}

	if (!gpio_is_ready_dt(&hp_enable_gpio) || !gpio_is_ready_dt(&amp_mute_gpio)) {
		LOG_ERR("audio path GPIOs not ready");
		return;
	}

	gpio_pin_configure_dt(&hp_enable_gpio, GPIO_OUTPUT_INACTIVE);

	if (MUTE_LEVEL_STRAP) {
		/*
		 * The strap holds the amplifier muted from power-on. Read which
		 * level that is with the pad's keeper out of the way, then keep
		 * driving it until the amplifier is enabled.
		 */
		duo_audio_path_pinctrl_apply();
		gpio_pin_configure_dt(&amp_mute_gpio, GPIO_INPUT);
		mute_level = gpio_pin_get_raw(amp_mute_gpio.port, amp_mute_gpio.pin) > 0;
		gpio_pin_configure(amp_mute_gpio.port, amp_mute_gpio.pin,
				   mute_level ? GPIO_OUTPUT_HIGH : GPIO_OUTPUT_LOW);

		LOG_INF("speaker amplifier mutes on a %s level", mute_level ? "high" : "low");
	} else {
		/* Start muted, so that bringing the pin up cannot pop. */
		gpio_pin_configure_dt(&amp_mute_gpio, GPIO_OUTPUT_ACTIVE);
	}

	initialised = true;
}

void amp_enable(void)
{
	if (MUTE_LEVEL_STRAP) {
		gpio_pin_set_raw(amp_mute_gpio.port, amp_mute_gpio.pin, !mute_level);
	} else {
		gpio_pin_configure_dt(&amp_mute_gpio, GPIO_OUTPUT_INACTIVE);
	}
}

void amp_disable(void)
{
	if (MUTE_LEVEL_STRAP) {
		gpio_pin_set_raw(amp_mute_gpio.port, amp_mute_gpio.pin, mute_level);
		return;
	}

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
