/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice.h"

#include "../compat/duo_compat.h"

#include <cstring>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>

namespace duo {

Voice voice;

/*
 * The delay line is the single largest allocation in the firmware, so it is
 * placed in DTCM: it is not touched by DMA, and keeping it out of the shared
 * SRAM leaves room there for the audio and LED buffers.
 */
#if DT_HAS_CHOSEN(zephyr_dtcm)
__dtcm_noinit_section static int16_t delay_buffer[DELAY_SAMPLES];
#else
static int16_t delay_buffer[DELAY_SAMPLES];
#endif

void Voice::init()
{
	/*
	 * DTCM is not zeroed at reset and the delay line is deliberately
	 * noinit, so without this the first 350 ms of delay would replay
	 * whatever the previous session left behind - audible after the warm
	 * reset that brings the DUO back up from a soft power off.
	 */
	memset(delay_buffer, 0, sizeof(delay_buffer));

	/* Oscillators; the mixer gains keep the sum below clipping. */
	osc_saw.set_amplitude(0.4f);
	osc_saw.set_frequency(110.0f);
	osc_pulse.set_amplitude(0.5f);
	osc_pulse.set_frequency(220.0f);
	osc_pulse.set_pulse_width(0.5f);
	saw_mix = 0.2f;
	pulse_mix = 0.4f;

	filter.set_resonance(0.7f);
	filter.set_frequency(400.0f);
	filter.set_octave_control(4.0f);

	/* Amp envelope. */
	amp_env.attack(2.0f);
	amp_env.decay(0.0f);
	amp_env.sustain(1.0f);
	amp_env.release(400.0f);

	/* Filter envelope. */
	filter_env_amount = 1.0f;
	filter_env.attack(15.0f);
	filter_env.decay(0.0f);
	filter_env.sustain(1.0f);
	filter_env.release(300.0f);

	crusher.set_sample_rate(44100.0f);

	delay.set_delay(350);
	delay_filter.set_frequency(400.0f);
	delay_filter.set_resonance(0.7f);

	delay_send_env.attack(15.0f);
	delay_send_env.decay(0.0f);
	delay_send_env.sustain(1.0f);
	delay_send_env.release(15.0f);
	delay_in_gain = 0.5f;
	delay_fb_gain = 0.4f;
	delay_hat_gain = 0.0f;

	/* Hi-hat: noise through a high pass into a band pass, plus a click. */
	hat_env.attack(2.0f);
	hat_env.decay(20.0f);
	hat_env.sustain(0.0f);
	hat_env.release(0.0f);
	hat_filter_hp.set_frequency(6000.0f);
	hat_filter_hp.set_resonance(0.7f);
	hat_filter_bp.set_frequency(4000.0f);
	hat_filter_bp.set_resonance(1.0f);
	hat_snappy.set_length(30);
	hat_snappy.set_pitch_mod(4.0f);
	hat_snappy.set_frequency(126.0f);

	kick.set_length(100);
	kick.set_frequency(60.0f);
	kick.set_pitch_mod(4.0f);

	pop_suppressor.fade_in(10);
}

void Voice::note_on()
{
	amp_env.note_on();
	filter_env.note_on();
}

void Voice::note_off()
{
	amp_env.note_off();
	filter_env.note_off();
}

void Voice::set_delay_enabled(bool enabled)
{
	if (enabled) {
		delay_fader.fade_in(10);
		delay_send_env.note_on();
		delay_hat_gain = 0.4f;
	} else {
		/* The long fade matches the 3 * 440 ms of the legacy firmware. */
		delay_fader.fade_out(3 * 440);
		delay_send_env.note_off();
		delay_hat_gain = 0.0f;
	}
}

void Voice::set_output_gains(float main, float delay_out, float kick_out, float hat_out)
{
	main_gain = main;
	delay_gain = delay_out;
	kick_gain = kick_out;
	hat_gain = hat_out;
}

void Voice::kick_note_on(uint8_t velocity)
{
	kick.set_length(200 - velocity);
	kick.set_frequency((float)(velocity / 4 + 40));
	kick.note_on();

	/* Sidechain: duck the pulse oscillator while the kick sounds. */
	osc_pulse.set_amplitude(0.35f);
}

void Voice::kick_note_off()
{
	osc_pulse.set_amplitude(0.4f);
}

void Voice::hat_note_on(uint8_t velocity)
{
	if (velocity > 63) {
		hat_snappy.note_on();
	}

	hat_noise.set_amplitude(0.8f);
	hat_env.decay((float)((velocity / 4) + 20));
	hat_filter_bp.set_resonance(map(velocity, 0, 127, 100, 70) / 100.0f);

	hat_snappy_gain = map(velocity, 0, 127, 0, 100) / 100.0f;
	hat_noise_gain = map(velocity, 0, 127, 50, 20) / 100.0f;

	hat_env.note_on();
}

void Voice::hat_note_off()
{
	hat_env.note_off();
	hat_noise.set_amplitude(0.0f);
}

void Voice::render(int16_t *out, size_t frames)
{
	for (size_t i = 0; i < frames; i++) {
		/* --- oscillators --- */
		const float mixed = saw_mix * osc_saw.saw() + pulse_mix * osc_pulse.pulse();

		/* --- filter, swept by its own envelope --- */
		filter.set_modulation(filter_env.next() * filter_env_amount);
		filter.process(mixed);

		/* --- amplitude envelope and bit crusher --- */
		const float voiced = filter.low() * amp_env.next();
		const float crushed = crusher.process(voiced);

		/* peak1 in the legacy patch hangs off the amp envelope, not the
		 * output mix, so the ENV LED tracks the synth voice alone.
		 */
		output_peak.process(voiced);

		/* --- drums --- */
		const float kick_out = kick.next();

		hat_filter_hp.process(hat_noise.next() * hat_env.next());
		hat_filter_bp.process(hat_filter_hp.high());
		const float hat_out = hat_noise_gain * hat_filter_bp.band() +
				      hat_snappy_gain * hat_snappy.next();

		/* --- delay, with the hi-hat feeding its input as well --- */
		const float delay_in = delay_in_gain * (crushed * delay_send_env.next()) +
				       delay_fb_gain * delay_feedback_sample +
				       delay_hat_gain * hat_out;
		const float delay_out = delay.process(delay_in, delay_buffer);

		delay_filter.process(delay_out);
		delay_feedback_sample = delay_filter.high() * delay_fader.next();

		/* --- output mix --- */
		float mix = main_gain * crushed + delay_gain * delay_out +
			    kick_gain * kick_out + hat_gain * hat_out;

		mix *= pop_suppressor.next();

		const float left = dsp::clampf(mix * headphone_gain, -1.0f, 1.0f);
		const float right = dsp::clampf(mix * speaker_gain, -1.0f, 1.0f);

		out[2 * i] = (int16_t)(left * 32767.0f);
		out[2 * i + 1] = (int16_t)(right * 32767.0f);
	}
}

} /* namespace duo */
