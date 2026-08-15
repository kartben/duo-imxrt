/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB MIDI transport, presented to the rest of the firmware as a MIDI 1.0 byte
 * stream so that it can share the parser with the DIN jacks.
 */

#pragma once

#include <cstddef>
#include <cstdint>

int usb_midi_init(void);

/* Translates a MIDI 1.0 byte stream into Universal MIDI Packets and sends it. */
void usb_midi_send_bytes(const uint8_t *bytes, size_t len);

/* Reads back received traffic, already flattened to a MIDI 1.0 byte stream. */
size_t usb_midi_get_bytes(uint8_t *buf, size_t len);

/* True once the host has enabled the USB MIDI interface. */
bool usb_midi_ready(void);

/* Detaches from the bus, so the DUO can power down without the host complaining. */
void usb_midi_disconnect(void);
