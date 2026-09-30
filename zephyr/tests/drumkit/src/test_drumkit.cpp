/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the sampled drum sounds that drumkit.py synthesises: every key
 * has a sound, none of them clicks on or off, clips or carries DC, and
 * together they stay small enough to flash quickly.
 */

#include "drum_kits.h"

/*
 * Ahead of ztest.h: glibc 2.43 names a field __unused, which Zephyr defines as
 * an attribute, so a C library header included after Zephyr's fails on hosts
 * that new.
 */
#include <cstdlib>

#include <zephyr/ztest.h>

struct Pad {
	const char *name;
	const duo::DrumSample *sounds;
};

static const Pad PADS[] = {
	{"kick", duo::KICK_PAD_SOUNDS},
	{"hat", duo::HAT_PAD_SOUNDS},
};

ZTEST_SUITE(drumkit, NULL, NULL, NULL, NULL, NULL);

ZTEST(drumkit, test_the_first_key_keeps_the_synthesised_drum)
{
	for (const Pad &pad : PADS) {
		zassert_is_null(pad.sounds[0].data, "the %s pad's first key should be the synth drum",
				pad.name);
	}
}

ZTEST(drumkit, test_every_other_key_has_a_sample)
{
	for (const Pad &pad : PADS) {
		for (size_t key = 1; key < duo::PAD_SOUNDS; key++) {
			zassert_not_null(pad.sounds[key].data, "%s pad key %u has no sample", pad.name,
					 key + 1);
			/* At least 20 ms, the shortest fade finish() applies. */
			zassert_true(pad.sounds[key].length >= 882, "%s pad key %u is too short",
				     pad.name, key + 1);
		}
	}
}

ZTEST(drumkit, test_samples_start_and_end_on_silence)
{
	for (const Pad &pad : PADS) {
		for (size_t key = 1; key < duo::PAD_SOUNDS; key++) {
			const duo::DrumSample &s = pad.sounds[key];

			/* Anything else would click as the hit starts or ends. */
			zassert_true(abs(s.data[0]) < 64, "%s pad key %u starts on %d", pad.name,
				     key + 1, s.data[0]);
			zassert_equal(s.data[s.length - 1], 0, "%s pad key %u ends on %d", pad.name,
				      key + 1, s.data[s.length - 1]);
		}
	}
}

ZTEST(drumkit, test_samples_are_loud_but_leave_headroom)
{
	for (const Pad &pad : PADS) {
		for (size_t key = 1; key < duo::PAD_SOUNDS; key++) {
			const duo::DrumSample &s = pad.sounds[key];
			int peak = 0;

			for (uint32_t i = 0; i < s.length; i++) {
				peak = MAX(peak, abs(s.data[i]));
			}

			/*
			 * The pads' velocity and the output gains scale these
			 * further; the level set in drumkit.py stays under 0.95.
			 */
			zassert_true(peak <= 31130, "%s pad key %u peaks at %d", pad.name, key + 1,
				     peak);
			zassert_true(peak >= 16384, "%s pad key %u peaks at only %d", pad.name, key + 1,
				     peak);
		}
	}
}

ZTEST(drumkit, test_samples_carry_no_dc)
{
	for (const Pad &pad : PADS) {
		for (size_t key = 1; key < duo::PAD_SOUNDS; key++) {
			const duo::DrumSample &s = pad.sounds[key];
			int64_t sum = 0;

			for (uint32_t i = 0; i < s.length; i++) {
				sum += s.data[i];
			}

			/* Under 1% of full scale on average. */
			const int64_t mean = sum / (int64_t)s.length;

			zassert_true(llabs(mean) < 328, "%s pad key %u has a DC offset of %lld",
				     pad.name, key + 1, (long long)mean);
		}
	}
}

ZTEST(drumkit, test_the_kit_fits_in_a_megabyte)
{
	size_t bytes = 0;

	for (const Pad &pad : PADS) {
		for (size_t key = 1; key < duo::PAD_SOUNDS; key++) {
			bytes += pad.sounds[key].length * sizeof(int16_t);
		}
	}

	/*
	 * Flash has room for far more, but every byte is erased, written and
	 * read back on each update, so this keeps flashing quick.
	 */
	zassert_true(bytes <= 1024 * 1024, "the samples take %u bytes", bytes);
	TC_PRINT("the samples take %u bytes of flash\n", bytes);
}
