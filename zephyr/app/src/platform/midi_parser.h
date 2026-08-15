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

#include <cstdint>

class MidiParser {
public:
	static const unsigned SYSEX_MAX = 64;

	void set_callbacks(const MIDI::Callbacks &cb)
	{
		callbacks = cb;
	}

	/* Feeds one received byte. `channel` is 1..16; voice messages on other
	 * channels are dropped, as the Arduino library does.
	 */
	void feed(uint8_t byte, uint8_t channel);

private:
	void dispatch_voice(uint8_t channel);
	void dispatch_realtime(uint8_t status);

	MIDI::Callbacks callbacks = {};
	uint8_t running_status = 0;
	uint8_t data[2] = {0, 0};
	uint8_t data_count = 0;
	uint8_t data_expected = 0;
	bool in_sysex = false;
	unsigned sysex_len = 0;
	uint8_t sysex[SYSEX_MAX] = {0};
};
