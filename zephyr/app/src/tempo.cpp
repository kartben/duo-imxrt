/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal clock generator, from brains2/apps/duo/tempo.cpp.
 *
 * The tempo mapping and the accumulator design are the original's. What differs
 * is the timebase: the accumulator counts microseconds, and the legacy code fed
 * it from millis(), so every tick landed on a millisecond boundary - 0.83 ms of
 * jitter at 120 BPM, exported on the MIDI clock and the sync jack. It now reads
 * micros() directly. The arithmetic itself lives in tempo_clock.cpp so that it
 * can be tested without the MIDI and sync machinery TempoHandler pulls in.
 */

#include "compat/duo_compat.h"
#include "compat/lib/tempo.h"

#include "shared/duo/TempoHandler.h"

void Tempo::update_internal(TempoHandler &handler, const int potvalue)
{
	const uint32_t period_us = tempo_period_from_pot(potvalue);

	uint32_t ticks = tempo_advance(accum, last_micros, micros(), period_us);

	while (ticks--) {
		handler._previous_clock_time = micros();
		handler.trigger();
	}
}

void Tempo::reset()
{
	accum = 0;
	last_micros = micros();
}
