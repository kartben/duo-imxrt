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

/*
 * Pulse width is heard as the fraction of each period spent high. The raw
 * waveform carries that as a DC offset of 2 * duty - 1; the oscillator
 * subtracts it (see below), so the duty has to be measured from the sample
 * signs instead of from the mean.
 */
ZTEST(dsp_primitives, test_pulse_width_sets_duty_cycle)
{
	const float widths[] = {0.5f, 0.75f, 0.95f};

	for (float width : widths) {
		dsp::Oscillator osc;
		int high = 0;

		osc.set_frequency(441.0f);
		osc.set_amplitude(1.0f);
		osc.set_pulse_width(width);

		for (int i = 0; i < 1000; i++) {
			if (osc.pulse() > 0.0f) {
				high++;
			}
		}

		zassert_within(high / 1000.0f, width, 0.02f,
			       "duty %f should be high that fraction of the time, got %f",
			       (double)width, (double)(high / 1000.0f));
	}
}

/*
 * The filter downstream passes DC at unity gain and the amp envelope gates it,
 * so any offset here becomes wasted headroom plus a step at every note on and
 * note off. At the panel's maximum pulse width the raw offset would be 0.45.
 */
ZTEST(dsp_primitives, test_pulse_is_dc_free_at_every_width)
{
	const float widths[] = {0.5f, 0.6f, 0.75f, 0.9f, 0.95f};
	const float notes[] = {55.0f, 110.0f, 440.0f};

	for (float width : widths) {
		for (float note : notes) {
			dsp::Oscillator osc;
			float sum = 0.0f;
			const int n = 44100;

			osc.set_frequency(note);
			osc.set_amplitude(0.5f);
			osc.set_pulse_width(width);

			for (int i = 0; i < n; i++) {
				sum += osc.pulse();
			}

			zassert_within(sum / n, 0.0f, 0.002f,
				       "pw %f at %f Hz still has DC: %f", (double)width,
				       (double)note, (double)(sum / n));
		}
	}
}

