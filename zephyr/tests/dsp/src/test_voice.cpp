/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration tests for the DUO voice: the whole patch rendering real frames,
 * checked for the behaviour a player would notice - silence when idle, sound
 * on a note, decay after release, and the delay actually repeating.
 */

#include "dsp/voice.h"

#include <zephyr/ztest.h>

#include <cmath>
#include <cstdlib>

#define FRAMES 128

/* Renders one block and returns the largest absolute sample in it. */
static int render_block(duo::Voice &voice)
{
	int16_t block[FRAMES * 2];
	int loudest = 0;

	voice.render(block, FRAMES);

	for (int i = 0; i < FRAMES * 2; i++) {
		const int sample = abs((int)block[i]);

		if (sample > loudest) {
			loudest = sample;
		}
	}

	return loudest;
}

/* Renders `blocks` blocks and returns the largest absolute sample across them. */
static int render_blocks(duo::Voice &voice, int blocks)
{
	int loudest = 0;

	for (int i = 0; i < blocks; i++) {
		/*
		 * Deliberately not MAX(): it is a macro that would evaluate
		 * render_block() twice and render extra audio.
		 */
		const int peak = render_block(voice);

		if (peak > loudest) {
			loudest = peak;
		}
	}

	return loudest;
}

static int ms_to_blocks(int ms)
{
	return (int)((44100.0f * (float)ms / 1000.0f) / (float)FRAMES) + 1;
}

ZTEST_SUITE(duo_voice, NULL, NULL, NULL, NULL, NULL);

ZTEST(duo_voice, test_idle_voice_is_silent)
{
	duo::Voice voice;

	voice.init();

	/*
	 * The oscillators free-run, so silence here depends on the amplitude
	 * envelope actually gating them.
	 */
	zassert_equal(render_blocks(voice, 20), 0, "an untouched voice must be silent");
}

ZTEST(duo_voice, test_note_on_produces_sound)
{
	duo::Voice voice;

	voice.init();
	voice.set_filter_env_amount(1.0f);
	voice.note_on();

	const int loudest = render_blocks(voice, 8);

	zassert_true(loudest > 100, "a held note should be audible, peak was %d", loudest);
}

ZTEST(duo_voice, test_note_off_decays_to_silence)
{
	duo::Voice voice;

	voice.init();
	voice.set_amp_release(50.0f);
	voice.note_on();
	render_blocks(voice, ms_to_blocks(50));

	voice.note_off();

	/* Generous margin: the integer envelope runs a few percent long. */
	render_blocks(voice, ms_to_blocks(200));

	zassert_equal(render_block(voice), 0, "the voice should fall silent after its release");
}

ZTEST(duo_voice, test_output_never_exceeds_full_scale)
{
	duo::Voice voice;

	voice.init();

	/* Gains far past anything the panel can ask for. */
	voice.set_output_gains(10.0f, 10.0f, 10.0f, 10.0f);
	voice.set_preamp_gains(10.0f, 10.0f);
	voice.set_filter_resonance(5.0f);
	voice.note_on();
	voice.kick_note_on(127);
	voice.hat_note_on(127);

	for (int block = 0; block < 40; block++) {
		int16_t frames[FRAMES * 2];

		voice.render(frames, FRAMES);

		for (int i = 0; i < FRAMES * 2; i++) {
			zassert_true(frames[i] >= -32767 && frames[i] <= 32767,
				     "sample %d wrapped: %d", i, frames[i]);
		}
	}
}

ZTEST(duo_voice, test_peak_follows_the_voice)
{
	duo::Voice voice;

	voice.init();

	render_blocks(voice, 4);
	zassert_within(voice.peak(), 0.0f, 0.001f, "idle voice should report no peak");

	voice.note_on();
	render_blocks(voice, 8);

	zassert_true(voice.peak() > 0.0f, "a sounding note should register on the peak meter");
}

