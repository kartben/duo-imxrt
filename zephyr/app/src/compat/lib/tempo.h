/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same interface as brains2/core/lib/tempo.h; the implementation lives in
 * src/tempo.cpp.
 */

#pragma once

#include <cstdint>

class TempoHandler;

struct Tempo {
	void update_internal(TempoHandler &handler, const int potvalue);
	void reset();

private:
	uint32_t accum = 0;
	uint32_t last_millis = 0;
};
