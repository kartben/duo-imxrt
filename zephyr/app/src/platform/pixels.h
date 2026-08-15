/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The small slice of the FastLED CRGB type that the DUO panel code uses. The
 * legacy firmware pulls in FastLED's pixeltypes.h plus lib8tion.h for this;
 * only the handful of operations below are actually exercised.
 */

#pragma once

#include <cstdint>

struct CRGB {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;

	CRGB() = default;

	constexpr CRGB(uint8_t red, uint8_t green, uint8_t blue) : r(red), g(green), b(blue)
	{
	}

	/* Lets colour tables be written as 0xRRGGBB literals. */
	constexpr CRGB(uint32_t code)
		: r((code >> 16) & 0xff), g((code >> 8) & 0xff), b(code & 0xff)
	{
	}

	CRGB &operator=(uint32_t code)
	{
		r = (code >> 16) & 0xff;
		g = (code >> 8) & 0xff;
		b = code & 0xff;
		return *this;
	}

	bool operator==(const CRGB &other) const
	{
		return r == other.r && g == other.g && b == other.b;
	}

	/* Scales the colour down, keeping its hue. 0 leaves it untouched. */
	CRGB &fadeLightBy(uint8_t amount)
	{
		const uint16_t scale = 255 - amount;

		r = (uint8_t)((r * scale) >> 8);
		g = (uint8_t)((g * scale) >> 8);
		b = (uint8_t)((b * scale) >> 8);
		return *this;
	}

	enum : uint32_t {
		Black = 0x000000,
		Blue = 0x0000ff,
		Teal = 0x008080,
		White = 0xffffff,
	};
};

/* Linear crossfade; amount_of_p2 runs 0 (all p1) to 255 (all p2). */
static inline CRGB blend(const CRGB &p1, const CRGB &p2, int amount_of_p2)
{
	if (amount_of_p2 < 0) {
		amount_of_p2 = 0;
	} else if (amount_of_p2 > 255) {
		amount_of_p2 = 255;
	}

	const int amount_of_p1 = 255 - amount_of_p2;

	return CRGB((uint8_t)((p1.r * amount_of_p1 + p2.r * amount_of_p2) >> 8),
		    (uint8_t)((p1.g * amount_of_p1 + p2.g * amount_of_p2) >> 8),
		    (uint8_t)((p1.b * amount_of_p1 + p2.b * amount_of_p2) >> 8));
}
