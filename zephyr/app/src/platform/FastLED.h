/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * FastLED-shaped facade over Zephyr's led_strip API, so that the DUO panel
 * code reads the same as it does in the legacy firmware. Like the adapter it
 * replaces (brains2/core/adapters/fast_led), addLeds() adopts the caller's
 * pixel array rather than appending to a chain, and the template parameters
 * are ignored: the strip type and data pin come from devicetree.
 */

#pragma once

#include "pixels.h"

#include <cstddef>
#include <cstdint>

struct FastLED_ {
	template <int LedType, int LedData, int ColorOrder>
	void addLeds(CRGB *raw_pixels, int count)
	{
		pixels = raw_pixels;
		pixel_count = count;
	}

	void show();
	void clear();
	void setBrightness(int brightness);
	void setCorrection(uint32_t correction);

private:
	/* Number of pixels actually pushed to the strip. */
	size_t strip_length() const;

	CRGB *pixels = nullptr;
	int pixel_count = 0;
	uint8_t brightness = 255;
	CRGB correction = 0xFFFFFF;
};

extern FastLED_ FastLED;
