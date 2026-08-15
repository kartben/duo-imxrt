/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "midi_parser.h"

static uint8_t data_bytes_for(uint8_t status)
{
	switch (status & 0xf0) {
	case midi::ProgramChange:
	case midi::AfterTouchChannel:
		return 1;
	case midi::NoteOff:
	case midi::NoteOn:
	case midi::AfterTouchPoly:
	case midi::ControlChange:
	case midi::PitchBend:
		return 2;
	default:
		return 0;
	}
}

void MidiParser::dispatch_realtime(uint8_t status)
{
	switch (status) {
	case midi::Clock:
		if (callbacks.clock) {
			callbacks.clock();
		}
		break;
	case midi::Start:
		if (callbacks.start) {
			callbacks.start();
		}
		break;
	case midi::Continue:
		if (callbacks.cont) {
			callbacks.cont();
		}
		break;
	case midi::Stop:
		if (callbacks.stop) {
			callbacks.stop();
		}
		break;
	default:
		break;
	}
}

void MidiParser::dispatch_voice(uint8_t channel)
{
	const uint8_t type = running_status & 0xf0;
	const uint8_t msg_channel = (running_status & 0x0f) + 1;

	if (msg_channel != channel) {
		return;
	}

	switch (type) {
	case midi::NoteOn:
		/* A note on with zero velocity is a note off. */
		if (data[1] == 0) {
			if (callbacks.note_off) {
				callbacks.note_off(msg_channel, data[0], data[1]);
			}
		} else if (callbacks.note_on) {
			callbacks.note_on(msg_channel, data[0], data[1]);
		}
		break;
	case midi::NoteOff:
		if (callbacks.note_off) {
			callbacks.note_off(msg_channel, data[0], data[1]);
		}
		break;
	case midi::ControlChange:
		if (callbacks.cc) {
			callbacks.cc(msg_channel, data[0], data[1]);
		}
		break;
	default:
		break;
	}
}

void MidiParser::feed(uint8_t byte, uint8_t channel)
{
	/* Real time messages may appear anywhere, even inside a sysex. */
	if (byte >= midi::Clock) {
		dispatch_realtime(byte);
		return;
	}

	if (byte & 0x80) {
		if (byte == midi::SystemExclusive) {
			in_sysex = true;
			sysex_len = 0;
			sysex[sysex_len++] = byte;
			return;
		}

		if (byte == midi::SystemExclusiveEnd) {
			if (in_sysex) {
				in_sysex = false;

				if (sysex_len < SYSEX_MAX) {
					sysex[sysex_len++] = byte;
				}

				if (callbacks.sysex) {
					callbacks.sysex(sysex, sysex_len);
				}
			}
			return;
		}

		/* Any other status byte ends an unterminated sysex. */
		in_sysex = false;

		if (byte < 0xf0) {
			running_status = byte;
			data_expected = data_bytes_for(byte);
			data_count = 0;
		} else {
			/* System common: not used by the DUO, but it must
			 * still cancel running status.
			 */
			running_status = 0;
			data_expected = 0;
			data_count = 0;
		}

		return;
	}

	if (in_sysex) {
		if (sysex_len < SYSEX_MAX) {
			sysex[sysex_len++] = byte;
		}
		return;
	}

	if (running_status == 0 || data_expected == 0) {
		return;
	}

	data[data_count++] = byte;

	if (data_count >= data_expected) {
		data_count = 0;
		dispatch_voice(channel);
	}
}
