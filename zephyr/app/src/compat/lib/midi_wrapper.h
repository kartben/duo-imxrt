/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same interface as brains2/core/lib/midi_wrapper.h. Messages are sent to, and
 * read from, both transports at once: the MIDI DIN jacks on LPUART1 and USB
 * MIDI.
 */

#pragma once

#include "../MIDI.h"

namespace MIDI {

typedef void(VoidCallback)();
typedef void(SyxCallback)(byte *data, unsigned length);
typedef void(NoteCallback)(byte channel, byte note, byte velocity);

struct Callbacks {
	NoteCallback *note_on;
	NoteCallback *note_off;
	VoidCallback *clock;
	VoidCallback *start;
	VoidCallback *cont;
	VoidCallback *stop;
	NoteCallback *cc;
	SyxCallback *sysex;
};

void init(const Callbacks &callbacks);
void read(byte channel);
void sendRealTime(midi::MidiType message);
void sendControlChange(byte cc, byte value, byte channel);
void sendNoteOn(byte inNoteNumber, byte inVelocity, byte inChannel);
void sendNoteOff(byte inNoteNumber, byte inVelocity, byte inChannel);
void sendSysEx(unsigned length, const byte *bytes);

} /* namespace MIDI */
