/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB MIDI transport. The legacy firmware carries its own TinyUSB stack and
 * descriptor set (brains2/core/lib/usb); here Zephyr's USB device stack and
 * USB MIDI 2.0 class do that work, and this file only translates between
 * Universal MIDI Packets and the MIDI 1.0 byte stream the rest of the
 * firmware speaks.
 *
 * The vendor and product IDs, and the descriptor strings, are deliberately the
 * same as in brains2/core/lib/usb/usb_descriptors.c so that hosts - and the
 * updater in tools/updater - see the same device.
 */

#include "usb_midi.h"

#include <zephyr/audio/midi.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(duo_usb_midi, CONFIG_DUO_LOG_LEVEL);

#define USB_MIDI_NODE DT_NODELABEL(usb_midi)

static const struct device *const usb_midi_dev = DEVICE_DT_GET(USB_MIDI_NODE);

USBD_DEVICE_DEFINE(duo_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_DUO_USB_VID, CONFIG_DUO_USB_PID);

USBD_DESC_LANG_DEFINE(duo_lang);
USBD_DESC_MANUFACTURER_DEFINE(duo_mfr, CONFIG_DUO_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(duo_product, CONFIG_DUO_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(duo_sn);
USBD_DESC_CONFIG_DEFINE(duo_fs_cfg_desc, "DUO");

USBD_CONFIGURATION_DEFINE(duo_fs_config, USB_SCD_SELF_POWERED, 250, &duo_fs_cfg_desc);

RING_BUF_DECLARE(usb_rx_rb, CONFIG_DUO_MIDI_RX_BUF_SIZE);

static volatile bool interface_ready;

/* Sysex reassembly state for incoming Data64 packets. */
static uint8_t sysex_started;

/*
 * The UMP_* helper macros in <zephyr/audio/midi.h> build their packets with
 * braced initialisers that C++ rejects as narrowing, so packets are assembled
 * from the same field layout here instead.
 */
static struct midi_ump ump_midi1_channel_voice(uint8_t group, uint8_t command, uint8_t channel,
					       uint8_t p1, uint8_t p2)
{
	struct midi_ump ump = {};

	ump.data[0] = ((uint32_t)UMP_MT_MIDI1_CHANNEL_VOICE << 28) |
		      ((uint32_t)(group & 0x0f) << 24) | ((uint32_t)(command & 0x0f) << 20) |
		      ((uint32_t)(channel & 0x0f) << 16) | ((uint32_t)(p1 & 0x7f) << 8) |
		      (uint32_t)(p2 & 0x7f);

	return ump;
}

static struct midi_ump ump_sys_rt_common(uint8_t group, uint8_t status)
{
	struct midi_ump ump = {};

	ump.data[0] = ((uint32_t)UMP_MT_SYS_RT_COMMON << 28) |
		      ((uint32_t)(group & 0x0f) << 24) | ((uint32_t)status << 16);

	return ump;
}

static void push_rx(const uint8_t *bytes, size_t len)
{
	if (ring_buf_put(&usb_rx_rb, bytes, len) < len) {
		LOG_WRN("USB MIDI RX buffer full");
	}
}

static void on_ump_received(const struct device *dev, const struct midi_ump ump)
{
	ARG_UNUSED(dev);

	switch (UMP_MT(ump)) {
	case UMP_MT_MIDI1_CHANNEL_VOICE: {
		const uint8_t status = (uint8_t)UMP_MIDI_STATUS(ump);
		const uint8_t command = (uint8_t)UMP_MIDI_COMMAND(ump);
		uint8_t msg[3] = {status, (uint8_t)((ump.data[0] >> 8) & 0x7f),
				  (uint8_t)(ump.data[0] & 0x7f)};
		size_t len = 3;

		if (command == UMP_MIDI_PROGRAM_CHANGE ||
		    command == UMP_MIDI_CHAN_AFTERTOUCH) {
			len = 2;
		}

		push_rx(msg, len);
		break;
	}
	case UMP_MT_SYS_RT_COMMON: {
		const uint8_t status = (uint8_t)UMP_MIDI_STATUS(ump);

		push_rx(&status, 1);
		break;
	}
	case UMP_MT_DATA_64: {
		/*
		 * Data64 carries sysex without its 0xF0/0xF7 framing, split
		 * over up to six payload bytes per packet. The framing is
		 * reinstated so the shared sysex handler sees the same layout
		 * as it does on the DIN input.
		 */
		const uint8_t status = (ump.data[0] >> 20) & 0x0f;
		const uint8_t nbytes = (ump.data[0] >> 16) & 0x0f;
		uint8_t payload[8];
		size_t len = 0;

		if (status == 0x0 || status == 0x1) {
			const uint8_t start = 0xf0;

			push_rx(&start, 1);
			sysex_started = 1;
		}

		if (!sysex_started) {
			/* Joined a transfer part way through; wait for a start. */
			break;
		}

		payload[len++] = (ump.data[0] >> 8) & 0x7f;
		payload[len++] = ump.data[0] & 0x7f;
		payload[len++] = (ump.data[1] >> 24) & 0x7f;
		payload[len++] = (ump.data[1] >> 16) & 0x7f;
		payload[len++] = (ump.data[1] >> 8) & 0x7f;
		payload[len++] = ump.data[1] & 0x7f;

		push_rx(payload, MIN((size_t)nbytes, len));

		if (status == 0x0 || status == 0x3) {
			const uint8_t end = 0xf7;

			push_rx(&end, 1);
			sysex_started = 0;
		}
		break;
	}
	default:
		break;
	}
}

static void on_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);

	interface_ready = ready;
}

