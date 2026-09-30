# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(init.c)

if(CONFIG_NXP_IMXRT_BOOT_HEADER AND CONFIG_BOOT_FLEXSPI_NOR)
  zephyr_library_sources(xip/duo_brains2_flexspi_nor_config.c)
  zephyr_library_include_directories(xip)
endif()

# The DUO is updated by tools/updater, which writes images starting at the
# FlexSPI configuration block (0x60000400) the way the legacy build lays them
# out. zephyr.bin starts 1 KiB earlier, at the beginning of flash, so flashing
# it as is would leave the DUO without a boot header. Emit duo_firmware.bin in
# the updater's layout instead, running the updater's own checks on it, so an
# image the boot ROM would reject fails the build rather than a DUO.
#
# This lives with the board rather than the application because it has to be
# registered before the top-level Zephyr CMakeLists.txt collects post-build
# steps, and because it holds for any image built for the DUO.
if(CONFIG_BUILD_OUTPUT_BIN AND CONFIG_NXP_IMXRT_BOOT_HEADER)
  set(duo_firmware_bin ${PROJECT_BINARY_DIR}/duo_firmware.bin)

  set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND ${PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_LIST_DIR}/../../../../tools/updater/duo_image.py
      ${PROJECT_BINARY_DIR}/${KERNEL_BIN_NAME}
      -o ${duo_firmware_bin}
  )
  set_property(GLOBAL APPEND PROPERTY extra_post_build_byproducts ${duo_firmware_bin})
endif()
