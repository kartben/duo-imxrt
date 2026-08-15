/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the translation between MIDI 1.0 byte streams and Universal MIDI
 * Packets. The DUO speaks MIDI 1.0 internally and only meets UMP at the USB
 * boundary, so anything that survives a round trip through this layer behaves
 * identically on both transports.
 */

#include "platform/ump_convert.h"

#include <zephyr/ztest.h>

#include <cstring>

#define MAX_PACKETS 8

struct capture {
	size_t count;
	struct midi_ump packets[MAX_PACKETS];
};

static void collect(const struct midi_ump &packet, void *user_data)
{
	struct capture *c = (struct capture *)user_data;

	if (c->count < MAX_PACKETS) {
		c->packets[c->count] = packet;
	}

	c->count++;
}

static struct capture encode(const uint8_t *bytes, size_t len)
{
	struct capture c = {};

	ump::from_midi1(bytes, len, collect, &c);
	return c;
}

/* Feeds a MIDI 1.0 message through UMP and back, returning what came out. */
static size_t round_trip(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	struct capture c = encode(in, in_len);
	ump::RxState state = {};
	size_t total = 0;

	for (size_t i = 0; i < c.count && i < MAX_PACKETS; i++) {
		uint8_t chunk[ump::MAX_MIDI1_BYTES];
		const size_t len = ump::to_midi1(c.packets[i], state, chunk, sizeof(chunk));

		for (size_t j = 0; j < len && total < out_len; j++) {
			out[total++] = chunk[j];
		}
	}

	return total;
}

ZTEST_SUITE(ump_convert, NULL, NULL, NULL, NULL, NULL);

ZTEST(ump_convert, test_note_on_encodes_to_one_packet)
{
	const uint8_t msg[] = {0x91, 0x3c, 0x64};
	struct capture c = encode(msg, sizeof(msg));

	zassert_equal(c.count, 1, "a channel voice message is one packet");
	zassert_equal(UMP_MT(c.packets[0]), UMP_MT_MIDI1_CHANNEL_VOICE);
	zassert_equal(UMP_MIDI_COMMAND(c.packets[0]), UMP_MIDI_NOTE_ON);
	zassert_equal(UMP_MIDI_CHANNEL(c.packets[0]), 1, "channel is zero-based on the wire");
	zassert_equal(UMP_MIDI1_P1(c.packets[0]), 0x3c);
	zassert_equal(UMP_MIDI1_P2(c.packets[0]), 0x64);
}

ZTEST(ump_convert, test_realtime_encodes_to_system_packet)
{
	const uint8_t msg[] = {0xf8};
	struct capture c = encode(msg, sizeof(msg));

	zassert_equal(c.count, 1);
	zassert_equal(UMP_MT(c.packets[0]), UMP_MT_SYS_RT_COMMON);
	zassert_equal(UMP_MIDI_STATUS(c.packets[0]), 0xf8);
}

ZTEST(ump_convert, test_short_sysex_is_a_single_packet)
{
	/* Four payload bytes fit in one Data64 packet. */
	const uint8_t msg[] = {0xf0, 0x7d, 0x64, 0x01, 0x02, 0xf7};
	struct capture c = encode(msg, sizeof(msg));

	zassert_equal(c.count, 1, "four payload bytes fit one packet");
	zassert_equal(UMP_MT(c.packets[0]), UMP_MT_DATA_64);
	zassert_equal((c.packets[0].data[0] >> 20) & 0x0f, 0x0, "status should be 'complete'");
	zassert_equal((c.packets[0].data[0] >> 16) & 0x0f, 4, "four payload bytes");
}

ZTEST(ump_convert, test_long_sysex_is_split_across_packets)
{
	/* 14 payload bytes: start (6), continue (6), end (2). */
	uint8_t msg[16];

	msg[0] = 0xf0;
	for (int i = 0; i < 14; i++) {
		msg[1 + i] = i;
	}
	msg[15] = 0xf7;

	struct capture c = encode(msg, sizeof(msg));

	zassert_equal(c.count, 3, "14 payload bytes need three packets");
	zassert_equal((c.packets[0].data[0] >> 20) & 0x0f, 0x1, "first packet is 'start'");
	zassert_equal((c.packets[1].data[0] >> 20) & 0x0f, 0x2, "middle packet is 'continue'");
	zassert_equal((c.packets[2].data[0] >> 20) & 0x0f, 0x3, "last packet is 'end'");
	zassert_equal((c.packets[2].data[0] >> 16) & 0x0f, 2, "two bytes left over");
}

