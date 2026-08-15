/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FastLED.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_leds, CONFIG_DUO_LOG_LEVEL);

FastLED_ FastLED;

static const struct device *const strip = DEVICE_DT_GET(DT_CHOSEN(zephyr_led_strip));

/*
 * The DUO addresses one pixel more than the panel physically has; the legacy
 * firmware uses the extra one as a loopback target during production test.
 * Only the fitted pixels are ever pushed to the strip.
 */
static struct led_rgb scratch[DT_PROP(DT_CHOSEN(zephyr_led_strip), chain_length)];

void FastLED_::show()
{
	if (pixels == nullptr || !device_is_ready(strip)) {
		return;
	}

	size_t count = strip_length();

	for (size_t i = 0; i < count; i++) {
		/*
		 * Brightness and the per-channel white-balance correction are
		 * folded together the same way the legacy driver does it: two
		 * 8-bit scalings applied to each channel.
		 */
		scratch[i].r = (uint8_t)((pixels[i].r * brightness * correction.r) >> 16);
		scratch[i].g = (uint8_t)((pixels[i].g * brightness * correction.g) >> 16);
		scratch[i].b = (uint8_t)((pixels[i].b * brightness * correction.b) >> 16);
	}

	int ret = led_strip_update_rgb(strip, scratch, count);

	if (ret < 0) {
		LOG_WRN("led strip update failed (%d)", ret);
	}
}

size_t FastLED_::strip_length() const
{
	size_t count = led_strip_length(strip);

	if (pixel_count >= 0 && (size_t)pixel_count < count) {
		count = pixel_count;
	}

	return MIN(count, ARRAY_SIZE(scratch));
}

void FastLED_::clear()
{
	if (pixels == nullptr) {
		return;
	}

	for (int i = 0; i < pixel_count; i++) {
		pixels[i] = CRGB::Black;
	}
}

void FastLED_::setBrightness(int value)
{
	brightness = (uint8_t)CLAMP(value, 0, 255);
}

void FastLED_::setCorrection(uint32_t value)
{
	correction = value;
}
