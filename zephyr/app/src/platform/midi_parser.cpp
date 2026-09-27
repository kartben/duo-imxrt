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

static uint8_t data_bytes_for_common(uint8_t status)
{
	switch (status) {
	case midi::TimeCodeQuarterFrame:
	case midi::SongSelect:
		return 1;
	case midi::SongPosition:
		return 2;
	default:
		return 0;
	}
}

/* The real time messages the Arduino library passes through. */
static bool is_thru_realtime(uint8_t status)
{
	switch (status) {
	case midi::Clock:
	case midi::Start:
	case midi::Continue:
	case midi::Stop:
	case midi::ActiveSensing:
	case midi::SystemReset:
		return true;
	default:
		return false;
	}
}

void MidiParser::pass_thru(const uint8_t *bytes, size_t len)
{
	if (thru) {
		thru(bytes, len);
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
		if (is_thru_realtime(byte)) {
			pass_thru(&byte, 1);
		}
		dispatch_realtime(byte);
		return;
	}

	if (byte & 0x80) {
		if (byte == midi::SystemExclusive) {
			in_sysex = true;
			sysex_overflow = false;
			sysex_len = 0;
			sysex[sysex_len++] = byte;
			return;
		}

		if (byte == midi::SystemExclusiveEnd) {
			if (in_sysex) {
				in_sysex = false;

				if (sysex_len < SYSEX_MAX) {
					sysex[sysex_len++] = byte;
				} else {
					sysex_overflow = true;
				}

				if (!sysex_overflow) {
					pass_thru(sysex, sysex_len);
				}

				if (callbacks.sysex) {
					callbacks.sysex(sysex, sysex_len);
				}
			}
			return;
		}

		/* Any other status byte ends an unterminated sysex. */
		in_sysex = false;
		data_count = 0;

		if (byte < 0xf0) {
			running_status = byte;
			common_status = 0;
			data_expected = data_bytes_for(byte);
		} else {
			/* System common: not used by the DUO, but passed
			 * through, and it cancels running status.
			 */
			running_status = 0;
			common_status = byte;
			data_expected = data_bytes_for_common(byte);

			if (data_expected == 0) {
				if (byte == midi::TuneRequest) {
					pass_thru(&byte, 1);
				}
				common_status = 0;
			}
		}

		return;
	}

	if (in_sysex) {
		if (sysex_len < SYSEX_MAX) {
			sysex[sysex_len++] = byte;
		} else {
			sysex_overflow = true;
		}
		return;
	}

	if (data_expected == 0) {
		return;
	}

	data[data_count++] = byte;

	if (data_count < data_expected) {
		return;
	}

	data_count = 0;

	const uint8_t status = running_status ? running_status : common_status;
	const uint8_t message[] = {status, data[0], data[1]};

	pass_thru(message, 1 + data_expected);

	if (running_status) {
		dispatch_voice(channel);
	} else {
		/* System common messages do not repeat. */
		common_status = 0;
		data_expected = 0;
	}
}
