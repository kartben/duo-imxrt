/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the MIDI 1.0 byte-stream parser that replaced the Arduino MIDI
 * Library. The DUO listens on one channel at a time, so channel filtering and
 * running status are as important as the message decoding itself.
 */

#include "platform/midi_parser.h"

#include <zephyr/ztest.h>

#include <cstring>

/* The parser reports through C function pointers, so the observations land here. */
static struct {
	int note_on_count;
	int note_off_count;
	int cc_count;
	int clock_count;
	int start_count;
	int cont_count;
	int stop_count;
	int sysex_count;

	uint8_t last_channel;
	uint8_t last_data1;
	uint8_t last_data2;

	unsigned sysex_len;
	uint8_t sysex[MidiParser::SYSEX_MAX];
} seen;

static void on_note_on(byte channel, byte note, byte velocity)
{
	seen.note_on_count++;
	seen.last_channel = channel;
	seen.last_data1 = note;
	seen.last_data2 = velocity;
}

static void on_note_off(byte channel, byte note, byte velocity)
{
	seen.note_off_count++;
	seen.last_channel = channel;
	seen.last_data1 = note;
	seen.last_data2 = velocity;
}

static void on_cc(byte channel, byte number, byte value)
{
	seen.cc_count++;
	seen.last_channel = channel;
	seen.last_data1 = number;
	seen.last_data2 = value;
}

static void on_clock(void)
{
	seen.clock_count++;
}

static void on_start(void)
{
	seen.start_count++;
}

static void on_continue(void)
{
	seen.cont_count++;
}

static void on_stop(void)
{
	seen.stop_count++;
}

static void on_sysex(byte *data, unsigned length)
{
	seen.sysex_count++;
	seen.sysex_len = length;
	memcpy(seen.sysex, data, MIN(length, sizeof(seen.sysex)));
}

static MidiParser parser;

static void feed(const uint8_t *bytes, size_t len, uint8_t channel)
{
	for (size_t i = 0; i < len; i++) {
		parser.feed(bytes[i], channel);
	}
}

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);

	memset(&seen, 0, sizeof(seen));

	parser = MidiParser();
	parser.set_callbacks(MIDI::Callbacks{.note_on = on_note_on,
					     .note_off = on_note_off,
					     .clock = on_clock,
					     .start = on_start,
					     .cont = on_continue,
					     .stop = on_stop,
					     .cc = on_cc,
					     .sysex = on_sysex});
}

ZTEST_SUITE(midi_parser, NULL, NULL, reset, NULL, NULL);

ZTEST(midi_parser, test_note_on)
{
	const uint8_t msg[] = {0x91, 0x3c, 0x64}; /* channel 2, note 60, vel 100 */

	feed(msg, sizeof(msg), 2);

	zassert_equal(seen.note_on_count, 1, "expected one note on");
	zassert_equal(seen.last_channel, 2, "channel is reported 1-based");
	zassert_equal(seen.last_data1, 0x3c);
	zassert_equal(seen.last_data2, 0x64);
}

ZTEST(midi_parser, test_note_off)
{
	const uint8_t msg[] = {0x80, 0x3c, 0x40};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.note_off_count, 1);
	zassert_equal(seen.note_on_count, 0);
	zassert_equal(seen.last_data1, 0x3c);
}

/* A note on with zero velocity is the conventional way to spell note off. */
ZTEST(midi_parser, test_note_on_zero_velocity_is_note_off)
{
	const uint8_t msg[] = {0x90, 0x3c, 0x00};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.note_off_count, 1, "zero velocity should report note off");
	zassert_equal(seen.note_on_count, 0);
}

ZTEST(midi_parser, test_running_status)
{
	/* One status byte followed by three note pairs. */
	const uint8_t msg[] = {0x90, 0x3c, 0x40, 0x3e, 0x40, 0x40, 0x40};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.note_on_count, 3, "running status should repeat the last status");
	zassert_equal(seen.last_data1, 0x40, "last note should be the third one");
}

ZTEST(midi_parser, test_channel_filtering)
{
	const uint8_t on_ch1[] = {0x90, 0x3c, 0x40};
	const uint8_t on_ch2[] = {0x91, 0x3e, 0x40};

	feed(on_ch1, sizeof(on_ch1), 1);
	feed(on_ch2, sizeof(on_ch2), 1);

	zassert_equal(seen.note_on_count, 1, "messages on other channels must be dropped");
	zassert_equal(seen.last_data1, 0x3c);
}