/* Removing the DC must not touch the audible part of the waveform. */
ZTEST(dsp_primitives, test_pulse_ac_content_is_unchanged)
{
	/* RMS about the mean, measured from the implementation before the fix. */
	const struct {
		float width;
		float rms;
	} cases[] = {
		{0.50f, 0.49883f}, {0.75f, 0.43167f}, {0.90f, 0.29805f}, {0.95f, 0.21526f},
	};

	for (const auto &c : cases) {
		dsp::Oscillator osc;
		const int n = 44100;
		float sum = 0.0f;
		float sum_sq = 0.0f;

		osc.set_frequency(110.0f);
		osc.set_amplitude(0.5f);
		osc.set_pulse_width(c.width);

		for (int i = 0; i < n; i++) {
			const float v = osc.pulse();

			sum += v;
			sum_sq += v * v;
		}

		const float mean = sum / n;
		const float rms = sqrtf(sum_sq / n - mean * mean);

		zassert_within(rms, c.rms, 0.001f, "pw %f AC content changed: %f vs %f",
			       (double)c.width, (double)rms, (double)c.rms);
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

/*
 * The filter follows its envelope on every sample, so it uses polynomials
 * rather than sinf()/exp2f(). They only have to hold over the range the
 * filter's own clamps allow.
 */
ZTEST(dsp_primitives, test_fast_math_matches_libm)
{
	/* PI * hz / (2 * SR) for hz up to the SR/4 clamp. */
	const float max_arg = dsp::PI_F * (SR * 0.25f) / (SR * 2.0f);

	for (int i = 0; i <= 20000; i++) {
		const float x = max_arg * (float)i / 20000.0f;

		zassert_within(dsp::sin_poly(x), sinf(x), 1e-6f,
			       "sin_poly(%f) = %f, want %f", (double)x,
			       (double)dsp::sin_poly(x), (double)sinf(x));
	}

	for (int i = 0; i <= 20000; i++) {
		const float x = dsp::EXP2_MAX * (float)i / 20000.0f;
		const float want = exp2f(x);

		zassert_within(dsp::exp2_poly(x), want, want * 1e-4f,
			       "exp2_poly(%f) = %f, want %f", (double)x,
			       (double)dsp::exp2_poly(x), (double)want);
	}

	/* Exact at integer exponents, so an unmodulated filter is unaffected. */
	for (int i = 0; i <= 8; i++) {
		zassert_equal(dsp::exp2_poly((float)i), exp2f((float)i),
			      "exp2_poly(%d) should be exact", i);
	}
}

/*
 * Modulating up by N octaves must land on the same filter as tuning the base
 * frequency up by N octaves - this is what wires the polynomials to the sound.
 */
ZTEST(dsp_primitives, test_modulation_matches_an_equivalent_base_frequency)
{
	const float modulations[] = {0.0f, 0.25f, 0.5f, 1.0f};

	for (float m : modulations) {
		dsp::StateVariableFilter modulated;
		dsp::StateVariableFilter tuned;

		modulated.set_frequency(200.0f);
		modulated.set_resonance(0.7f);
		modulated.set_octave_control(4.0f);
		modulated.set_modulation(m);

		tuned.set_frequency(200.0f * exp2f(m * 4.0f));
		tuned.set_resonance(0.7f);

		for (int i = 0; i < 4410; i++) {
			const float x = sinf(2.0f * dsp::PI_F * 500.0f * (float)i / SR);

			modulated.process(x);
			tuned.process(x);

			zassert_within(modulated.low(), tuned.low(), 1e-3f,
				       "modulation %f diverged at sample %d: %f vs %f",
				       (double)m, i, (double)modulated.low(),
				       (double)tuned.low());
		}
	}
}

/*
 * The Chamberlin topology can run away when the coefficient gets large
 * relative to the damping. Two passes per sample is what buys the margin, so
 * this pins the whole cutoff/resonance space rather than a single corner.
 */
ZTEST(dsp_primitives, test_filter_is_stable_across_its_whole_range)
{
	const float cutoffs[] = {541.0f, 2000.0f, 4000.0f, 8656.0f, 11025.0f};
	const float resonances[] = {0.7f, 3.2f, 4.0f, 5.0f};

	for (float hz : cutoffs) {
		for (float q : resonances) {
			dsp::StateVariableFilter filter;
			float loudest = 0.0f;

			filter.set_frequency(hz);
			filter.set_resonance(q);

			/* Excited at the cutoff, the worst case for ringing. */
			for (int i = 0; i < 200000; i++) {
				filter.process(0.2f * sinf(2.0f * dsp::PI_F * hz *
							   (float)i / SR));

				const float v = fabsf(filter.low());

				if (v > loudest) {
					loudest = v;
				}
			}

			zassert_true(loudest < 2.0f,
				     "filter blew up at %f Hz Q %f: peak %f", (double)hz,
				     (double)q, (double)loudest);
		}
	}
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

/*
 * The decay is accumulated one multiply at a time rather than calling expf()
 * on every sample. It has to stay on the curve it replaces.
 */
ZTEST(dsp_primitives, test_simple_drum_decay_matches_an_exact_exponential)
{
	const uint32_t lengths[] = {30, 100, 200};

	for (uint32_t ms : lengths) {
		dsp::SimpleDrum drum;
		const int n = samples_ms((float)ms);

		/* No pitch sweep, so the sine is a known reference too. */
		drum.set_length(ms);
		drum.set_frequency(100.0f);
		drum.set_pitch_mod(1.0f);
		drum.note_on();

		float phase = 0.0f;

		for (int i = 0; i < n; i++) {
			phase += 100.0f / SR;
			if (phase >= 1.0f) {
				phase -= 1.0f;
			}

			const float want =
				sinf(2.0f * dsp::PI_F * phase) * expf(-5.0f * (float)i / (float)n);
			/* Held in a local: next() advances the drum, so it must
			 * not be evaluated again for the failure message.
			 */
			const float got = drum.next();

			/* Half an LSB at 16 bit. */
			zassert_within(got, want, 1.0f / 65536.0f, "length %u sample %d: %f vs %f",
				       ms, i, (double)got, (double)want);
		}
	}
}

/* --- Sample conversion ------------------------------------------------- */

ZTEST(dsp_primitives, test_to_pcm16_rounds_to_nearest)
{
	/* A plain cast truncates towards zero; these all sit between codes. */
	zassert_equal(dsp::to_pcm16(1.0f), 32767);
	zassert_equal(dsp::to_pcm16(-1.0f), -32767);
	zassert_equal(dsp::to_pcm16(0.0f), 0);

	/* 0.99998474 * 32767 = 32766.5, which truncation would put at 32766. */
	zassert_equal(dsp::to_pcm16(32766.5f / 32767.0f), 32767);
	zassert_equal(dsp::to_pcm16(-32766.5f / 32767.0f), -32767);

	/* Rounding must never bias one way: symmetric input, symmetric output. */
	for (int i = 1; i < 1000; i++) {
		const float v = (float)i / 1000.0f;

		zassert_equal(dsp::to_pcm16(v), -dsp::to_pcm16(-v),
			      "rounding is asymmetric at %f", (double)v);
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

/*
 * Left on its built-in constant the generator replays the same sequence from
 * every power-up, so every hi-hat sounds identical boot to boot.
 */
ZTEST(dsp_primitives, test_white_noise_seed)
{
	dsp::WhiteNoise a;
	dsp::WhiteNoise b;
	dsp::WhiteNoise c;
	int differences = 0;

	a.set_amplitude(1.0f);
	b.set_amplitude(1.0f);
	c.set_amplitude(1.0f);

	a.seed(1);
	b.seed(2);
	c.seed(1);

	for (int i = 0; i < 1000; i++) {
		const float from_a = a.next();

		/* Same seed reproduces exactly, so tests stay deterministic. */
		zassert_equal(from_a, c.next(), "the same seed must give the same sequence");

		if (from_a != b.next()) {
			differences++;
		}
	}

	zassert_true(differences > 900, "different seeds should give different noise, %d/1000",
		     differences);

	/* Zero would lock xorshift up permanently. */
	dsp::WhiteNoise zeroed;
	bool nonzero = false;

	zeroed.set_amplitude(1.0f);
	zeroed.seed(0);

	for (int i = 0; i < 100; i++) {
		if (zeroed.next() != 0.0f) {
			nonzero = true;
		}
	}

	zassert_true(nonzero, "a zero seed must not stall the generator");
}
