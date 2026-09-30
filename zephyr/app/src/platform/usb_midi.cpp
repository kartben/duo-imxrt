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

#include "build_info.h"
#include "power.h"
#include "ump_convert.h"

#include <cstdio>
#include <cstring>

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

/*
 * String descriptors, in the legacy firmware's order: tools/updater reads the
 * serial number and the build description from indices 3 to 7. Zephyr numbers
 * strings in the order they are added, so usb_midi_init() adds them in the
 * order listed here, and nothing else in the device has a string.
 */
USBD_DESC_LANG_DEFINE(duo_lang);
USBD_DESC_MANUFACTURER_DEFINE(duo_mfr, CONFIG_DUO_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(duo_product, CONFIG_DUO_USB_PRODUCT);
USBD_DESC_STRING_DEFINE(duo_tag, DUO_GIT_TAG, USBD_DUT_STRING_INTERFACE);
USBD_DESC_STRING_DEFINE(duo_branch, DUO_GIT_BRANCH, USBD_DUT_STRING_INTERFACE);
USBD_DESC_STRING_DEFINE(duo_hash, DUO_GIT_HASH, USBD_DUT_STRING_INTERFACE);
USBD_DESC_STRING_DEFINE(duo_board, DUO_BOARD_NAME, USBD_DUT_STRING_INTERFACE);

/*
 * The serial number is the legacy firmware's rendering of the SoC's unique ID
 * ("%li-%li" of OCOTP CFG0 and CFG1) rather than Zephyr's hwinfo hex string,
 * so a DUO keeps the identity hosts already know it by across the switch.
 */
static char serial_ascii[24];
static struct usbd_desc_node duo_sn = {
	.str = {
		.utype = USBD_DUT_STRING_SERIAL_NUMBER,
		.ascii7 = true,
	},
	.ptr = serial_ascii,
	.bDescriptorType = USB_DESC_STRING,
};

static struct usbd_desc_node *const string_descriptors[] = {
	&duo_lang, &duo_mfr, &duo_product, &duo_sn,
	&duo_tag, &duo_branch, &duo_hash, &duo_board,
};

/* Bus powered, up to 500 mA, as the legacy firmware declares. */
USBD_CONFIGURATION_DEFINE(duo_fs_config, 0, 250, NULL);
USBD_CONFIGURATION_DEFINE(duo_hs_config, 0, 250, NULL);

/* bcdDevice and bus-attach delay of the legacy firmware. */
static const uint16_t USB_PRODUCT_RELEASE = 0x0002;
static const uint32_t USB_STARTUP_DELAY_MS = 280;

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

static int add_configuration(const enum usbd_speed speed, struct usbd_config_node *const config)
{
	int err;

	err = usbd_add_configuration(&duo_usbd, speed, config);
	if (err) {
		LOG_ERR("failed to add USB configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&duo_usbd, speed, 1, NULL);
	if (err) {
		LOG_ERR("failed to register USB classes (%d)", err);
		return err;
	}

	/*
	 * The MIDI 2.0 class spans several interfaces, so the device must
	 * advertise an Interface Association Descriptor.
	 */
	usbd_device_set_code_triple(&duo_usbd, speed, USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	return 0;
}

int usb_midi_init(void)
{
	int err;

	if (!device_is_ready(usb_midi_dev)) {
		LOG_ERR("USB MIDI device not ready");
		return -ENODEV;
	}

	usbd_midi_set_ops(usb_midi_dev, &midi_ops);

	uint32_t id_high;
	uint32_t id_low;

	power_read_device_id(&id_high, &id_low);
	snprintf(serial_ascii, sizeof(serial_ascii), "%ld-%ld", (long)(int32_t)id_high,
		 (long)(int32_t)id_low);
	duo_sn.bLength = 2 + 2 * strlen(serial_ascii);

	for (size_t i = 0; i < ARRAY_SIZE(string_descriptors); i++) {
		err = usbd_add_descriptor(&duo_usbd, string_descriptors[i]);
		if (err) {
			LOG_ERR("failed to add USB string descriptor %u (%d)", i, err);
			return err;
		}
	}

	/*
	 * The i.MX RT USB controller runs at high speed whenever the host
	 * does, and a device with only a full-speed configuration then has
	 * nothing to offer and fails enumeration. Offer the MIDI function at
	 * both speeds.
	 */
	if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&duo_usbd) == USBD_SPEED_HS) {
		err = add_configuration(USBD_SPEED_HS, &duo_hs_config);
		if (err) {
			return err;
		}
	}

	err = add_configuration(USBD_SPEED_FS, &duo_fs_config);
	if (err) {
		return err;
	}

	usbd_device_set_bcd_device(&duo_usbd, USB_PRODUCT_RELEASE);

	err = usbd_init(&duo_usbd);
	if (err) {
		LOG_ERR("failed to initialise USB device (%d)", err);
		return err;
	}

	/*
	 * The legacy firmware keeps off the bus until 280 ms after reset
	 * (USB_STARTUP_DELAY_MS). Why is not recorded, so the port keeps it.
	 */
	k_sleep(K_TIMEOUT_ABS_MS(USB_STARTUP_DELAY_MS));

	err = usbd_enable(&duo_usbd);
	if (err) {
		LOG_ERR("failed to enable USB device (%d)", err);
		return err;
	}

	return 0;
}
