/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * DSP primitives for the DUO voice.
 *
 * The legacy firmware builds its voice from Teensy Audio Library objects
 * (brains2/apps/duo/duo-firmware/src/Synth.h). That library is bound to the
 * Teensy core's block/DMA machinery, so the same signal chain is rebuilt here
 * from self-contained blocks. Each block below states which Teensy object it
 * stands in for, and the parameter ranges are kept identical so that the
 * mapping code in Synth.h could be carried over unchanged.
 */

#pragma once

#include "shared/duo/envelope.h"

#include <cmath>
#include <cstdint>

namespace dsp {

static constexpr float SAMPLE_RATE = 44100.0f;

static constexpr float PI_F = 3.14159265358979323846f;

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Full scale sample conversion, rounding to nearest rather than truncating
 * towards zero as a plain cast would - truncation is a half LSB error that
 * changes sign with the signal, which is crossover distortion at low levels.
 *
 * Written out rather than calling lrintf() because GCC emits a real library
 * call for that even with -fno-math-errno, and this runs three times a frame.
 * The caller must have clamped `v` to +/-1 already.
 */
static inline int16_t to_pcm16(float v)
{
	return (int16_t)(int32_t)(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

/*
 * sinf() and exp2f() are software routines on this core, and the filter needs
 * both of them on every sample to follow its envelope. Neither is needed at
 * full range: the arguments below are bounded by the filter's own clamps, so a
 * short polynomial covers them to far better than 16 bit precision. See
 * test_fast_math_matches_libm in the DSP tests, which pins the error.
 */

/* sin(x) to within 3e-7 absolute over 0 <= x <= PI/8, the filter's range. */
static inline float sin_poly(float x)
{
	const float x2 = x * x;

	/* x - x^3/6 + x^5/120 */
	return x * (1.0f - x2 * (1.0f / 6.0f) * (1.0f - x2 * (1.0f / 20.0f)));
}

/* Largest exponent exp2_poly() accepts; the filter never asks for more than 4. */
static constexpr float EXP2_MAX = 8.0f;

/* 2^x for 0 <= x <= EXP2_MAX, to within 9e-5 relative and exact at integers. */
static inline float exp2_poly(float x)
{
	static const float POW2[] = {1.0f,  2.0f,  4.0f,   8.0f,  16.0f,
				     32.0f, 64.0f, 128.0f, 256.0f};

	x = clampf(x, 0.0f, EXP2_MAX);

	/* x is non-negative, so a truncating cast is the integer part. */
	const int whole = (int)x;
	const float frac = x - (float)whole;

	/*
	 * 2^f = e^(f ln2), truncated after the f^5 term. The coefficients are
	 * just ln(2)^k / k!, which is easier to check by eye than a minimax
	 * fit and, over [0, 1), is already accurate to 8.5e-5.
	 */
	const float mantissa =
		1.0f + frac * (0.69314718f +
			       frac * (0.24022651f +
				       frac * (0.05550411f +
					       frac * (0.00961813f + frac * 0.00133336f))));

	return POW2[whole] * mantissa;
}

/*
 * Band-limited oscillator, standing in for AudioSynthWaveform in
 * WAVEFORM_BANDLIMIT_SAWTOOTH / WAVEFORM_BANDLIMIT_PULSE mode. Discontinuities
 * are smoothed with PolyBLEP, which is the cheap equivalent of the Teensy
 * library's band-limited step tables.
 */
class Oscillator {
public:
	void set_frequency(float hz)
	{
		phase_inc = clampf(hz, 0.0f, SAMPLE_RATE * 0.45f) / SAMPLE_RATE;
	}

	void set_amplitude(float value)
	{
		amplitude = clampf(value, 0.0f, 1.0f);
	}

	void set_pulse_width(float value)
	{
		pulse_width = clampf(value, 0.02f, 0.98f);
	}

	float saw()
	{
		const float value = (2.0f * phase - 1.0f) - poly_blep(phase);

		advance();
		return value * amplitude;
	}

	float pulse()
	{
		float value = phase < pulse_width ? 1.0f : -1.0f;

		value += poly_blep(phase);
		value -= poly_blep(wrap(phase - pulse_width));

		/*
		 * A pulse of duty d has a mean of 2d - 1, and the filter it feeds
		 * passes DC at unity gain. The DUO's pulse width runs to 0.95, so
		 * without this the amp envelope would be gating up to 0.14 of full
		 * scale of constant offset: wasted headroom, and a step at every
		 * note on and note off.
		 *
		 * Removing it here rather than with a DC blocker on the output is
		 * both cheaper and better behaved - it leaves the audible content
		 * bit-identical and there is no filter transient to decay.
		 */
		value -= 2.0f * pulse_width - 1.0f;

		advance();
		return value * amplitude;
	}

private:
	static float wrap(float p)
	{
		while (p < 0.0f) {
			p += 1.0f;
		}
		while (p >= 1.0f) {
			p -= 1.0f;
		}
		return p;
	}

	void advance()
	{
		phase += phase_inc;
		if (phase >= 1.0f) {
			phase -= 1.0f;
		}
	}

	/* Correction applied either side of a discontinuity. */
	float poly_blep(float t) const
	{
		const float dt = phase_inc;

		if (dt <= 0.0f) {
			return 0.0f;
		}

		if (t < dt) {
			t /= dt;
			return t + t - t * t - 1.0f;
		}

		if (t > 1.0f - dt) {
			t = (t - 1.0f) / dt;
			return t * t + t + t + 1.0f;
		}

		return 0.0f;
	}

	float phase = 0.0f;
	float phase_inc = 0.0f;
	float amplitude = 0.0f;
	float pulse_width = 0.5f;
};

/*
 * Chamberlin state variable filter, standing in for
 * AudioFilterStateVariable. Resonance uses the same 0.7 to 5.0 range as the
 * Teensy object, where the damping factor is its reciprocal.
 */
class StateVariableFilter {
public:
	void set_frequency(float hz)
	{
		base_frequency = clampf(hz, 20.0f, SAMPLE_RATE * 0.4f);
		base_argument = PI_F * base_frequency / (SAMPLE_RATE * 2.0f);
		update_coefficient(1.0f);
	}

	void set_resonance(float value)
	{
		damping = 1.0f / clampf(value, 0.7f, 5.0f);
	}

	/* Matches AudioFilterStateVariable::octaveControl(). */
	void set_octave_control(float octaves)
	{
		octave_control = octaves;
	}

	/*
	 * `modulation` runs 0 to 1 and shifts the cutoff up by that many
	 * octaves. This is called once per sample to follow the filter
	 * envelope, so it takes the polynomial path rather than exp2f/sinf.
	 */
	void set_modulation(float modulation)
	{
		const float argument = clampf(base_argument * exp2_poly(modulation * octave_control),
					      MIN_ARGUMENT, MAX_ARGUMENT);

		coefficient = 2.0f * sin_poly(argument);
	}

	void process(float in)
	{
		/*
		 * Two passes per sample: the classic remedy for the Chamberlin
		 * topology's poor behaviour as the cutoff approaches Nyquist.
		 */
		for (int i = 0; i < 2; i++) {
			highpass = in - lowpass - damping * bandpass;
			bandpass += coefficient * highpass;
			lowpass += coefficient * bandpass;
		}
	}

	float low() const
	{
		return lowpass;
	}

	float band() const
	{
		return bandpass;
	}

	float high() const
	{
		return highpass;
	}

private:
	/* The cutoff clamp of update_coefficient(), expressed as its own argument. */
	static constexpr float MIN_ARGUMENT = PI_F * 20.0f / (SAMPLE_RATE * 2.0f);
	static constexpr float MAX_ARGUMENT = PI_F * (SAMPLE_RATE * 0.25f) / (SAMPLE_RATE * 2.0f);

	void update_coefficient(float multiplier)
	{
		const float hz = clampf(base_frequency * multiplier, 20.0f, SAMPLE_RATE * 0.25f);

		coefficient = 2.0f * sinf(PI_F * hz / (SAMPLE_RATE * 2.0f));
	}

	float base_frequency = 400.0f;
	/* PI * base_frequency / (2 * SAMPLE_RATE), so set_modulation() only scales. */
	float base_argument = PI_F * 400.0f / (SAMPLE_RATE * 2.0f);
	float coefficient = 0.05f;
	float damping = 1.0f / 0.7f;
	float octave_control = 0.0f;
	float lowpass = 0.0f;
	float bandpass = 0.0f;
	float highpass = 0.0f;
};

/*
 * Envelope generator. This reuses shared/duo/envelope.h - the same integer
 * linear envelope the legacy firmware runs - so the characteristic straight
 * attack and release curves are preserved exactly.
 */
class Envelope {
public:
	static const int ENVELOPE_MAX = 0x10000;

	Envelope() : env(ENVELOPE_MAX, 1)
	{
	}

	void attack(float ms)
	{
		attack_ms = ms;
		refresh();
	}

	void decay(float ms)
	{
		decay_ms = ms;
		refresh();
	}

	void sustain(float level)
	{
		sustain_level = (int)(clampf(level, 0.0f, 1.0f) * ENVELOPE_MAX);
		refresh();
	}

	void release(float ms)
	{
		release_ms = ms;
		refresh();
	}

	void note_on()
	{
		env.on();
	}

	void note_off()
	{
		env.off();
	}

	/* Advances one sample and returns the current level, 0 to 1. */
	float next()
	{
		env.step();
		return (float)env.curVal * (1.0f / (float)ENVELOPE_MAX);
	}

private:
	static int ms_to_samples(float ms)
	{
		if (ms < 0.0f) {
			ms = 0.0f;
		}

		return (int)(ms * (SAMPLE_RATE / 1000.0f));
	}

	void refresh()
	{
		env.adsr(ms_to_samples(attack_ms), ms_to_samples(decay_ms), sustain_level,
			 ms_to_samples(release_ms));
	}

	LinearEnvelope env;
	float attack_ms = 0.0f;
	float decay_ms = 0.0f;
	float release_ms = 0.0f;
	int sustain_level = ENVELOPE_MAX;
};

/* Linear fade in/out, standing in for AudioEffectFade. */
class Fade {
public:
	void fade_in(uint32_t ms)
	{
		target = 1.0f;
		set_rate(ms);
	}

	void fade_out(uint32_t ms)
	{
		target = 0.0f;
		set_rate(ms);
	}

	float next()
	{
		if (level < target) {
			level = clampf(level + rate, 0.0f, target);
		} else if (level > target) {
			level = clampf(level - rate, target, 1.0f);
		}

		return level;
	}

private:
	void set_rate(uint32_t ms)
	{
		const float samples = (float)ms * (SAMPLE_RATE / 1000.0f);

		rate = samples > 1.0f ? 1.0f / samples : 1.0f;
	}

	float level = 0.0f;
	float target = 0.0f;
	float rate = 1.0f;
};

/*
 * Sample rate reduction, standing in for AudioEffectBitcrusher. The DUO only
 * ever varies the sample rate; the bit depth stays at 16.
 */
class Bitcrusher {
public:
	void set_sample_rate(float hz)
	{
		step = clampf(hz, 1.0f, SAMPLE_RATE) / SAMPLE_RATE;
	}

	float process(float in)
	{
		accumulator += step;

		if (accumulator >= 1.0f) {
			accumulator -= 1.0f;
			held = in;
		}

		return held;
	}

private:
	float step = 1.0f;
	float accumulator = 0.0f;
	float held = 0.0f;
};

/*
 * Percussion voice, standing in for AudioSynthSimpleDrum: a sine whose pitch
 * sweeps down from pitch_mod times the base frequency, with an exponential
 * amplitude decay over `length` milliseconds.
 */
class SimpleDrum {
public:
	void set_length(uint32_t ms)
	{
		length_samples = (uint32_t)((float)ms * (SAMPLE_RATE / 1000.0f));
		if (length_samples == 0) {
			length_samples = 1;
		}
	}

	void set_frequency(float hz)
	{
		frequency = hz;
	}

	void set_pitch_mod(float value)
	{
		pitch_mod = value;
	}

	void note_on()
	{
		remaining = length_samples;
		phase = 0.0f;
		amplitude = 1.0f;
		/*
		 * The decay is expf(-5 * progress) and progress advances by
		 * 1/length_samples per sample, so the same curve is one multiply
		 * per sample once this ratio is known. Over the DUO's drum
		 * lengths the two agree to within a quarter of an LSB at 16 bit.
		 */
		decay = expf(-5.0f / (float)length_samples);
	}

	float next()
	{
		if (remaining == 0) {
			return 0.0f;
		}

		const float progress = 1.0f - ((float)remaining / (float)length_samples);
		/* Sweep from pitch_mod * f down to f across the note. */
		const float hz = frequency * (1.0f + (pitch_mod - 1.0f) * (1.0f - progress));

		phase += hz / SAMPLE_RATE;
		if (phase >= 1.0f) {
			phase -= 1.0f;
		}

		remaining--;

		const float value = sinf(2.0f * PI_F * phase) * amplitude;

		amplitude *= decay;

		return value;
	}

private:
	uint32_t length_samples = 4410;
	uint32_t remaining = 0;
	float frequency = 60.0f;
	float pitch_mod = 1.0f;
	float phase = 0.0f;
	float amplitude = 0.0f;
	float decay = 1.0f;
};

/* White noise, standing in for AudioSynthNoiseWhite. */
class WhiteNoise {
public:
	/*
	 * Without this the generator starts from the same constant every boot,
	 * so every power cycle produces an identical sequence of hi-hats.
	 */
	void seed(uint32_t value)
	{
		/* xorshift32 is stuck at zero, so never let it start there. */
		state = value != 0 ? value : 0x13579bdf;
	}

	void set_amplitude(float value)
	{
		amplitude = clampf(value, 0.0f, 1.0f);
	}

	float next()
	{
		/* xorshift32; cheap and flat enough for a hi-hat. */
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;

		return ((float)(int32_t)state * (1.0f / 2147483648.0f)) * amplitude;
	}

private:
	uint32_t state = 0x13579bdf;
	float amplitude = 0.0f;
};

/*
 * Fixed-length delay line, standing in for AudioEffectDelay. Samples are
 * stored as 16 bit to keep the buffer small enough for on-chip RAM.
 */
template <size_t Samples> class DelayLine {
public:
	void set_delay(uint32_t ms)
	{
		size_t requested = (size_t)((float)ms * (SAMPLE_RATE / 1000.0f));

		delay_samples = requested < Samples ? requested : Samples - 1;
		if (delay_samples == 0) {
			delay_samples = 1;
		}
	}

	float process(float in, int16_t *buffer)
	{
		/*
		 * Samples is not a power of two, so the modulo this replaces cost
		 * a multiply-shift sequence. Neither index can be more than one
		 * period out, so a conditional subtract is equivalent.
		 */
		size_t read_pos = write_pos + Samples - delay_samples;

		if (read_pos >= Samples) {
			read_pos -= Samples;
		}

		const float out = (float)buffer[read_pos] * (1.0f / 32768.0f);

		buffer[write_pos] = to_pcm16(clampf(in, -1.0f, 1.0f));

		if (++write_pos >= Samples) {
			write_pos = 0;
		}

		return out;
	}

private:
	size_t write_pos = 0;
	size_t delay_samples = Samples - 1;
};

/* Running peak detector, standing in for AudioAnalyzePeak. */
class PeakDetector {
public:
	void process(float in)
	{
		const float magnitude = fabsf(in);

		if (magnitude > peak) {
			peak = magnitude;
		}
	}

	/* Reads and clears, matching AudioAnalyzePeak::read(). */
	float read()
	{
		const float value = peak;

		peak = 0.0f;
		return value;
	}

private:
	float peak = 0.0f;
};

} /* namespace dsp */
