/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the internal clock generator's arithmetic: the TEMPO pot mapping
 * and the accumulator that turns elapsed microseconds into 24 PPQN ticks.
 *
 * The thing these are really pinning is jitter. The accumulator carries its
 * remainder, so the average tempo was always right; what was wrong was that
 * every individual tick landed on a whole millisecond, because the accumulator
 * counts microseconds but used to be fed from millis().
 */

#include "compat/lib/tempo.h"

#include <zephyr/ztest.h>

#include <cstdlib>

/* Tick period in microseconds for a given BPM, at 24 pulses per quarter note. */
static uint32_t period_for(int bpm)
{
	return (uint32_t)(60000000 / (bpm * 24));
}

ZTEST_SUITE(duo_tempo, NULL, NULL, NULL, NULL, NULL);

/* --- Pot mapping ------------------------------------------------------- */

ZTEST(duo_tempo, test_pot_maps_to_the_documented_tempo_range)
{
	/* The three piecewise bands from the legacy firmware. */
	const struct {
		int pot;
		int bpm;
	} points[] = {
		{0, 30}, {128, 60}, {895, 200}, {1023, 603},
	};

	for (const auto &p : points) {
		const uint32_t got = tempo_period_from_pot(p.pot);
		const uint32_t want = period_for(p.bpm);

		/* Integer division in the mapping, so allow a tick period LSB. */
		zassert_within((int32_t)got, (int32_t)want, 2,
			       "pot %d should be %d BPM (%u us), got %u us", p.pot, p.bpm,
			       want, got);
	}
}

ZTEST(duo_tempo, test_pot_is_monotonic)
{
	uint32_t previous = tempo_period_from_pot(0);

	for (int pot = 1; pot < 1024; pot++) {
		const uint32_t period = tempo_period_from_pot(pot);

		/* Faster tempo means a shorter period. */
		zassert_true(period <= previous, "pot %d went backwards: %u after %u", pot,
			     period, previous);
		previous = period;
	}
}

/* --- Accumulator ------------------------------------------------------- */

/*
 * Feeding the accumulator at a fixed poll rate should produce ticks whose
 * spacing never strays further than one poll interval from nominal. Driven
 * from millis() the spacing wandered by up to 0.83 ms at 120 BPM.
 */
ZTEST(duo_tempo, test_tick_spacing_stays_close_to_nominal)
{
	const int tempos[] = {90, 120, 174, 200};
	/* 100 us is the timebase resolution of micros() on this board. */
	const uint32_t poll_us = 100;

	for (int bpm : tempos) {
		const uint32_t period = period_for(bpm);
		uint32_t accum = 0;
		uint32_t last_us = 0;
		uint32_t now = 0;
		uint32_t last_tick = 0;
		int32_t worst = 0;
		int ticks = 0;

		/* Ten seconds of polling. */
		for (uint32_t i = 0; i < 10 * 1000000U / poll_us; i++) {
			now += poll_us;

			uint32_t due = tempo_advance(accum, last_us, now, period);

			while (due--) {
				if (ticks++ > 0) {
					const int32_t spacing = (int32_t)(now - last_tick);
					const int32_t error = spacing - (int32_t)period;

					if (abs(error) > abs(worst)) {
						worst = error;
					}
				}
				last_tick = now;
			}
		}

		zassert_true(ticks > 0, "%d BPM produced no ticks", bpm);
		zassert_true((uint32_t)abs(worst) <= poll_us,
			     "%d BPM jittered %d us, more than the %u us poll interval", bpm,
			     worst, poll_us);
	}
}

/*
 * The remainder carries between updates, so the tick count over a long run has
 * to match the tempo exactly - this is what stops the clock drifting when the
 * poll interval does not divide the tick period.
 */
