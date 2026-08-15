/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stand-in for the Arduino MIDI Library header that shared/duo/ includes.
 * Only the type and enumeration names the shared code actually references are
 * provided; the wire protocol itself is implemented natively in
 * platform/midi_serial.cpp and platform/midi_usb.cpp.
 */

#pragma once

#include "duo_compat.h"

namespace midi {

enum MidiType : uint8_t {
	InvalidType = 0x00,
	NoteOff = 0x80,
	NoteOn = 0x90,
	AfterTouchPoly = 0xA0,
	ControlChange = 0xB0,
	ProgramChange = 0xC0,
	AfterTouchChannel = 0xD0,
	PitchBend = 0xE0,
	SystemExclusive = 0xF0,
	TimeCodeQuarterFrame = 0xF1,
	SongPosition = 0xF2,
	SongSelect = 0xF3,
	TuneRequest = 0xF6,
	SystemExclusiveEnd = 0xF7,
	Clock = 0xF8,
	Start = 0xFA,
	Continue = 0xFB,
	Stop = 0xFC,
	ActiveSensing = 0xFE,
	SystemReset = 0xFF,
};

} /* namespace midi */
