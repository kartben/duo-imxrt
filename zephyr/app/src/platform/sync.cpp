/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Analog clock sync jacks, ported from brains2/core/lib/sync.cpp.
 */

#include "../compat/duo_compat.h"
#include "../compat/lib/sync.h"
#include "pins.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_sync, CONFIG_DUO_LOG_LEVEL);

#define SYNC_NODE DT_NODELABEL(sync)

BUILD_ASSERT(DT_NODE_HAS_STATUS(SYNC_NODE, okay), "sync node is missing");

static const struct gpio_dt_spec sync_in = GPIO_DT_SPEC_GET(SYNC_NODE, in_gpios);
static const struct gpio_dt_spec sync_out = GPIO_DT_SPEC_GET(SYNC_NODE, out_gpios);

static struct gpio_callback sync_cb;

/* Set by the edge interrupt, consumed by Sync::read(). */
static volatile bool pulse_seen;

static void sync_pulse(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	pulse_seen = true;
}

namespace Sync {

void init()
{
	int ret;

	if (!gpio_is_ready_dt(&sync_in) || !gpio_is_ready_dt(&sync_out)) {
		LOG_ERR("sync GPIOs not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&sync_out, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("sync out configure failed (%d)", ret);
		return;
	}

	ret = gpio_pin_configure_dt(&sync_in, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("sync in configure failed (%d)", ret);
		return;
	}

	/*
	 * The jack is inverted by its input buffer, so a rising edge on the
	 * cable arrives here as a falling edge, exactly as in the legacy
	 * firmware.
	 */
	ret = gpio_pin_interrupt_configure_dt(&sync_in, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("sync in interrupt configure failed (%d)", ret);
		return;
	}

	gpio_init_callback(&sync_cb, sync_pulse, BIT(sync_in.pin));
	gpio_add_callback_dt(&sync_in, &sync_cb);
}

uint32_t read()
{
	if (pulse_seen) {
		pulse_seen = false;
		return HIGH;
	}

	return LOW;
}

void write(uint8_t value)
{
	gpio_pin_set_dt(&sync_out, value);
}

bool detect()
{
	return pinRead(SYNC_DETECT_PIN);
}

} /* namespace Sync */
