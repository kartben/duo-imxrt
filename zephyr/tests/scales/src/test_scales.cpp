/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keyboard scales: the default has to be exactly the legacy keyboard, every
 * scale has to stay inside the range the synth voice can play, and switching
 * scales has to carry a pattern across without losing its shape.
 */

#include "scales.h"

#include <zephyr/ztest.h>

using namespace duo;

/* brains2/apps/duo/globals.h */
static const uint8_t LEGACY_SCALE[] = {49, 51, 54, 56, 58, 61, 63, 66, 68, 70};

/* shared/duo/MidiFunctions.h clamps to this; transpose reaches +24. */
static const int MIDI_HIGHEST_NOTE = 94;
static const int MAX_TRANSPOSE = 24;

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);

	select_scale(DEFAULT_SCALE);
}

ZTEST_SUITE(duo_scales, NULL, NULL, reset, NULL, NULL);

ZTEST(duo_scales, test_default_is_the_legacy_keyboard)
{
	const ScaleKeys keys;

	zassert_equal(current_scale_index(), DEFAULT_SCALE);
	zassert_equal(ARRAY_SIZE(LEGACY_SCALE), KEYBOARD_KEYS);

	for (int key = 0; key < KEYBOARD_KEYS; key++) {
		zassert_equal(keys[key], LEGACY_SCALE[key], "key %d", key);
	}
}

ZTEST(duo_scales, test_scales_are_well_formed)
{
	for (int s = 0; s < SCALE_COUNT; s++) {
		const Scale &scale = SCALES[s];

		zassert_true(scale.length > 0 && scale.length <= 12, "%s", scale.name);
		zassert_equal(scale.intervals[0], 0, "%s must start on the root", scale.name);

		for (int i = 1; i < scale.length; i++) {
			zassert_true(scale.intervals[i] > scale.intervals[i - 1] &&
					     scale.intervals[i] < 12,
				     "%s interval %d", scale.name, i);
		}
	}
}

ZTEST(duo_scales, test_every_scale_starts_on_the_same_key)
{
	for (int s = 0; s < SCALE_COUNT; s++) {
		zassert_equal(scale_note(SCALES[s], 0), LEGACY_SCALE[0], "%s", SCALES[s].name);
	}
}

/* Keys go up, and stay playable at full transposition. */
ZTEST(duo_scales, test_keys_ascend_and_stay_in_range)
{
	for (int s = 0; s < SCALE_COUNT; s++) {
		for (int key = 1; key < KEYBOARD_KEYS; key++) {
			zassert_true(scale_note(SCALES[s], key) > scale_note(SCALES[s], key - 1),
				     "%s key %d", SCALES[s].name, key);
		}

		zassert_true(scale_note(SCALES[s], KEYBOARD_KEYS - 1) + MAX_TRANSPOSE <=
				     MIDI_HIGHEST_NOTE,
			     "%s top key out of range", SCALES[s].name);
	}
}

ZTEST(duo_scales, test_position_is_the_inverse_of_note)
{
	for (int s = 0; s < SCALE_COUNT; s++) {
		for (int position = -30; position <= 30; position++) {
			const int note = scale_note(SCALES[s], position);
			int found;

			zassert_true(scale_position(SCALES[s], note, &found), "%s %d",
				     SCALES[s].name, position);
			zassert_equal(found, position, "%s note %d", SCALES[s].name, note);
		}
	}
}

ZTEST(duo_scales, test_octaves_repeat)
{
	for (int s = 0; s < SCALE_COUNT; s++) {
		const Scale &scale = SCALES[s];

		for (int position = -12; position < 12; position++) {
			zassert_equal(scale_note(scale, position + scale.length),
				      scale_note(scale, position) + 12, "%s %d", scale.name, position);
		}
	}
}