static const struct usbd_midi_ops midi_ops = {
	.rx_packet_cb = on_ump_received,
	.ready_cb = on_ready,
};

bool usb_midi_ready(void)
{
	return interface_ready;
}

size_t usb_midi_get_bytes(uint8_t *buf, size_t len)
{
	return ring_buf_get(&usb_rx_rb, buf, len);
}

static void send_sysex(const uint8_t *bytes, size_t len)
{
	/* Strip the 0xF0/0xF7 framing; UMP carries it in the packet status. */
	if (len < 2) {
		return;
	}

	bytes++;
	len -= 2;

	size_t offset = 0;

	do {
		const size_t chunk = MIN((size_t)6, len - offset);
		uint8_t status;

		if (offset == 0 && chunk == len) {
			status = 0x0; /* complete in one packet */
		} else if (offset == 0) {
			status = 0x1; /* start */
		} else if (offset + chunk >= len) {
			status = 0x3; /* end */
		} else {
			status = 0x2; /* continue */
		}

		uint8_t payload[6] = {0};

		memcpy(payload, bytes + offset, chunk);

		struct midi_ump ump = {};

		ump.data[0] = ((uint32_t)UMP_MT_DATA_64 << 28) | ((uint32_t)status << 20) |
			      ((uint32_t)chunk << 16) | ((uint32_t)payload[0] << 8) | payload[1];
		ump.data[1] = ((uint32_t)payload[2] << 24) | ((uint32_t)payload[3] << 16) |
			      ((uint32_t)payload[4] << 8) | payload[5];

		usbd_midi_send(usb_midi_dev, ump);

		offset += chunk;
	} while (offset < len);
}

void usb_midi_send_bytes(const uint8_t *bytes, size_t len)
{
	if (!interface_ready || len == 0) {
		return;
	}

	if (bytes[0] == 0xf0) {
		send_sysex(bytes, len);
		return;
	}

	if (bytes[0] >= 0xf0) {
		usbd_midi_send(usb_midi_dev, ump_sys_rt_common(0, bytes[0]));
		return;
	}

	if ((bytes[0] & 0x80) == 0) {
		return;
	}

	const uint8_t command = bytes[0] >> 4;
	const uint8_t channel = bytes[0] & 0x0f;
	const uint8_t p1 = len > 1 ? bytes[1] : 0;
	const uint8_t p2 = len > 2 ? bytes[2] : 0;

	usbd_midi_send(usb_midi_dev, ump_midi1_channel_voice(0, command, channel, p1, p2));
}

void usb_midi_disconnect(void)
{
	usbd_disable(&duo_usbd);
	interface_ready = false;
}

int usb_midi_init(void)
{
	int err;

	if (!device_is_ready(usb_midi_dev)) {
		LOG_ERR("USB MIDI device not ready");
		return -ENODEV;
	}

	usbd_midi_set_ops(usb_midi_dev, &midi_ops);

	err = usbd_add_descriptor(&duo_usbd, &duo_lang);
	if (err == 0) {
		err = usbd_add_descriptor(&duo_usbd, &duo_mfr);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&duo_usbd, &duo_product);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&duo_usbd, &duo_sn);
	}
	if (err) {
		LOG_ERR("failed to add USB descriptors (%d)", err);
		return err;
	}

	err = usbd_add_configuration(&duo_usbd, USBD_SPEED_FS, &duo_fs_config);
	if (err) {
		LOG_ERR("failed to add USB configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&duo_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("failed to register USB classes (%d)", err);
		return err;
	}

	/*
	 * The MIDI 2.0 class spans several interfaces, so the device must
	 * advertise an Interface Association Descriptor.
	 */
	usbd_device_set_code_triple(&duo_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_self_powered(&duo_usbd, true);

	err = usbd_init(&duo_usbd);
	if (err) {
		LOG_ERR("failed to initialise USB device (%d)", err);
		return err;
	}

	err = usbd_enable(&duo_usbd);
	if (err) {
		LOG_ERR("failed to enable USB device (%d)", err);
		return err;
	}

	return 0;
}
