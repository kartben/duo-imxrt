/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The notes held down right now, from the keyboard and over MIDI, for
 * duophonic mode: with two or more held, the lowest and the highest play on
 * the two oscillators.
 *
 * The sequencer keeps its own list of held notes for the arpeggiator, but in
 * code shared with the Brains 1 firmware and out of reach, so the port keeps
 * this one alongside it.
 */

#pragma once

#include <cstdint>

namespace duo {

class HeldNotes {
public:
	static constexpr uint8_t NOTES = 128;

	void press(uint8_t note)
	{
		if (note < NOTES && holders[note]++ == 0) {
			distinct++;
		}
	}

	void release(uint8_t note)
	{
		if (note < NOTES && holders[note] > 0 && --holders[note] == 0) {
			distinct--;
		}
	}

	void release_all()
	{
		for (uint8_t &h : holders) {
			h = 0;
		}
		distinct = 0;
	}

	/* How many different notes are held; a key and MIDI holding the same one count once. */
	uint8_t count() const
	{
		return distinct;
	}

	/*
	 * The lowest and the highest note held, if at least two different notes
	 * are; otherwise false.
	 */
	bool outer_pair(uint8_t *lowest, uint8_t *highest) const
	{
		if (distinct < 2) {
			return false;
		}

		uint8_t low = 0;
		uint8_t high = NOTES - 1;

		while (holders[low] == 0) {
			low++;
		}
		while (holders[high] == 0) {
			high--;
		}

		*lowest = low;
		*highest = high;
		return true;
	}

private:
	/* How many sources hold each note: a key and MIDI can hold the same one. */
	uint8_t holders[NOTES] = {};
	uint8_t distinct = 0;
};

} /* namespace duo */
