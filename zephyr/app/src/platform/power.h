/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

/* Reboots into the i.MX RT serial downloader, which the updater talks to. */
void power_enter_rom_bootloader(void);

/* Warm reset, used to bring the DUO back up after a soft power off. */
void power_reset(void);

/* Reads the SoC unique ID, reported over sysex as the DUO's serial number. */
void power_read_device_id(uint32_t *high, uint32_t *low);
