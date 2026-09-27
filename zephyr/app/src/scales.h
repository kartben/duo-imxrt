/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keyboard scales.
 *
 * The legacy firmware hard-wires the ten keys to one pentatonic scale (the
 * black keys from C#3). Here that is the first of eight scales. Every scale
 * starts on the same lowest key, C#3, so the arrow buttons transpose all of
 * them the same way they always did, and the default is note for note the
 * legacy keyboard.
 *
 * Notes are addressed by their position in the scale: position 0 is the root,
 * 1 the next note up, and so on across octaves (and below zero downwards).
 * Keys 0 to 9 play positions 0 to 9.
 */

#pragma once

#include <cstdint>

namespace duo {

struct Scale {
	const char *name;
	/* Notes per octave. */
	uint8_t length;
	/* Semitones above the root, ascending, the first one 0. */
	uint8_t intervals[12];
};

static const uint8_t SCALE_ROOT = 49; /* C#3 */
static const uint8_t SCALE_COUNT = 8;
static const uint8_t KEYBOARD_KEYS = 10;

/* The DUO's own pentatonic, the default. */
static const uint8_t DEFAULT_SCALE = 0;

extern const Scale SCALES[SCALE_COUNT];

/* The note at `position` in `scale`, which may be outside the MIDI range. */
int scale_note(const Scale &scale, int position);

/* Where `note` sits in `scale`; false if the scale does not contain it. */
bool scale_position(const Scale &scale, int note, int *position);

/*
 * The note that takes the place of `note` when switching from one scale to
 * another: the one at the same position, so that a pattern keeps its shape.
 * Notes that are not in `from`, or whose counterpart would fall outside 1 to
 * 127, are left alone.
 */
uint8_t scale_remap(const Scale &from, const Scale &to, uint8_t note);

/* The scale the keyboard currently plays. */
const Scale &current_scale();
uint8_t current_scale_index();

/* Selects scale `index`; false if it does not exist or is already selected. */
bool select_scale(uint8_t index);

/*
 * The notes under the ten keys, in the current scale: `SCALE[key]`. Stands in
 * for the legacy firmware's `const uint8_t SCALE[]` array, so the shared code
 * that indexes it keeps working.
 */
struct ScaleKeys {
	uint8_t operator[](int key) const
	{
		return (uint8_t)scale_note(current_scale(), key);
	}
};

} /* namespace duo */
