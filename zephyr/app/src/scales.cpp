/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scales.h"

namespace duo {

/* Ordered as the step buttons that select them at power-on, 1 to 8. */
const Scale SCALES[SCALE_COUNT] = {
	/* C# D# F# G# A#: the legacy keyboard. */
	{"pentatonic", 5, {0, 2, 5, 7, 9}},
	{"major", 7, {0, 2, 4, 5, 7, 9, 11}},
	{"minor", 7, {0, 2, 3, 5, 7, 8, 10}},
	{"dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
	{"mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
	{"harmonic minor", 7, {0, 2, 3, 5, 7, 8, 11}},
	{"blues", 6, {0, 3, 5, 6, 7, 10}},
	{"chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};

static uint8_t selected = DEFAULT_SCALE;

/* Division rounding towards minus infinity, for positions below the root. */
static int floor_div(int a, int b)
{
	return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

int scale_note(const Scale &scale, int position)
{
	const int octave = floor_div(position, scale.length);
	const int degree = position - octave * scale.length;

	return SCALE_ROOT + 12 * octave + scale.intervals[degree];
}

bool scale_position(const Scale &scale, int note, int *position)
{
	const int offset = note - SCALE_ROOT;
	const int octave = floor_div(offset, 12);
	const int semitone = offset - octave * 12;

	for (int degree = 0; degree < scale.length; degree++) {
		if (scale.intervals[degree] == semitone) {
			*position = octave * scale.length + degree;
			return true;
		}
	}

	return false;
}

uint8_t scale_remap(const Scale &from, const Scale &to, uint8_t note)
{
	int position;

	if (!scale_position(from, note, &position)) {
		return note;
	}

	const int remapped = scale_note(to, position);

	/*
	 * Far from the root the scales drift apart by octaves. A note that
	 * would land outside MIDI's range stays where it is; so does one that
	 * would land on 0, which note_off() takes to mean no note is playing.
	 */
	if (remapped < 1 || remapped > 127) {
		return note;
	}

	return (uint8_t)remapped;
}

const Scale &current_scale()
{
	return SCALES[selected];
}

uint8_t current_scale_index()
{
	return selected;
}

bool select_scale(uint8_t index)
{
	if (index >= SCALE_COUNT || index == selected) {
		return false;
	}

	selected = index;

	return true;
}

} /* namespace duo */