ZTEST(midi_parser, test_control_change)
{
	const uint8_t msg[] = {0xb0, 74, 99};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.cc_count, 1);
	zassert_equal(seen.last_data1, 74);
	zassert_equal(seen.last_data2, 99);
}

ZTEST(midi_parser, test_realtime_messages)
{
	const uint8_t msg[] = {0xf8, 0xfa, 0xfb, 0xfc};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.clock_count, 1);
	zassert_equal(seen.start_count, 1);
	zassert_equal(seen.cont_count, 1);
	zassert_equal(seen.stop_count, 1);
}

/* Real time bytes are allowed to arrive between the bytes of another message. */
ZTEST(midi_parser, test_realtime_interleaved_in_voice_message)
{
	const uint8_t msg[] = {0x90, 0x3c, 0xf8, 0x40};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.clock_count, 1, "clock should be dispatched immediately");
	zassert_equal(seen.note_on_count, 1, "the note should survive the interruption");
	zassert_equal(seen.last_data2, 0x40);
}

ZTEST(midi_parser, test_sysex_includes_framing)
{
	/* The firmware version request the DUO answers. */
	const uint8_t msg[] = {0xf0, 0x7d, 0x64, 0x01, 0xf7};

	feed(msg, sizeof(msg), 1);

	zassert_equal(seen.sysex_count, 1);
	zassert_equal(seen.sysex_len, sizeof(msg), "0xF0 and 0xF7 are part of the buffer");
	zassert_mem_equal(seen.sysex, msg, sizeof(msg));
}

/* Real time bytes inside a sysex must not end up in the reassembled buffer. */
ZTEST(midi_parser, test_realtime_inside_sysex)
{
	const uint8_t stream[] = {0xf0, 0x7d, 0xf8, 0x64, 0x01, 0xf7};
	const uint8_t expected[] = {0xf0, 0x7d, 0x64, 0x01, 0xf7};

	feed(stream, sizeof(stream), 1);

	zassert_equal(seen.clock_count, 1);
	zassert_equal(seen.sysex_count, 1);
	zassert_equal(seen.sysex_len, sizeof(expected));
	zassert_mem_equal(seen.sysex, expected, sizeof(expected));
}

/* An unterminated sysex is abandoned when the next status byte arrives. */
ZTEST(midi_parser, test_sysex_interrupted_by_status)
{
	const uint8_t stream[] = {0xf0, 0x7d, 0x64, 0x90, 0x3c, 0x40};

	feed(stream, sizeof(stream), 1);

	zassert_equal(seen.sysex_count, 0, "an interrupted sysex must not be dispatched");
	zassert_equal(seen.note_on_count, 1, "the interrupting message should still parse");
}

/* A sysex longer than the reassembly buffer must not overrun it. */
ZTEST(midi_parser, test_sysex_overflow_is_truncated)
{
	parser.feed(0xf0, 1);
	for (unsigned i = 0; i < MidiParser::SYSEX_MAX * 2; i++) {
		parser.feed(0x01, 1);
	}
	parser.feed(0xf7, 1);

	zassert_equal(seen.sysex_count, 1);
	zassert_true(seen.sysex_len <= MidiParser::SYSEX_MAX,
		     "reassembly must stay inside the buffer, got %u", seen.sysex_len);
}

/* Data bytes with no preceding status byte are meaningless and dropped. */
ZTEST(midi_parser, test_data_without_status_is_ignored)
{
	const uint8_t stream[] = {0x3c, 0x40, 0x3e};

	feed(stream, sizeof(stream), 1);

	zassert_equal(seen.note_on_count, 0);
	zassert_equal(seen.note_off_count, 0);
	zassert_equal(seen.cc_count, 0);
}

/* System common cancels running status, per the MIDI specification. */
ZTEST(midi_parser, test_system_common_cancels_running_status)
{
	const uint8_t stream[] = {0x90, 0x3c, 0x40, 0xf3, 0x01, 0x3e, 0x40};

	feed(stream, sizeof(stream), 1);

	zassert_equal(seen.note_on_count, 1, "only the first note should be decoded");
}

