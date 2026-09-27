/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIDI 1.0 byte-stream parser. Replaces the Arduino MIDI Library the legacy
 * firmware links against; only the messages the DUO reacts to are decoded.
 *
 * One instance per transport, matching the legacy firmware's two independent
 * MidiInterface objects, so that running status on one link cannot be
 * corrupted by traffic on the other.
 */

#pragma once

#include "../compat/lib/midi_wrapper.h"

#include <cstddef>
#include <cstdint>

class MidiParser {
public:
	/* The Arduino library's default, which the legacy firmware uses. */
	static const unsigned SYSEX_MAX = 128;

	/* Receives each complete message, on any channel, for soft thru. */
	typedef void (*ThruHandler)(const uint8_t *bytes, size_t len);

	void set_callbacks(const MIDI::Callbacks &cb)
	{
		callbacks = cb;
	}

	/*
	 * Echoes everything received to `handler`, one whole message at a
	 * time, the way the Arduino library's Thru::Full mode (on by default
	 * for its serial transport) does in the legacy firmware. Messages are
	 * re-sent with their status byte, so running status is not preserved,
	 * and a sysex too long to buffer is not passed on.
	 */
	void set_thru(ThruHandler handler)
	{
		thru = handler;
	}

	/* Feeds one received byte. `channel` is 1..16; voice messages on other
	 * channels are dropped, as the Arduino library does.
	 */
	void feed(uint8_t byte, uint8_t channel);

private:
	void dispatch_voice(uint8_t channel);
	void dispatch_realtime(uint8_t status);
	void pass_thru(const uint8_t *bytes, size_t len);

	MIDI::Callbacks callbacks = {};
	ThruHandler thru = nullptr;
	/* Status of the message being assembled: a channel voice status, which
	 * is also the running status, or a system common one, which is not.
	 */
	uint8_t running_status = 0;
	uint8_t common_status = 0;
	uint8_t data[2] = {0, 0};
	uint8_t data_count = 0;
	uint8_t data_expected = 0;
	bool in_sysex = false;
	bool sysex_overflow = false;
	unsigned sysex_len = 0;
	uint8_t sysex[SYSEX_MAX] = {0};
};
