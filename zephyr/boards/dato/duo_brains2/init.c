/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>

void SystemInitHook(void)
{
#if DT_SAME_NODE(DT_NODELABEL(flexspi), DT_PARENT(DT_CHOSEN(zephyr_flash_controller)))
	/*
	 * Make the FlexSPI fetch more data than each AHB burst strictly needs,
	 * so that reads always meet the flash's alignment requirements. Without
	 * this the controller returns corrupted data during early boot, before
	 * the instruction cache is enabled, and the CPU faults.
	 *
	 * Carried over from BOARD_ConfigMPU() in brains2/core/lib/board.c.
	 */
	FLEXSPI->AHBCR |= FLEXSPI_AHBCR_READADDROPT_MASK;
#endif
}
