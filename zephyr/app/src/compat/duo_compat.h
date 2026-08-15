/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The sequencer, tempo, pitch and MIDI logic in shared/duo/ is shared with the
 * Brains 1 firmware and is compiled here unmodified. It expects a handful of
 * Arduino-flavoured helpers, which this header provides on top of Zephyr.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include <cstdint>

typedef uint8_t byte;
typedef bool boolean;

#ifndef LOW
#define LOW 0
#endif
#ifndef HIGH
#define HIGH 1
#endif

static inline uint32_t millis(void)
{
	return k_uptime_get_32();
}

static inline uint32_t micros(void)
{
	return (uint32_t)(k_ticks_to_us_floor64(k_uptime_ticks()));
}

static inline void delay(uint32_t ms)
{
	k_msleep(ms);
}

static inline void delayMicroseconds(uint32_t us)
{
	k_busy_wait(us);
}

/* Arduino's map(): integer rescale, deliberately truncating like the original. */
static inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
	if (in_max == in_min) {
		return out_min;
	}

	return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static inline long random(long max)
{
	if (max <= 0) {
		return 0;
	}

	return (long)(sys_rand32_get() % (uint32_t)max);
}

static inline long random(long min, long max)
{
	if (max <= min) {
		return min;
	}

	return min + random(max - min);
}

static inline void randomSeed(unsigned long seed)
{
	/* Zephyr seeds its own entropy source; kept so shared code compiles. */
	ARG_UNUSED(seed);
}

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
