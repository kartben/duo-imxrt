/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the DSP primitives that stand in for the Teensy Audio Library
 * objects. These pin down the behaviour the DUO voice depends on: envelope
 * timings, filter response, delay line offsets and so on.
 */

#include "dsp/dsp.h"

#include <zephyr/ztest.h>

#include <cmath>

static constexpr float SR = dsp::SAMPLE_RATE;

/* Samples in a given number of milliseconds, matching the DSP's own rounding. */
static int samples_ms(float ms)
{
	return (int)(ms * (SR / 1000.0f));
}

ZTEST_SUITE(dsp_primitives, NULL, NULL, NULL, NULL, NULL);

/* --- Envelope ---------------------------------------------------------- */

ZTEST(dsp_primitives, test_envelope_idle_is_silent)
{
	dsp::Envelope env;

	env.attack(10.0f);
	env.sustain(1.0f);

	for (int i = 0; i < 1000; i++) {
		zassert_equal(env.next(), 0.0f, "envelope must be silent before note on");
	}
}

ZTEST(dsp_primitives, test_envelope_attack_reaches_full_scale)
{
	dsp::Envelope env;

	env.attack(10.0f);
	env.decay(0.0f);
	env.sustain(1.0f);
	env.release(100.0f);
	env.note_on();

	float level = 0.0f;

	/*
	 * The shared integer envelope truncates its per-sample increment, so
	 * the attack lands slightly late; allow a little margin past nominal.
	 */
	for (int i = 0; i < samples_ms(10.0f) + 100; i++) {
		level = env.next();
	}

	zassert_within(level, 1.0f, 0.01f, "attack should reach full scale, got %f",
		       (double)level);
}

ZTEST(dsp_primitives, test_envelope_holds_at_sustain)
{
	dsp::Envelope env;

	env.attack(1.0f);
	env.decay(10.0f);
	env.sustain(0.5f);
	env.release(100.0f);
	env.note_on();

	float level = 0.0f;

	for (int i = 0; i < samples_ms(11.0f) + 200; i++) {
		level = env.next();
	}

	zassert_within(level, 0.5f, 0.02f, "should settle at the sustain level, got %f",
		       (double)level);

	/* And stay there. */
	for (int i = 0; i < 10000; i++) {
		level = env.next();
	}

	zassert_within(level, 0.5f, 0.02f, "sustain should hold indefinitely");
}

ZTEST(dsp_primitives, test_envelope_release_decays_to_silence)
{
	dsp::Envelope env;

	env.attack(1.0f);
	env.decay(0.0f);
	env.sustain(1.0f);
	env.release(100.0f);
	env.note_on();

	for (int i = 0; i < samples_ms(2.0f); i++) {
		env.next();
	}

	env.note_off();

	float level = 1.0f;

	/* Integer truncation makes the release run a few percent long. */
	for (int i = 0; i < samples_ms(100.0f) * 2; i++) {
		level = env.next();
	}

	zassert_equal(level, 0.0f, "release should reach exactly zero, got %f", (double)level);
}

/* --- Oscillator -------------------------------------------------------- */

ZTEST(dsp_primitives, test_oscillator_silent_at_zero_amplitude)
{
	dsp::Oscillator osc;

	osc.set_frequency(440.0f);
	osc.set_amplitude(0.0f);

	for (int i = 0; i < 1000; i++) {
		zassert_equal(osc.saw(), 0.0f);
	}
}

ZTEST(dsp_primitives, test_saw_frequency_matches_zero_crossings)
{
	dsp::Oscillator osc;

	/* 441 Hz at 44.1 kHz is exactly 100 samples per period. */
	osc.set_frequency(441.0f);
	osc.set_amplitude(1.0f);

	float prev = osc.saw();
	int rising = 0;

	for (int i = 0; i < 1000; i++) {
		const float cur = osc.saw();

		if (prev < 0.0f && cur >= 0.0f) {
			rising++;
		}
		prev = cur;
	}

	zassert_within((float)rising, 10.0f, 1.0f,
		       "expected ~10 periods in 1000 samples, counted %d", rising);
}

