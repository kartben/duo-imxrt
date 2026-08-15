/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reset and bootloader entry, ported from BOARD_EnterROMBootloader() in
 * brains2/core/lib/board.c.
 */

#include "power.h"

#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

/*
 * The boot ROM publishes a table of entry points at a fixed address; the third
 * word is the bootloader entry function.
 */
struct bootloader_api_entry {
	const uint32_t version;
	const char *copyright;
	void (*run_bootloader)(void *arg);
	const uint32_t *reserved0;
	const uint32_t *reserved1;
};

#define BOOTLOADER_TREE (*(struct bootloader_api_entry **)(0x0020001cU))

void power_enter_rom_bootloader(void)
{
	/* 0xEB = enter bootloader, 1 = serial downloader mode. */
	uint32_t arg = 0xEB100000;

	irq_lock();
	BOOTLOADER_TREE->run_bootloader(&arg);

	/* Not reached. */
	for (;;) {
	}
}

void power_reset(void)
{
	sys_reboot(SYS_REBOOT_WARM);
}

void power_read_device_id(uint32_t *high, uint32_t *low)
{
	/*
	 * Read the OCOTP configuration fuses directly rather than through
	 * hwinfo: hwinfo reports CFG2/CFG1 byte-swapped to big endian, while
	 * the legacy firmware reports CFG0/CFG1 as read. Going through hwinfo
	 * would give the same unit a different serial number over sysex than
	 * it reports today.
	 */
	*high = OCOTP->CFG0;
	*low = OCOTP->CFG1;
}
