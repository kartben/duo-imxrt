# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(init.c)

if(CONFIG_NXP_IMXRT_BOOT_HEADER AND CONFIG_BOOT_FLEXSPI_NOR)
  zephyr_library_sources(xip/duo_brains2_flexspi_nor_config.c)
  zephyr_library_include_directories(xip)
endif()