ZTEST(duo_scales, test_notes_outside_a_scale_are_not_found)
{
	int position;

	/* D3 is not one of the DUO's black keys. */
	zassert_false(scale_position(SCALES[DEFAULT_SCALE], 50, &position));

	/* Chromatic has every note. */
	for (int note = 0; note < 128; note++) {
		zassert_true(scale_position(SCALES[SCALE_COUNT - 1], note, &position));
	}
}

/* A step recorded from the n-th key plays the n-th key of the new scale. */
ZTEST(duo_scales, test_remap_follows_the_keys)
{
	for (int from = 0; from < SCALE_COUNT; from++) {
		for (int to = 0; to < SCALE_COUNT; to++) {
			for (int key = 0; key < KEYBOARD_KEYS; key++) {
				zassert_equal(scale_remap(SCALES[from], SCALES[to],
							  scale_note(SCALES[from], key)),
					      scale_note(SCALES[to], key), "%s -> %s key %d",
					      SCALES[from].name, SCALES[to].name, key);
			}
		}
	}
}

/* Going there and back gives the original pattern. */
ZTEST(duo_scales, test_remap_round_trip)
{
	for (int to = 0; to < SCALE_COUNT; to++) {
		for (int position = -20; position <= 20; position++) {
			const uint8_t note = scale_note(SCALES[DEFAULT_SCALE], position);
			const uint8_t there = scale_remap(SCALES[DEFAULT_SCALE], SCALES[to], note);

			zassert_equal(scale_remap(SCALES[to], SCALES[DEFAULT_SCALE], there), note,
				      "via %s", SCALES[to].name);
		}
	}
}

/* Notes that came in over MIDI from outside the scale are left alone. */
ZTEST(duo_scales, test_remap_leaves_foreign_notes)
{
	zassert_equal(scale_remap(SCALES[DEFAULT_SCALE], SCALES[1], 50), 50);
}

/*
 * Far from the root, a note's counterpart can fall outside MIDI. It stays put
 * rather than being clamped: clamping onto note 0 would play a note that
 * note_off() never releases, since it takes 0 to mean nothing is playing.
 */
ZTEST(duo_scales, test_remap_never_leaves_midi_range_or_lands_on_zero)
{
	const Scale &chromatic = SCALES[SCALE_COUNT - 1];

	/* The top of MIDI in chromatic is far beyond it in a pentatonic. */
	zassert_equal(scale_remap(chromatic, SCALES[DEFAULT_SCALE], 127), 127);
	zassert_equal(scale_remap(chromatic, SCALES[DEFAULT_SCALE], 28), 28);

	for (int from = 0; from < SCALE_COUNT; from++) {
		for (int to = 0; to < SCALE_COUNT; to++) {
			for (int note = 1; note < 128; note++) {
				const uint8_t remapped = scale_remap(SCALES[from], SCALES[to], note);

				zassert_true(remapped >= 1 && remapped <= 127, "%s -> %s: %d -> %d",
					     SCALES[from].name, SCALES[to].name, note, remapped);
			}
		}
	}
}

ZTEST(duo_scales, test_selection)
{
	const ScaleKeys keys;

	zassert_false(select_scale(DEFAULT_SCALE), "already selected");
	zassert_false(select_scale(SCALE_COUNT), "no such scale");
	zassert_equal(current_scale_index(), DEFAULT_SCALE);

	zassert_true(select_scale(1));
	zassert_equal(current_scale_index(), 1);
	zassert_equal(keys[2], scale_note(SCALES[1], 2));

	/* Major from C#: C# D# F. */
	zassert_equal(keys[2], 53);
}

/* The CC 3 zones each select one scale, and cover the whole value range. */
ZTEST(duo_scales, test_cc_zones)
{
	for (int value = 0; value < 128; value++) {
		const int index = value / (128 / SCALE_COUNT);

		zassert_true(index >= 0 && index < SCALE_COUNT, "value %d", value);
	}

	zassert_equal(127 / (128 / SCALE_COUNT), SCALE_COUNT - 1);
}
