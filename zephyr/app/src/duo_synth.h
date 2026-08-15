/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel-to-voice parameter mapping, ported from
 * brains2/apps/duo/duo-firmware/src/Synth.h. The patch itself now lives in
 * dsp/voice.h; what remains here is the mapping from 10 bit pot readings to
 * synth parameters, which is unchanged.
 */

#pragma once

#include "dsp/voice.h"

/* Output gains, from brains2/core/boards/DUO_BRAINS_2.1/board_audio_output.h. */
static const float HEADPHONE_GAIN = 2.0f;
static const float SPEAKER_GAIN = 2.0f;

static const float HEADPHONE_KICK_GAIN = 0.5f;
static const float HEADPHONE_HAT_GAIN = 0.5f;
static const float HEADPHONE_DELAY_GAIN = 1.0f;
static const float HEADPHONE_MAIN_GAIN = 0.8f;

static const float SPEAKER_MAIN_GAIN = 0.8f;
static const float SPEAKER_DELAY_GAIN = 1.0f;
static const float SPEAKER_KICK_GAIN = 0.8f;
static const float SPEAKER_HAT_GAIN = 1.0f;

float MAIN_GAIN = 0.8f;
float DELAY_GAIN = 1.0f;
float KICK_GAIN = 1.0f;
float HAT_GAIN = 1.2f;

static void audio_init()
{
	duo::voice.init();
	duo::voice.set_preamp_gains(HEADPHONE_GAIN, SPEAKER_GAIN);
	duo::voice.set_output_gains(MAIN_GAIN, DELAY_GAIN, KICK_GAIN, HAT_GAIN);
}

static void audio_volume(int volume)
{
	static const int LOW_VOLUME_THRESHOLD = 4;

	if (volume < LOW_VOLUME_THRESHOLD) {
		duo::voice.set_output_gains(0.0f, 0.0f, 0.0f, 0.0f);
	} else {
		duo::voice.set_output_gains(volume / (1023.0f / MAIN_GAIN),
					    volume / (1023.0f / DELAY_GAIN),
					    (volume + 512) / (2048.0f / KICK_GAIN),
					    (volume + 512) / (2048.0f / HAT_GAIN));
	}
}

static void synth_update()
{
	float osc_saw_amplitude = 0.4f;

	if (synth.detune > 850) {
		osc_saw_amplitude = map(synth.detune, 850, 1023, 400, 0) / 1000.0f;
	} else if (synth.detune < 200) {
		osc_saw_amplitude = map(synth.detune, 0, 200, 200, 400) / 1000.0f;
	}

	float bitcrusher_samplerate = HIGH_SAMPLE_RATE;

	if (synth.crush) {
		bitcrusher_samplerate = LOW_SAMPLE_RATE;
	}

	const float osc_pulse_pulseWidth = map(synth.pulseWidth, 0, 1023, 500, 950) / 1000.0f;
	float filter_resonance = map(synth.resonance, 0, 1023, 70, 320) / 100.0f;

	if (synth.accent) {
		filter_resonance = 4.0f;
	}

	duo::voice.set_saw_frequency(osc_saw_frequency);
	duo::voice.set_saw_amplitude(osc_saw_amplitude);

	duo::voice.set_pulse_frequency(osc_pulse_frequency / 2);
	duo::voice.set_pulse_width(osc_pulse_pulseWidth);

	duo::voice.set_filter_frequency((synth.filter / 2) + 30);
	duo::voice.set_filter_resonance(filter_resonance);

	duo::voice.set_amp_release((float)(((synth.release * synth.release) >> 11) + 30));

	duo::voice.set_crush_sample_rate(bitcrusher_samplerate);

	audio_volume(synth.amplitude);
}
