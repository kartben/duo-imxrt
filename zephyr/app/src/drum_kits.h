/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The sounds each drum pad can play, one per keyboard key. The samples are
 * synthesised at build time by drumkit/drumkit.py, which generates the
 * definitions below, and play straight out of flash.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace duo {

/* A sampled drum: 16 bit mono at 44.1 kHz. */
struct DrumSample {
	const int16_t *data;
	uint32_t length;
};

/*
 * One sound per keyboard key, in key order. The first is the DUO's own
 * synthesised drum, which the voice renders itself, so it has no sample
 * (data is nullptr).
 */
static constexpr size_t PAD_SOUNDS = 10;

extern const DrumSample KICK_PAD_SOUNDS[PAD_SOUNDS];
extern const DrumSample HAT_PAD_SOUNDS[PAD_SOUNDS];

} /* namespace duo */
