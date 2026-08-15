/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Analog clock sync jacks. Same interface as brains2/core/lib/sync.h so that
 * shared/duo/TempoHandler.h compiles unchanged.
 */

#pragma once

#include <cstdint>

namespace Sync {

void init();

/* Returns HIGH once for each pulse seen on the input jack since the last call. */
uint32_t read();

void write(uint8_t value);

/* True while a cable is plugged into the sync input jack. */
bool detect();

} /* namespace Sync */
