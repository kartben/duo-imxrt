/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ump_convert.h"

#include <zephyr/sys/util.h>

#include <cstring>

namespace ump {

/* Data64 packet status nibbles, from the UMP specification. */
enum : uint8_t {
	SYSEX_COMPLETE = 0x0,
	SYSEX_START = 0x1,
	SYSEX_CONTINUE = 0x2,
	SYSEX_END = 0x3,
};

static constexpr size_t SYSEX_BYTES_PER_PACKET = 6;

/*
 * The UMP_* helper macros in <zephyr/audio/midi.h> build their packets with
 * braced initialisers that C++ rejects as narrowing, so packets are assembled
 * from the same field layout here instead.
 */
static struct midi_ump midi1_channel_voice(uint8_t group, uint8_t command, uint8_t channel,
					   uint8_t p1, uint8_t p2)
{
	struct midi_ump packet = {};

	packet.data[0] = ((uint32_t)UMP_MT_MIDI1_CHANNEL_VOICE << 28) |
			 ((uint32_t)(group & 0x0f) << 24) | ((uint32_t)(command & 0x0f) << 20) |
			 ((uint32_t)(channel & 0x0f) << 16) | ((uint32_t)(p1 & 0x7f) << 8) |
			 (uint32_t)(p2 & 0x7f);

	return packet;
}

static struct midi_ump sys_rt_common(uint8_t group, uint8_t status)
{
	struct midi_ump packet = {};

	packet.data[0] = ((uint32_t)UMP_MT_SYS_RT_COMMON << 28) |
			 ((uint32_t)(group & 0x0f) << 24) | ((uint32_t)status << 16);

	return packet;
}

static struct midi_ump data64(uint8_t status, const uint8_t *payload, size_t count)
{
	struct midi_ump packet = {};
	uint8_t bytes[SYSEX_BYTES_PER_PACKET] = {0};

	memcpy(bytes, payload, count);

	packet.data[0] = ((uint32_t)UMP_MT_DATA_64 << 28) | ((uint32_t)status << 20) |
			 ((uint32_t)count << 16) | ((uint32_t)bytes[0] << 8) | bytes[1];
	packet.data[1] = ((uint32_t)bytes[2] << 24) | ((uint32_t)bytes[3] << 16) |
			 ((uint32_t)bytes[4] << 8) | bytes[5];

	return packet;
}

static void sysex_from_midi1(const uint8_t *bytes, size_t len, PacketSink sink, void *user_data)
{
	/* Strip the 0xF0/0xF7 framing; UMP carries it in the packet status. */
	if (len < 2) {
		return;
	}

	bytes++;
	len -= 2;

	size_t offset = 0;

	do {
		const size_t chunk = MIN(SYSEX_BYTES_PER_PACKET, len - offset);
		uint8_t status;

		if (offset == 0 && chunk == len) {
			status = SYSEX_COMPLETE;
		} else if (offset == 0) {
			status = SYSEX_START;
		} else if (offset + chunk >= len) {
			status = SYSEX_END;
		} else {
			status = SYSEX_CONTINUE;
		}

		sink(data64(status, bytes + offset, chunk), user_data);

		offset += chunk;
	} while (offset < len);
}

void from_midi1(const uint8_t *bytes, size_t len, PacketSink sink, void *user_data)
{
	if (sink == nullptr || len == 0) {
		return;
	}

	if (bytes[0] == 0xf0) {
		sysex_from_midi1(bytes, len, sink, user_data);
		return;
	}

	if (bytes[0] >= 0xf0) {
		sink(sys_rt_common(0, bytes[0]), user_data);
		return;
	}

	if ((bytes[0] & 0x80) == 0) {
		/* A data byte with no status: nothing sensible to send. */
		return;
	}

	const uint8_t command = bytes[0] >> 4;
	const uint8_t channel = bytes[0] & 0x0f;
	const uint8_t p1 = len > 1 ? bytes[1] : 0;
	const uint8_t p2 = len > 2 ? bytes[2] : 0;

	sink(midi1_channel_voice(0, command, channel, p1, p2), user_data);
}

size_t to_midi1(const struct midi_ump &packet, RxState &state, uint8_t *out, size_t out_len)
{
	size_t len = 0;

	switch (UMP_MT(packet)) {
	case UMP_MT_MIDI1_CHANNEL_VOICE: {
		const uint8_t command = (uint8_t)UMP_MIDI_COMMAND(packet);
		const size_t wanted = (command == UMP_MIDI_PROGRAM_CHANGE ||
				       command == UMP_MIDI_CHAN_AFTERTOUCH)
					      ? 2
					      : 3;

		if (out_len < wanted) {
			return 0;
		}

		out[len++] = (uint8_t)UMP_MIDI_STATUS(packet);
		out[len++] = (uint8_t)((packet.data[0] >> 8) & 0x7f);

		if (wanted == 3) {
			out[len++] = (uint8_t)(packet.data[0] & 0x7f);
		}
		break;
	}
	case UMP_MT_SYS_RT_COMMON:
		if (out_len < 1) {
			return 0;
		}

		out[len++] = (uint8_t)UMP_MIDI_STATUS(packet);
		break;
	case UMP_MT_DATA_64: {
		/*
		 * Data64 carries sysex without its 0xF0/0xF7 framing. The
		 * framing is reinstated so the shared sysex handler sees the
		 * same layout as it does on the DIN input.
		 */
		const uint8_t status = (packet.data[0] >> 20) & 0x0f;
		const uint8_t count = MIN((uint8_t)((packet.data[0] >> 16) & 0x0f),
					  (uint8_t)SYSEX_BYTES_PER_PACKET);
		const bool starts = (status == SYSEX_COMPLETE || status == SYSEX_START);
		const bool ends = (status == SYSEX_COMPLETE || status == SYSEX_END);

		if (starts) {
			if (out_len < 1) {
				return 0;
			}

			out[len++] = 0xf0;
			state.sysex_started = true;
		} else if (!state.sysex_started) {
			/* Joined a transfer part way through; wait for a start. */
			return 0;
		}

		const uint8_t payload[SYSEX_BYTES_PER_PACKET] = {
			(uint8_t)((packet.data[0] >> 8) & 0x7f),
			(uint8_t)(packet.data[0] & 0x7f),
			(uint8_t)((packet.data[1] >> 24) & 0x7f),
			(uint8_t)((packet.data[1] >> 16) & 0x7f),
			(uint8_t)((packet.data[1] >> 8) & 0x7f),
			(uint8_t)(packet.data[1] & 0x7f),
		};

		for (uint8_t i = 0; i < count && len < out_len; i++) {
			out[len++] = payload[i];
		}

		if (ends) {
			if (len < out_len) {
				out[len++] = 0xf7;
			}
			state.sysex_started = false;
		}
		break;
	}
	default:
		break;
	}

	return len;
}

} /* namespace ump */