ZTEST(duo_voice, test_drums_sound_independently_of_the_synth)
{
	duo::Voice voice;

	voice.init();

	/* No note held, so anything audible here is the drum voice. */
	voice.kick_note_on(100);
	zassert_true(render_blocks(voice, 8) > 100, "the kick should be audible on its own");

	duo::Voice hat_voice;

	hat_voice.init();
	hat_voice.hat_note_on(127);
	zassert_true(render_blocks(hat_voice, 8) > 100, "the hat should be audible on its own");
}

/* Renders for `ms` and returns the loudest sample heard in that span. */
static int render_ms(duo::Voice &voice, int ms)
{
	return render_blocks(voice, ms_to_blocks(ms));
}

/*
 * With the delay engaged, a note should come back one tap time later - not
 * sooner. Checking a window before the tap as well as one around it pins down
 * the 350 ms tap time itself, rather than just the presence of an echo.
 */
ZTEST(duo_voice, test_delay_repeats_one_tap_time_later)
{
	duo::Voice voice;

	voice.init();
	voice.set_amp_release(10.0f);
	voice.set_delay_enabled(true);

	/* t = 0: a short note, silent again well before t = 150 ms. */
	voice.note_on();
	render_ms(voice, 50);
	voice.note_off();
	render_ms(voice, 100);

	/* t = 150..250 ms: too early for a 350 ms tap, so nothing should sound. */
	const int before_tap = render_ms(voice, 100);

	/* t = 250..450 ms: the repeat lands here. */
	const int at_tap = render_ms(voice, 200);

	zassert_equal(before_tap, 0, "nothing should sound before the tap time, peak was %d",
		      before_tap);
	zassert_true(at_tap > 100, "the delay should repeat the note, peak was %d", at_tap);
}

/* The same timeline with the delay left off must stay silent throughout. */
ZTEST(duo_voice, test_no_repeat_when_delay_is_off)
{
	duo::Voice voice;

	voice.init();
	voice.set_amp_release(10.0f);

	voice.note_on();
	render_ms(voice, 50);
	voice.note_off();
	render_ms(voice, 100);

	zassert_equal(render_ms(voice, 300), 0, "no delay means no repeat");
}

/*
 * The pulse oscillator's duty cycle shows up as a DC offset, and the filter
 * passes DC at unity gain, so at the panel's widest pulse width it would eat
 * about 14% of the output range for something inaudible.
 */
ZTEST(duo_voice, test_output_is_dc_free_at_maximum_pulse_width)
{
	duo::Voice voice;
	int16_t block[FRAMES * 2];
	long sum = 0;
	int counted = 0;

	voice.init();
	voice.set_pulse_width(0.95f);
	voice.set_amp_release(500.0f);
	voice.note_on();

	/* Skip the first blocks so the pop suppressor has faded in. */
	for (int b = 0; b < 40; b++) {
		voice.render(block, FRAMES);

		if (b < 10) {
			continue;
		}

		for (int i = 0; i < FRAMES * 2; i++) {
			sum += block[i];
			counted++;
		}
	}

	const float mean = (float)sum / (float)counted;

	zassert_true(fabsf(mean) < 200.0f, "output carries %f of DC at full scale 32767",
		     (double)mean);
}

/*
 * The amp envelope gates whatever the oscillators produce, so any DC riding on
 * them becomes a step at note on and another at note off - a thump rather than
 * a note. This is the reason the offset is removed at the oscillator instead of
 * with a DC blocker further down.
 */
ZTEST(duo_voice, test_note_on_does_not_step_the_output)
{
	duo::Voice voice;
	int16_t block[FRAMES * 2];

	voice.init();
	voice.set_pulse_width(0.95f);

	/* Settle, with no note held: the output should be sitting at silence. */
	for (int b = 0; b < 20; b++) {
		voice.render(block, FRAMES);
	}

	zassert_equal(render_block(voice), 0, "an idle voice should be silent");

	voice.note_on();
	voice.render(block, FRAMES);

	/*
	 * The amp envelope attack is 2 ms, so the first few samples should rise
	 * from zero rather than jumping. A DC step would appear immediately.
	 */
	for (int i = 0; i < 8; i++) {
		zassert_true(abs((int)block[i]) < 400, "sample %d jumped to %d at note on", i,
			     block[i]);
	}
}

