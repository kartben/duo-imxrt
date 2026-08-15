# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(duo_dsp_tests LANGUAGES C CXX)

set(DUO_APP_SRC ${CMAKE_CURRENT_LIST_DIR}/../../app/src)

target_sources(app PRIVATE
  src/test_dsp.cpp
  src/test_voice.cpp

  # Unit under test, compiled straight out of the firmware.
  ${DUO_APP_SRC}/dsp/voice.cpp
)

target_include_directories(app PRIVATE
  ${DUO_APP_SRC}
  ${DUO_APP_SRC}/compat

  # dsp.h reuses the linear envelope shared with the Brains 1 firmware.
  ${CMAKE_CURRENT_LIST_DIR}/../../..
)