/* What the soft thru sends back out, one entry per message. */
static struct {
	unsigned count;
	uint8_t bytes[512];
	size_t len;
	size_t lens[32];
} thru;

static void on_thru(const uint8_t *bytes, size_t len)
{
	zassert_true(thru.len + len <= sizeof(thru.bytes));
	memcpy(&thru.bytes[thru.len], bytes, len);
	thru.len += len;
	if (thru.count < ARRAY_SIZE(thru.lens)) {
		thru.lens[thru.count] = len;
	}
	thru.count++;
}

static void enable_thru(void)
{
	memset(&thru, 0, sizeof(thru));
	parser.set_thru(on_thru);
}

/*
 * Like the Arduino library's Thru::Full, every channel is passed on, not only
 * the one the DUO listens to, and running status is expanded.
 */
ZTEST(midi_parser, test_thru_passes_all_channels_as_whole_messages)
{
	const uint8_t stream[] = {0x95, 0x3c, 0x40, 0x3e, 0x41, 0xb0, 0x07, 0x64};
	const uint8_t expected[] = {0x95, 0x3c, 0x40, 0x95, 0x3e, 0x41, 0xb0, 0x07, 0x64};

	enable_thru();
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 3);
	zassert_equal(thru.len, sizeof(expected));
	zassert_mem_equal(thru.bytes, expected, sizeof(expected));
	zassert_equal(seen.cc_count, 1, "the DUO still only reacts to its own channel");
	zassert_equal(seen.note_on_count, 0);
}

/* Messages the DUO ignores still go through. */
ZTEST(midi_parser, test_thru_passes_messages_the_duo_ignores)
{
	const uint8_t stream[] = {
		0xc2, 0x05,             /* program change */
		0xe0, 0x00, 0x40,       /* pitch bend */
		0xf2, 0x10, 0x00,       /* song position */
		0xf3, 0x02,             /* song select */
		0xf1, 0x31,             /* MTC quarter frame */
		0xf6,                   /* tune request */
	};

	enable_thru();
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 6);
	zassert_equal(thru.len, sizeof(stream));
	zassert_mem_equal(thru.bytes, stream, sizeof(stream));
}

/* A real time byte inside a message is echoed on its own, and the message after it intact. */
ZTEST(midi_parser, test_thru_realtime_inside_message)
{
	const uint8_t stream[] = {0x90, 0x3c, 0xf8, 0x40};
	const uint8_t expected[] = {0xf8, 0x90, 0x3c, 0x40};

	enable_thru();
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 2);
	zassert_equal(thru.lens[0], 1);
	zassert_mem_equal(thru.bytes, expected, sizeof(expected));
	zassert_equal(seen.clock_count, 1);
	zassert_equal(seen.note_on_count, 1);
}

ZTEST(midi_parser, test_thru_sysex)
{
	const uint8_t stream[] = {0xf0, 0x7d, 0x64, 0x01, 0xf7};

	enable_thru();
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 1);
	zassert_mem_equal(thru.bytes, stream, sizeof(stream));
}

/* A sysex that did not fit is not passed on in pieces. */
ZTEST(midi_parser, test_thru_drops_oversized_sysex)
{
	enable_thru();

	parser.feed(0xf0, 1);
	for (unsigned i = 0; i < MidiParser::SYSEX_MAX; i++) {
		parser.feed(0x01, 1);
	}
	parser.feed(0xf7, 1);

	zassert_equal(thru.count, 0);
}

/* An unterminated sysex is not passed on either. */
ZTEST(midi_parser, test_thru_drops_interrupted_sysex)
{
	const uint8_t stream[] = {0xf0, 0x7d, 0x64, 0x90, 0x3c, 0x40};
	const uint8_t expected[] = {0x90, 0x3c, 0x40};

	enable_thru();
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 1);
	zassert_mem_equal(thru.bytes, expected, sizeof(expected));
}

/* Without a handler nothing changes, which is how the USB side runs. */
ZTEST(midi_parser, test_no_thru_by_default)
{
	const uint8_t stream[] = {0x90, 0x3c, 0x40};

	memset(&thru, 0, sizeof(thru));
	feed(stream, sizeof(stream), 1);

	zassert_equal(thru.count, 0);
	zassert_equal(seen.note_on_count, 1);
}
