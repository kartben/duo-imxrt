/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Translation between MIDI 1.0 byte streams and Universal MIDI Packets.
 *
 * Kept separate from the USB transport in usb_midi.cpp so that it can be
 * exercised without bringing up the USB device stack; see zephyr/tests/midi.
 */

#pragma once

#include <zephyr/audio/midi.h>

#include <cstddef>
#include <cstdint>

namespace ump {

/*
 * Longest MIDI 1.0 byte string a single packet can expand into: a Data64
 * sysex packet carrying six payload bytes plus its 0xF0 and 0xF7 framing.
 */
static constexpr size_t MAX_MIDI1_BYTES = 8;

/* Called once per packet produced from a MIDI 1.0 message. */
typedef void (*PacketSink)(const struct midi_ump &packet, void *user_data);

/*
 * Translates one complete MIDI 1.0 message into Universal MIDI Packets.
 * Sysex longer than six payload bytes is split across start, continue and
 * end packets.
 */
void from_midi1(const uint8_t *bytes, size_t len, PacketSink sink, void *user_data);

/* Sysex spans several packets, so reassembly needs state between calls. */
struct RxState {
	bool sysex_started = false;
};

/*
 * Translates one Universal MIDI Packet back into MIDI 1.0 bytes, writing at
 * most MAX_MIDI1_BYTES. Returns the number of bytes written, which is zero for
 * packet types the DUO does not use and for sysex continuations whose start
 * packet was missed.
 */
size_t to_midi1(const struct midi_ump &packet, RxState &state, uint8_t *out, size_t out_len);

} /* namespace ump */
