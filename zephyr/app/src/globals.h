/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from brains2/apps/duo/globals.h. The globals here are the ones the
 * shared sequencer, pitch and MIDI code in shared/duo/ reaches for by name.
 */

#pragma once

#include "compat/duo_compat.h"

#define VERSION "1.2.0-beta1"
static const uint8_t FIRMWARE_VERSION[] = {1, 2, 0};

int MIDI_CHANNEL = 1;

/* Musical settings */
static const uint8_t SCALE[] = {49, 51, 54, 56, 58, 61, 63, 66, 68, 70};
static const uint8_t SCALE_OFFSET_FROM_C3[]{1, 3, 6, 8, 10, 13, 15, 18, 20, 22};

#define HIGH_SAMPLE_RATE 44100.0f
#define LOW_SAMPLE_RATE 2489.0f

/* Sequencer and voice state shared across the modules below. */
float osc_saw_frequency = 0.0f;
float osc_pulse_frequency = 0.0f;
float osc_pulse_target_frequency = 0.0f;
float osc_saw_target_frequency = 0.0f;
uint8_t osc_pulse_midi_note = 0;
int transpose = 0;
bool random_flag = 0;
bool dfu_flag = 0;
bool in_setup = true;

void note_on(uint8_t midi_note, uint8_t velocity, bool enabled);
void note_off();
void enter_dfu();

bool power_flag = true;

#include "shared/duo/synth_params.h"
synth_parameters synth;

void sequencer_note_on(uint8_t midi_note, uint8_t velocity)
{
	note_on(midi_note + transpose, velocity, true);
}

#include "shared/duo/seq.h"

void sequencer_randomize_step_offset(Sequencer::Sequencer &seq)
{
	const uint8_t offset = seq.get_step_offset() + random(1, (Sequencer::NUM_STEPS - 2));

	seq.set_step_offset(offset);
}

void sequencer_on_running_advance(Sequencer::Sequencer &seq)
{
	if (random_flag) {
		sequencer_randomize_step_offset(seq);
	} else {
		seq.set_step_offset(0);
	}
}

Sequencer::Sequencer sequencer(Sequencer::Output::Callbacks{.note_on = sequencer_note_on,
							    .note_off = note_off},
			       sequencer_on_running_advance);