ZTEST(dsp_primitives, test_saw_stays_in_range)
{
	dsp::Oscillator osc;

	osc.set_frequency(441.0f);
	osc.set_amplitude(1.0f);

	for (int i = 0; i < 4410; i++) {
		const float v = osc.saw();

		zassert_true(fabsf(v) <= 1.05f, "saw left its range: %f", (double)v);
	}
}

/* The mean of a pulse wave is 2 * duty - 1, which is how pulse width is heard. */
ZTEST(dsp_primitives, test_pulse_width_sets_duty_cycle)
{
	const float widths[] = {0.5f, 0.75f};

	for (float width : widths) {
		dsp::Oscillator osc;
		float sum = 0.0f;

		osc.set_frequency(441.0f);
		osc.set_amplitude(1.0f);
		osc.set_pulse_width(width);

		for (int i = 0; i < 1000; i++) {
			sum += osc.pulse();
		}

		const float mean = sum / 1000.0f;
		const float expected = 2.0f * width - 1.0f;

		zassert_within(mean, expected, 0.1f,
			       "duty %f should give mean %f, got %f", (double)width,
			       (double)expected, (double)mean);
	}
}

/* --- State variable filter --------------------------------------------- */

ZTEST(dsp_primitives, test_filter_passes_dc)
{
	dsp::StateVariableFilter filter;

	filter.set_frequency(1000.0f);
	filter.set_resonance(0.7f);

	for (int i = 0; i < 5000; i++) {
		filter.process(1.0f);
	}

	zassert_within(filter.low(), 1.0f, 0.05f, "lowpass should pass DC, got %f",
		       (double)filter.low());
}

ZTEST(dsp_primitives, test_filter_rejects_high_frequencies)
{
	dsp::StateVariableFilter filter;
	float energy = 0.0f;

	filter.set_frequency(200.0f);
	filter.set_resonance(0.7f);

	/* Alternating samples are a signal at the Nyquist frequency. */
	for (int i = 0; i < 2000; i++) {
		filter.process(i % 2 ? 1.0f : -1.0f);

		if (i >= 1500) {
			energy += filter.low() * filter.low();
		}
	}

	const float rms = sqrtf(energy / 500.0f);

	zassert_true(rms < 0.1f, "Nyquist should be well attenuated, rms was %f", (double)rms);
}

ZTEST(dsp_primitives, test_filter_modulation_opens_the_cutoff)
{
	float rms[2];

	for (int pass = 0; pass < 2; pass++) {
		dsp::StateVariableFilter filter;
		float energy = 0.0f;

		filter.set_frequency(200.0f);
		filter.set_resonance(0.7f);
		filter.set_octave_control(4.0f);
		filter.set_modulation(pass == 0 ? 0.0f : 1.0f);

		for (int i = 0; i < 4410; i++) {
			/* A 2 kHz tone, between the closed and open cutoffs. */
			filter.process(sinf(2.0f * dsp::PI_F * 2000.0f * (float)i / SR));

			if (i >= 2205) {
				energy += filter.low() * filter.low();
			}
		}

		rms[pass] = sqrtf(energy / 2205.0f);
	}

	zassert_true(rms[1] > rms[0] * 2.0f,
		     "opening the cutoff by four octaves should pass much more: %f vs %f",
		     (double)rms[1], (double)rms[0]);
}

/* --- Bitcrusher -------------------------------------------------------- */

ZTEST(dsp_primitives, test_bitcrusher_transparent_at_full_rate)
{
	dsp::Bitcrusher crusher;

	crusher.set_sample_rate(SR);

	for (int i = 0; i < 100; i++) {
		const float in = (float)i / 100.0f;

		zassert_equal(crusher.process(in), in, "full rate should pass samples through");
	}
}

