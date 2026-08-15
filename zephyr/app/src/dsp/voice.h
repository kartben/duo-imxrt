/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The DUO voice: a one-for-one rebuild of the Teensy Audio Library patch in
 * brains2/apps/duo/duo-firmware/src/Synth.h and DrumSynth.h.
 *
 *   osc_saw ---\
 *               mixer1 --> filter1 --> envelope1 --> bitcrusher --> mixer_out
 *   osc_pulse -/             ^                            |            ^ ^ ^
 *                            |                            v            | | |
 *              dc1 -> envelope2                    delay_envelope      | | |
 *                                                         |            | | |
 *                              delay_fader <- delay_filter <- delay <--+ | |
 *                                                                       | |
 *                     kick_drum ---------------------------------------+ |
 *                     hat (noise -> env -> HP -> BP) + snappy ------------+
 *
 * Every gain, envelope time and filter setting below is the value the legacy
 * firmware programs; only the implementation of the blocks differs.
 */

#pragma once

#include "dsp.h"

#include <cstddef>
#include <cstdint>

namespace duo {

/* 350 ms of delay at 44.1 kHz, the tap time the legacy firmware sets. */
static constexpr size_t DELAY_SAMPLES = 15500;

class Voice {
public:
	/* Ported from audio_init() in Synth.h and drum_init() in DrumSynth.h. */
	void init();

	/* Renders `frames` stereo frames, left = headphone, right = speaker. */
	void render(int16_t *out, size_t frames);

	/* --- oscillators and pitch --- */
	void set_saw_frequency(float hz)
	{
		osc_saw.set_frequency(hz);
	}

	void set_saw_amplitude(float value)
	{
		osc_saw.set_amplitude(value);
	}

	void set_pulse_frequency(float hz)
	{
		osc_pulse.set_frequency(hz);
	}

	void set_pulse_amplitude(float value)
	{
		osc_pulse.set_amplitude(value);
	}

	void set_pulse_width(float value)
	{
		osc_pulse.set_pulse_width(value);
	}

	/* --- filter --- */
	void set_filter_frequency(float hz)
	{
		filter.set_frequency(hz);
	}

	void set_filter_resonance(float value)
	{
		filter.set_resonance(value);
	}

	/* Envelope depth, driven by note velocity; stands in for dc1.amplitude(). */
	void set_filter_env_amount(float value)
	{
		filter_env_amount = dsp::clampf(value, 0.0f, 1.0f);
	}

	/* --- envelopes --- */
	void set_amp_release(float ms)
	{
		amp_env.release(ms);
	}

	void note_on();
	void note_off();

	/* --- bitcrusher --- */
	void set_crush_sample_rate(float hz)
	{
		crusher.set_sample_rate(hz);
	}

	/* --- delay --- */
	void set_delay_enabled(bool enabled);

	/* --- output --- */
	void set_output_gains(float main, float delay, float kick, float hat);

	void set_preamp_gains(float headphone, float speaker)
	{
		headphone_gain = headphone;
		speaker_gain = speaker;
	}

	void pop_suppressor_fade_in(uint32_t ms)
	{
		pop_suppressor.fade_in(ms);
	}

	void pop_suppressor_fade_out(uint32_t ms)
	{
		pop_suppressor.fade_out(ms);
	}

	float peak()
	{
		return output_peak.read();
	}

	/* --- drums, ported from DrumSynth.h --- */
	void kick_note_on(uint8_t velocity);
	void kick_note_off();
	void hat_note_on(uint8_t velocity);
	void hat_note_off();

private:
	dsp::Oscillator osc_saw;
	dsp::Oscillator osc_pulse;

	dsp::StateVariableFilter filter;
	dsp::Envelope amp_env;    /* envelope1 */
	dsp::Envelope filter_env; /* envelope2, fed by dc1 */
	float filter_env_amount = 1.0f;

	dsp::Bitcrusher crusher;

	dsp::Envelope delay_send_env; /* delay_envelope */
	dsp::DelayLine<DELAY_SAMPLES> delay;
	dsp::StateVariableFilter delay_filter;
	dsp::Fade delay_fader;
	float delay_feedback_sample = 0.0f;

	dsp::SimpleDrum kick;
	dsp::WhiteNoise hat_noise;
	dsp::Envelope hat_env;
	dsp::StateVariableFilter hat_filter_hp;
	dsp::StateVariableFilter hat_filter_bp;
	dsp::SimpleDrum hat_snappy;
	float hat_noise_gain = 0.5f;
	float hat_snappy_gain = 0.5f;

	dsp::Fade pop_suppressor;
	dsp::PeakDetector output_peak;

	/* mixer1: oscillator balance. */
	float saw_mix = 0.2f;
	float pulse_mix = 0.4f;

	/* mixer_delay: delay input, feedback and hi-hat send. */
	float delay_in_gain = 0.5f;
	float delay_fb_gain = 0.4f;
	float delay_hat_gain = 0.0f;

	/* mixer_output. */
	float main_gain = 0.8f;
	float delay_gain = 1.0f;
	float kick_gain = 1.0f;
	float hat_gain = 1.2f;

	float headphone_gain = 2.0f;
	float speaker_gain = 2.0f;
};

extern Voice voice;

} /* namespace duo */