/*
 * The delay line is noinit and lives in DTCM, which a warm reset does not
 * clear, so init() has to blank it or the first tap replays stale audio.
 */
ZTEST(duo_voice, test_init_clears_the_delay_line)
{
	{
		duo::Voice voice;

		voice.init();
		voice.set_delay_enabled(true);
		voice.note_on();
		render_ms(voice, 100);
	}

	/* A fresh voice on the same (shared, noinit) buffer must start clean. */
	duo::Voice voice;

	voice.init();
	voice.set_delay_enabled(true);

	zassert_equal(render_ms(voice, 400), 0,
		      "a re-initialised voice must not replay the previous session");
}

/* A 200 Hz tone standing in for a sampled drum. */
static const int16_t *test_tone(size_t *length)
{
	static int16_t tone[4410];

	for (size_t i = 0; i < ARRAY_SIZE(tone); i++) {
		tone[i] = (int16_t)(20000.0f * sinf(2.0f * 3.14159265f * 200.0f * i / 44100.0f));
	}

	*length = ARRAY_SIZE(tone);
	return tone;
}

ZTEST(duo_voice, test_a_pad_sample_plays_instead_of_the_synthesised_drum)
{
	static const int16_t silence[4410] = {};
	size_t tone_length;
	const int16_t *tone = test_tone(&tone_length);

	/*
	 * A silent sample on each pad: if the synthesised drum still sounded
	 * alongside it, this would not be silent.
	 */
	duo::Voice voice;

	voice.init();
	voice.set_kick_sample(silence, ARRAY_SIZE(silence));
	voice.set_hat_sample(silence, ARRAY_SIZE(silence));
	voice.kick_note_on(127);
	voice.hat_note_on(127);
	zassert_equal(render_blocks(voice, 20), 0, "a sampled pad must not also play the synth");

	/* A real sample is heard, at every velocity the pads report. */
	static const uint8_t pad_velocities[] = {0, 31, 63, 95, 127};

	for (const uint8_t velocity : pad_velocities) {
		duo::Voice sampled;

		sampled.init();
		sampled.set_kick_sample(tone, tone_length);
		sampled.kick_note_on(velocity);
		zassert_true(render_blocks(sampled, 8) > 1000,
			     "the kick pad sample should be heard at velocity %u", velocity);

		duo::Voice sampled_hat;

		sampled_hat.init();
		sampled_hat.set_hat_sample(tone, tone_length);
		sampled_hat.hat_note_on(velocity);
		zassert_true(render_blocks(sampled_hat, 8) > 1000,
			     "the hat pad sample should be heard at velocity %u", velocity);
	}

	/* Taking the sample away brings the synthesised drum back. */
	duo::Voice restored;

	restored.init();
	restored.set_kick_sample(silence, ARRAY_SIZE(silence));
	restored.set_kick_sample(nullptr, 0);
	restored.kick_note_on(100);
	zassert_true(render_blocks(restored, 8) > 100, "the synthesised kick should be back");
}

ZTEST(duo_voice, test_a_sampled_hat_feeds_the_delay)
{
	size_t tone_length;
	const int16_t *tone = test_tone(&tone_length);
	duo::Voice voice;

	/* The synthesised hat is sent to the delay; a sample in its place must be too. */
	voice.init();
	voice.set_delay_enabled(true);
	voice.set_hat_sample(tone, tone_length);
	voice.hat_note_on(127);

	/* The 100 ms tone, then a gap until the 350 ms tap brings it back. */
	render_ms(voice, 200);
	zassert_true(render_ms(voice, 300) > 100, "the hat sample should repeat in the delay");
}