ZTEST(dsp_primitives, test_bitcrusher_holds_samples_at_reduced_rate)
{
	dsp::Bitcrusher crusher;
	float last = -1.0f;
	int changes = 0;

	/* A tenth of the sample rate should update once every ten samples. */
	crusher.set_sample_rate(SR / 10.0f);

	for (int i = 0; i < 100; i++) {
		const float out = crusher.process((float)i / 100.0f);

		if (out != last) {
			changes++;
			last = out;
		}
	}

	zassert_within((float)changes, 10.0f, 2.0f,
		       "expected ~10 updates in 100 samples, saw %d", changes);
}

/* --- Delay line -------------------------------------------------------- */

ZTEST(dsp_primitives, test_delay_line_returns_sample_after_delay)
{
	static int16_t buffer[100] = {};
	dsp::DelayLine<100> delay;
	const int expected_at = samples_ms(1.0f); /* 44 samples */

	delay.set_delay(1);

	for (int i = 0; i < 99; i++) {
		/* One impulse, then silence. */
		const float out = delay.process(i == 0 ? 1.0f : 0.0f, buffer);

		if (i == expected_at) {
			zassert_true(out > 0.99f, "impulse should reappear at sample %d, got %f",
				     expected_at, (double)out);
		} else {
			zassert_within(out, 0.0f, 0.001f,
				       "unexpected output at sample %d: %f", i, (double)out);
		}
	}
}

/* --- Fade -------------------------------------------------------------- */

ZTEST(dsp_primitives, test_fade_in_and_out)
{
	dsp::Fade fade;

	fade.fade_in(10);

	float level = 0.0f;

	for (int i = 0; i < samples_ms(10.0f) + 10; i++) {
		level = fade.next();
	}

	zassert_within(level, 1.0f, 0.001f, "fade in should reach unity, got %f", (double)level);

	fade.fade_out(10);

	for (int i = 0; i < samples_ms(10.0f) + 10; i++) {
		level = fade.next();
	}

	zassert_within(level, 0.0f, 0.001f, "fade out should reach zero, got %f", (double)level);
}

/* --- Peak detector ----------------------------------------------------- */

ZTEST(dsp_primitives, test_peak_detector_reads_and_clears)
{
	dsp::PeakDetector peak;

	peak.process(0.5f);
	peak.process(-0.8f);
	peak.process(0.3f);

	zassert_within(peak.read(), 0.8f, 0.001f, "peak should be the largest magnitude");
	zassert_equal(peak.read(), 0.0f, "reading should clear the peak");
}

/* --- Simple drum ------------------------------------------------------- */

ZTEST(dsp_primitives, test_simple_drum_sounds_then_stops)
{
	dsp::SimpleDrum drum;
	const int length = samples_ms(10.0f);
	float loudest = 0.0f;

	drum.set_length(10);
	drum.set_frequency(60.0f);
	drum.set_pitch_mod(4.0f);

	for (int i = 0; i < 100; i++) {
		zassert_equal(drum.next(), 0.0f, "drum must be silent before note on");
	}

	drum.note_on();

	for (int i = 0; i < length; i++) {
		/* Not MAX(): the macro would evaluate next() twice. */
		const float sample = fabsf(drum.next());

		if (sample > loudest) {
			loudest = sample;
		}
	}

	zassert_true(loudest > 0.1f, "drum should make a sound, peak was %f", (double)loudest);

	for (int i = 0; i < 1000; i++) {
		zassert_equal(drum.next(), 0.0f, "drum should stop after its length");
	}
}

/* --- White noise ------------------------------------------------------- */

ZTEST(dsp_primitives, test_white_noise)
{
	dsp::WhiteNoise noise;

	for (int i = 0; i < 100; i++) {
		zassert_equal(noise.next(), 0.0f, "silent at zero amplitude");
	}

	noise.set_amplitude(1.0f);

	float first = noise.next();
	bool varies = false;

	for (int i = 0; i < 1000; i++) {
		const float v = noise.next();

		zassert_true(fabsf(v) <= 1.0f, "noise left its range: %f", (double)v);

		if (v != first) {
			varies = true;
		}
	}

	zassert_true(varies, "noise should not be constant");
}
