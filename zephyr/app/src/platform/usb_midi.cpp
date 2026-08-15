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

#include "ump_convert.h"

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
static ump::RxState rx_state;

static void push_rx(const uint8_t *bytes, size_t len)
{
	if (ring_buf_put(&usb_rx_rb, bytes, len) < len) {
		LOG_WRN("USB MIDI RX buffer full");
	}
}

static void on_ump_received(const struct device *dev, const struct midi_ump packet)
{
	uint8_t bytes[ump::MAX_MIDI1_BYTES];

	ARG_UNUSED(dev);

	const size_t len = ump::to_midi1(packet, rx_state, bytes, sizeof(bytes));

	if (len > 0) {
		push_rx(bytes, len);
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

static void send_packet(const struct midi_ump &packet, void *user_data)
{
	ARG_UNUSED(user_data);

	usbd_midi_send(usb_midi_dev, packet);
}

void usb_midi_send_bytes(const uint8_t *bytes, size_t len)
{
	if (!interface_ready) {
		return;
	}

	ump::from_midi1(bytes, len, send_packet, NULL);
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
