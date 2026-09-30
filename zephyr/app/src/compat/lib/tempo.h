/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Same interface as brains2/core/lib/tempo.h; the implementation lives in
 * src/tempo.cpp.
 */

#pragma once

#include <cstdint>

class TempoHandler;

/*
 * Maps the TEMPO pot to the tick period in microseconds. Split out of
 * Tempo::update_internal() so it can be tested without the MIDI and sync
 * machinery TempoHandler drags in.
 */
uint32_t tempo_period_from_pot(int potvalue);

/*
 * Advances the clock to `now_us` and returns how many ticks fell due, leaving
 * the remainder in `accum` so the tick rate does not drift, and `last_us` at
 * `now_us`.
 *
 * The elapsed time is an unsigned difference, so this stays correct across the
 * microsecond counter's 32 bit wrap every ~71 minutes. It is clamped internally
 * so a pathological gap cannot spin the loop, but ordinary catch-up is
 * deliberately unbounded: that is what keeps the tempo from drifting when a
 * poll arrives late.
 */
uint32_t tempo_advance(uint32_t &accum, uint32_t &last_us, uint32_t now_us, uint32_t period_us);

struct Tempo {
	void update_internal(TempoHandler &handler, const int potvalue);
	void reset();

private:
	uint32_t accum = 0;
	/*
	 * Microseconds, not milliseconds. The accumulator has always been in
	 * microseconds; feeding it from millis() quantised every tick onto a
	 * millisecond boundary, which at 120 BPM is 4% of a tick of jitter on
	 * the MIDI clock and the sync jack.
	 */
	uint32_t last_micros = 0;
};
