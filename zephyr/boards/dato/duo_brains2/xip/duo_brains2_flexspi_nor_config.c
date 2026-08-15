/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * FlexSPI boot configuration block for the Winbond W25Q-series QSPI NOR the
 * Brains 2 boots from. Transcribed from the MCUXpresso-SDK firmware in
 * brains2/sdk/xip/duobrains2_flexspi_nor_config.c so that both firmwares
 * present the boot ROM with identical flash parameters.
 */

#include <zephyr/kernel.h>
#include <flexspi_nor_config.h>

#if defined(CONFIG_NXP_IMXRT_BOOT_HEADER) && defined(CONFIG_BOOT_FLEXSPI_NOR)

__attribute__((section(".boot_hdr.conf"), used))
const struct flexspi_nor_config_t duo_brains2_flash_config = {
	.mem_config = {
		.tag = FLEXSPI_CFG_BLK_TAG,
		.version = FLEXSPI_CFG_BLK_VERSION,
		.read_sample_clk_src = FLEXSPI_READ_SAMPLE_CLK_LOOPBACK_INTERNALLY,
		.cs_hold_time = 1u,
		.cs_setup_time = 2u,
		.device_type = FLEXSPI_DEVICE_TYPE_SERIAL_NOR,
		.sflash_pad_type = SERIAL_FLASH_4_PADS,
		.serial_clk_freq = FLEXSPI_SERIAL_CLK_133MHZ,
		.sflash_a1_size = 16u * 1024u * 1024u,
		.lookup_table = {
			/* 0: Quad read (0xEB), 6 dummy cycles */
			FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0xEB,
					RADDR_SDR, FLEXSPI_4PAD, 0x18),
			FLEXSPI_LUT_SEQ(DUMMY_SDR, FLEXSPI_4PAD, 0x06,
					READ_SDR, FLEXSPI_4PAD, 0x04),

			/* 1: Read status register */
			[4 * 1 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x05,
						      READ_SDR, FLEXSPI_1PAD, 0x02),

			/* 3: Write enable */
			[4 * 3 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x06,
						      STOP, FLEXSPI_1PAD, 0x0),

			/* 5: Erase sector (4 KiB) */
			[4 * 5 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x20,
						      RADDR_SDR, FLEXSPI_1PAD, 0x18),

			/* 8: Erase block (64 KiB) */
			[4 * 8 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0xD8,
						      RADDR_SDR, FLEXSPI_1PAD, 0x18),

			/* 9: Page program */
			[4 * 9 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x02,
						      RADDR_SDR, FLEXSPI_1PAD, 0x18),
			[4 * 9 + 1] = FLEXSPI_LUT_SEQ(WRITE_SDR, FLEXSPI_1PAD, 0x04,
						      STOP, FLEXSPI_1PAD, 0x0),

			/* 11: Chip erase */
			[4 * 11 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x60,
						       STOP, FLEXSPI_1PAD, 0x0),
		},
	},
	.page_size = 256u,
	.sector_size = 4u * 1024u,
	.ipcmd_serial_clk_freq = 1u,
	.block_size = 64u * 1024u,
	.is_uniform_block_size = false,
};

#endif /* CONFIG_NXP_IMXRT_BOOT_HEADER && CONFIG_BOOT_FLEXSPI_NOR */
