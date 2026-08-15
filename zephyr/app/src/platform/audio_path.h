/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same interface as brains2/core/lib/audio.h.
 */

#pragma once

namespace Audio {

void amp_init(void);
void amp_enable(void);
void amp_disable(void);
void headphone_enable(void);
void headphone_disable(void);

} /* namespace Audio */
