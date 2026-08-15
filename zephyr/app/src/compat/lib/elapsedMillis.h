/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal replacement for the PJRC elapsedMillis helper, providing just the
 * operations TempoHandler uses: assignment from an integer to restart the
 * timer, and comparison against a millisecond count.
 */

#pragma once

#include "../duo_compat.h"

class elapsedMillis {
public:
	elapsedMillis() : origin(millis())
	{
	}

	elapsedMillis(uint32_t value) : origin(millis() - value)
	{
	}

	operator uint32_t() const
	{
		return millis() - origin;
	}

	elapsedMillis &operator=(uint32_t value)
	{
		origin = millis() - value;
		return *this;
	}

private:
	uint32_t origin;
};