ZTEST(duo_tempo, test_no_drift_over_a_minute)
{
	const int tempos[] = {90, 120, 174};
	/* Deliberately not a divisor of any tick period. */
	const uint32_t poll_us = 703;

	for (int bpm : tempos) {
		const uint32_t period = period_for(bpm);
		uint32_t accum = 0;
		uint32_t last_us = 0;
		uint32_t now = 0;
		uint32_t ticks = 0;

		for (uint32_t i = 0; i < 60 * 1000000U / poll_us; i++) {
			now += poll_us;
			ticks += tempo_advance(accum, last_us, now, period);
		}

		const uint32_t expected = 60 * 1000000U / period;

		zassert_within((int32_t)ticks, (int32_t)expected, 2,
			       "%d BPM produced %u ticks in a minute, expected %u", bpm, ticks,
			       expected);
	}
}

/* An irregular poll interval must not change how many ticks come out. */
ZTEST(duo_tempo, test_irregular_polling_does_not_lose_ticks)
{
	const uint32_t period = period_for(120);
	uint32_t accum = 0;
	uint32_t last_us = 0;
	uint32_t ticks = 0;
	uint32_t total = 0;
	uint32_t step = 1;

	while (total < 10 * 1000000U) {
		/* Poll intervals wandering between 1 us and 11 ms. */
		step = (step * 7919 + 13) % 11000 + 1;

		if (total + step > 10 * 1000000U) {
			step = 10 * 1000000U - total;
		}

		total += step;
		ticks += tempo_advance(accum, last_us, total, period);
	}

	zassert_within((int32_t)ticks, (int32_t)(10 * 1000000U / period), 2,
		       "irregular polling produced %u ticks", ticks);
}

/*
 * The microsecond counter is 32 bit and wraps every ~71 minutes. The elapsed
 * time is a modular difference, so a wrap should be invisible here; the legacy
 * code guarded with `if (cur >= last)` and silently dropped that interval.
 */
ZTEST(duo_tempo, test_counter_wrap_is_transparent)
{
	const uint32_t period = period_for(120);

	/*
	 * One step straddling the wrap, rather than many small ones either side
	 * of it: the whole interval has to be accounted for in a single call, so
	 * a guard that discards it cannot hide the loss in rounding.
	 */
	uint32_t accum = 0;
	uint32_t last_us = UINT32_MAX - 30000;
	const uint32_t now = 30000;
	/* now - last_us in modular arithmetic. */
	const uint32_t elapsed = 60001;

	const uint32_t ticks = tempo_advance(accum, last_us, now, period);

	zassert_equal(ticks, elapsed / period, "a wrap lost ticks: got %u, expected %u", ticks,
		      elapsed / period);
	zassert_equal(last_us, now, "the timestamp should advance across a wrap");

	/* And the remainder carries, so the next tick is not late either. */
	zassert_equal(accum, elapsed % period, "the wrap lost the accumulator remainder");
}

/* A pathological gap must not spin the catch-up loop for a long time. */
ZTEST(duo_tempo, test_long_stall_is_bounded)
{
	const uint32_t period = period_for(603);
	uint32_t accum = 0;
	uint32_t last_us = 0;

	const uint32_t ticks = tempo_advance(accum, last_us, UINT32_MAX, period);

	zassert_true(ticks > 0, "a stall should still produce ticks");
	zassert_true(ticks < 1000, "a stall produced %u ticks in one update", ticks);
}

/* Ordinary catch-up must not be capped, or the tempo drifts slow after a hiccup. */
ZTEST(duo_tempo, test_a_late_poll_catches_up)
{
	const uint32_t period = period_for(120);
	uint32_t accum = 0;
	uint32_t last_us = 0;

	/* One 50 ms hiccup, about two and a half ticks at 120 BPM. */
	const uint32_t ticks = tempo_advance(accum, last_us, 50000, period);

	zassert_equal(ticks, 50000 / period, "expected %u ticks after a 50 ms gap, got %u",
		      50000 / period, ticks);
}

ZTEST(duo_tempo, test_zero_period_is_ignored)
{
	uint32_t accum = 0;
	uint32_t last_us = 0;

	zassert_equal(tempo_advance(accum, last_us, 1000, 0), 0, "a zero period must not divide");
}
