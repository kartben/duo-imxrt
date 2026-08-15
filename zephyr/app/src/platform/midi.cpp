/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIDI transport layer, ported from brains2/core/lib/midi_wrapper.cpp.
 *
 * Like the legacy firmware, every outgoing message goes to both transports at
 * once - the DIN jacks on LPUART1 and USB - and both are polled for input.
 * On the USB side the Arduino USB-MIDI bridge is replaced by Zephyr's USB
 * MIDI 2.0 class, so MIDI 1.0 byte streams are translated to and from
 * Universal MIDI Packets here.
 */

#include "../compat/lib/midi_wrapper.h"
#include "midi_parser.h"
#include "usb_midi.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(duo_midi, CONFIG_DUO_LOG_LEVEL);

static const struct device *const midi_uart = DEVICE_DT_GET(DT_ALIAS(midi_uart));

RING_BUF_DECLARE(uart_rx_rb, CONFIG_DUO_MIDI_RX_BUF_SIZE);
RING_BUF_DECLARE(uart_tx_rb, CONFIG_DUO_MIDI_TX_BUF_SIZE);

static MidiParser serial_parser;
static MidiParser usb_parser;

/* Set once the UART is up; guards against sends during early boot. */
static bool serial_ready;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[16];
			int len = uart_fifo_read(dev, buf, sizeof(buf));

			if (len > 0) {
				ring_buf_put(&uart_rx_rb, buf, len);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *tx;
			uint32_t claimed = ring_buf_get_claim(&uart_tx_rb, &tx, 16);

			if (claimed == 0) {
				uart_irq_tx_disable(dev);
				ring_buf_get_finish(&uart_tx_rb, 0);
				continue;
			}

			int sent = uart_fifo_fill(dev, tx, claimed);

			ring_buf_get_finish(&uart_tx_rb, MAX(sent, 0));
		}
	}
}

static void serial_write(const uint8_t *bytes, size_t len)
{
	/*
	 * At 31250 baud a three byte message takes about a millisecond, so
	 * transmission is interrupt driven; the audio and UI loops must not
	 * block on it. If the buffer is full the message is dropped rather
	 * than stalling the sequencer.
	 */
	if (!serial_ready) {
		return;
	}

	uint32_t written = ring_buf_put(&uart_tx_rb, bytes, len);

	if (written < len) {
		LOG_WRN("MIDI TX buffer full, dropped %u bytes", (unsigned)(len - written));
	}

	uart_irq_tx_enable(midi_uart);
}

static void send_to_all(const uint8_t *bytes, size_t len)
{
	serial_write(bytes, len);
	usb_midi_send_bytes(bytes, len);
}

namespace MIDI {

void init(const Callbacks &callbacks)
{
	serial_parser.set_callbacks(callbacks);
	usb_parser.set_callbacks(callbacks);

	if (!device_is_ready(midi_uart)) {
		LOG_ERR("MIDI UART not ready");
		return;
	}

	uart_irq_callback_user_data_set(midi_uart, uart_isr, NULL);
	uart_irq_rx_enable(midi_uart);
	serial_ready = true;

	usb_midi_init();
}

void read(byte channel)
{
	uint8_t buf[32];
	uint32_t len;

	if (!serial_ready) {
		return;
	}

	while ((len = ring_buf_get(&uart_rx_rb, buf, sizeof(buf))) > 0) {
		for (uint32_t i = 0; i < len; i++) {
			serial_parser.feed(buf[i], channel);
		}
	}

	while ((len = usb_midi_get_bytes(buf, sizeof(buf))) > 0) {
		for (uint32_t i = 0; i < len; i++) {
			usb_parser.feed(buf[i], channel);
		}
	}
}

void sendRealTime(midi::MidiType message)
{
	const uint8_t status = (uint8_t)message;

	send_to_all(&status, 1);
}

void sendControlChange(byte cc, byte value, byte channel)
{
	const uint8_t msg[] = {(uint8_t)(midi::ControlChange | ((channel - 1) & 0x0f)),
			       (uint8_t)(cc & 0x7f), (uint8_t)(value & 0x7f)};

	send_to_all(msg, sizeof(msg));
}

void sendNoteOn(byte inNoteNumber, byte inVelocity, byte inChannel)
{
	const uint8_t msg[] = {(uint8_t)(midi::NoteOn | ((inChannel - 1) & 0x0f)),
			       (uint8_t)(inNoteNumber & 0x7f), (uint8_t)(inVelocity & 0x7f)};

	send_to_all(msg, sizeof(msg));
}

void sendNoteOff(byte inNoteNumber, byte inVelocity, byte inChannel)
{
	const uint8_t msg[] = {(uint8_t)(midi::NoteOff | ((inChannel - 1) & 0x0f)),
			       (uint8_t)(inNoteNumber & 0x7f), (uint8_t)(inVelocity & 0x7f)};

	send_to_all(msg, sizeof(msg));
}

void sendSysEx(unsigned length, const byte *bytes)
{
	send_to_all(bytes, length);
}

} /* namespace MIDI */
