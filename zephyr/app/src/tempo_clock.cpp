/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The arithmetic behind the internal clock generator: turning the TEMPO pot
 * into a tick period, and turning elapsed time into ticks.
 *
 * This is kept apart from tempo.cpp so it can be tested on the host. The rest
 * of the clock reaches TempoHandler, which pulls in the MIDI and sync
 * transports; the part that decides *when* a tick falls needs none of that.
 */

#include "compat/duo_compat.h"
#include "compat/lib/tempo.h"

/*
 * Microseconds per 24 PPQN tick: 60e6 us per minute / bpm / 24 ticks. The
 * legacy spelling of this was BPM_TO_MILLIS, which was never milliseconds.
 */
#define BPM_TO_TICK_US(bpm) (2500000 / bpm)

/*
 * Longest gap one update will account for. The catch-up loop below is what
 * keeps the tempo accurate across a late poll, so it must not be capped in
 * normal operation - dropping ticks would make the DUO drift slow after every
 * hiccup. This only bounds the pathological case, and at the fastest tempo it
 * still allows a couple of hundred iterations.
 */
#define TEMPO_MAX_ELAPSED_US 1000000U

uint32_t tempo_period_from_pot(const int potvalue)
{
	if (potvalue < 128) {
		return map(potvalue, 0, 128, BPM_TO_TICK_US(30), BPM_TO_TICK_US(60));
	}

	if (potvalue < 895) {
		return map(potvalue, 128, 895, BPM_TO_TICK_US(60), BPM_TO_TICK_US(200));
	}

	/* For Toon: 603 BPM in gives 600 BPM out */
	return map(potvalue, 895, 1023, BPM_TO_TICK_US(200), BPM_TO_TICK_US(603));
}

uint32_t tempo_advance(uint32_t &accum, uint32_t &last_us, const uint32_t now_us,
		       const uint32_t period_us)
{
	/*
	 * Unsigned, so the counter wrapping past 2^32 gives the right answer
	 * rather than a huge one. The legacy code guarded this with
	 * `if (cur >= last)` and silently dropped the interval that straddled
	 * the wrap.
	 */
	const uint32_t elapsed = now_us - last_us;
	uint32_t ticks = 0;

	last_us = now_us;

	if (period_us == 0) {
		return 0;
	}

	accum += elapsed > TEMPO_MAX_ELAPSED_US ? TEMPO_MAX_ELAPSED_US : elapsed;

	while (accum >= period_us) {
		accum -= period_us;
		ticks++;
	}

	return ticks;
}