ZTEST(ump_convert, test_round_trip_note_on)
{
	const uint8_t msg[] = {0x91, 0x3c, 0x64};
	uint8_t out[8];

	const size_t len = round_trip(msg, sizeof(msg), out, sizeof(out));

	zassert_equal(len, sizeof(msg));
	zassert_mem_equal(out, msg, sizeof(msg));
}

ZTEST(ump_convert, test_round_trip_program_change_is_two_bytes)
{
	/* Program change carries one parameter, not two. */
	const uint8_t msg[] = {0xc0, 0x07};
	uint8_t out[8];

	const size_t len = round_trip(msg, sizeof(msg), out, sizeof(out));

	zassert_equal(len, 2, "program change must not gain a third byte");
	zassert_mem_equal(out, msg, sizeof(msg));
}

ZTEST(ump_convert, test_round_trip_realtime)
{
	const uint8_t msg[] = {0xfa};
	uint8_t out[8];

	const size_t len = round_trip(msg, sizeof(msg), out, sizeof(out));

	zassert_equal(len, 1);
	zassert_equal(out[0], 0xfa);
}

ZTEST(ump_convert, test_round_trip_short_sysex)
{
	const uint8_t msg[] = {0xf0, 0x7d, 0x64, 0x01, 0xf7};
	uint8_t out[16];

	const size_t len = round_trip(msg, sizeof(msg), out, sizeof(out));

	zassert_equal(len, sizeof(msg));
	zassert_mem_equal(out, msg, sizeof(msg));
}

/* The serial number reply is 24 bytes, the longest sysex the DUO sends. */
ZTEST(ump_convert, test_round_trip_long_sysex)
{
	uint8_t msg[24];
	uint8_t out[32];

	msg[0] = 0xf0;
	for (int i = 1; i < 23; i++) {
		msg[i] = i & 0x7f;
	}
	msg[23] = 0xf7;

	const size_t len = round_trip(msg, sizeof(msg), out, sizeof(out));

	zassert_equal(len, sizeof(msg), "round trip changed the length");
	zassert_mem_equal(out, msg, sizeof(msg), "round trip changed the payload");
}

/*
 * A receiver that starts listening mid-transfer would otherwise inject bare
 * data bytes into the stream, which the parser would attribute to whatever
 * running status was latched.
 */
ZTEST(ump_convert, test_sysex_continuation_without_start_is_dropped)
{
	struct midi_ump packet = {};
	ump::RxState state = {};
	uint8_t out[ump::MAX_MIDI1_BYTES];

	/* A 'continue' packet carrying six bytes, with no preceding 'start'. */
	packet.data[0] = ((uint32_t)UMP_MT_DATA_64 << 28) | (0x2u << 20) | (6u << 16);
	packet.data[1] = 0;

	const size_t len = ump::to_midi1(packet, state, out, sizeof(out));

	zassert_equal(len, 0, "continuation without a start must produce nothing");
}

ZTEST(ump_convert, test_unknown_packet_type_is_ignored)
{
	struct midi_ump packet = {};
	ump::RxState state = {};
	uint8_t out[ump::MAX_MIDI1_BYTES];

	/* Message type 5 is a 128-bit data message the DUO does not use. */
	packet.data[0] = 0x5u << 28;

	zassert_equal(ump::to_midi1(packet, state, out, sizeof(out)), 0);
}

ZTEST(ump_convert, test_stray_data_byte_produces_nothing)
{
	const uint8_t msg[] = {0x3c};

	zassert_equal(encode(msg, sizeof(msg)).count, 0);
	zassert_equal(encode(NULL, 0).count, 0);
}
