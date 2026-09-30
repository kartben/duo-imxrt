/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the held-note bookkeeping behind duophonic mode: which two notes
 * the oscillators get as notes come and go from the keyboard and MIDI.
 */

#include "held_notes.h"

#include <zephyr/ztest.h>

ZTEST_SUITE(duo_held_notes, NULL, NULL, NULL, NULL, NULL);

ZTEST(duo_held_notes, test_one_note_is_not_a_pair)
{
	duo::HeldNotes held;
	uint8_t low;
	uint8_t high;

	zassert_false(held.outer_pair(&low, &high), "nothing held, no pair");

	held.press(60);
	zassert_equal(held.count(), 1);
	zassert_false(held.outer_pair(&low, &high), "one note plays on both oscillators");
}

ZTEST(duo_held_notes, test_the_pair_is_the_lowest_and_highest)
{
	duo::HeldNotes held;
	uint8_t low;
	uint8_t high;

	/* Pressed in no particular order, as fingers do. */
	held.press(64);
	held.press(49);
	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(low, 49);
	zassert_equal(high, 64);

	/* A note inside the pair changes nothing; one outside it widens it. */
	held.press(56);
	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(low, 49);
	zassert_equal(high, 64);

	held.press(73);
	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(high, 73, "the new top note should take the second oscillator");

	/* Letting go of the top note hands its oscillator to the next one down. */
	held.release(73);
	held.release(64);
	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(low, 49);
	zassert_equal(high, 56);
}

ZTEST(duo_held_notes, test_a_note_held_twice_counts_once)
{
	duo::HeldNotes held;
	uint8_t low;
	uint8_t high;

	/* The same note from a key and over MIDI is one note, not a pair. */
	held.press(61);
	held.press(61);
	zassert_equal(held.count(), 1);
	zassert_false(held.outer_pair(&low, &high));

	held.press(68);
	held.release(61);
	zassert_equal(held.count(), 2, "61 is still held by its other source");
	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(low, 61);

	held.release(61);
	zassert_equal(held.count(), 1);
}

ZTEST(duo_held_notes, test_stray_releases_are_ignored)
{
	duo::HeldNotes held;

	/* All Notes Off can empty the list while keys are still down. */
	held.press(60);
	held.press(67);
	held.release_all();
	zassert_equal(held.count(), 0);

	held.release(60);
	held.release(67);
	held.release(200);
	zassert_equal(held.count(), 0, "releasing what is not held must not underflow");

	held.press(200);
	zassert_equal(held.count(), 0, "notes outside MIDI's range are ignored");

	held.press(0);
	held.press(127);

	uint8_t low;
	uint8_t high;

	zassert_true(held.outer_pair(&low, &high));
	zassert_equal(low, 0);
	zassert_equal(high, 127, "the whole MIDI range counts");
}
