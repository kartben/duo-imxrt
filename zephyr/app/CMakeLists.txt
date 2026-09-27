# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(duo_firmware LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/tempo.cpp
  src/tempo_clock.cpp

  # Sequencer core, shared with the Brains 1 firmware and compiled as is.
  ${CMAKE_CURRENT_LIST_DIR}/../../shared/duo/seq.cpp

  src/dsp/audio_out.cpp
  src/dsp/voice.cpp

  src/platform/FastLED.cpp
  src/platform/audio_path.cpp
  src/platform/audio_path_pinctrl.c
  src/platform/keys.cpp
  src/platform/midi.cpp
  src/platform/mux_pinctrl.c
  src/platform/midi_parser.cpp
  src/platform/panel_leds.cpp
  src/platform/pins.cpp
  src/platform/power.cpp
  src/platform/recovery.cpp
  src/platform/sync.cpp
  src/platform/ump_convert.cpp
  src/platform/usb_midi.cpp

  # WS2812 driver for the panel. Zephyr has no FlexIO abstraction, so this
  # lives with the firmware rather than in the Zephyr tree.
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/led_strip/ws2812_flexio_imxrt.c
)

target_include_directories(app PRIVATE
  src
  src/compat
  ${CMAKE_CURRENT_BINARY_DIR}/include

  # The sequencer, tempo, pitch and MIDI logic shared with the Brains 1
  # firmware is compiled straight out of the repository root.
  ${CMAKE_CURRENT_LIST_DIR}/../..
)

# Describe the build the way the legacy firmware does (brains2/apps/duo/
# CMakeLists.txt), for the USB string descriptors the updater reads back.
find_package(Git QUIET)

function(duo_git out)
  set(value "")
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} ${ARGN}
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
      OUTPUT_VARIABLE value
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()
  set(${out} "${value}" PARENT_SCOPE)
endfunction()

# USB strings are printable ASCII, and the legacy firmware caps them at 31
# characters.
function(duo_usb_string var)
  string(REGEX REPLACE "[^ -~]" "?" value "${${var}}")
  string(REPLACE "\"" "'" value "${value}")
  string(REPLACE "\\" "/" value "${value}")
  string(SUBSTRING "${value}" 0 31 value)
  set(${var} "${value}" PARENT_SCOPE)
endfunction()

duo_git(DUO_GIT_TAG name-rev --name-only --tags HEAD)
if(DUO_GIT_TAG STREQUAL "" OR DUO_GIT_TAG STREQUAL "undefined")
  set(DUO_GIT_TAG "Dev build")
endif()

duo_git(DUO_GIT_HASH log -1 --format=%h)
duo_git(DUO_GIT_BRANCH rev-parse --abbrev-ref HEAD)
duo_git(git_diff diff --stat)
if(NOT git_diff STREQUAL "")
  set(DUO_GIT_BRANCH "[edited] ${DUO_GIT_BRANCH}")
endif()

# The revision naming follows the legacy BOARD names, DUO_BRAINS_<major>.<minor>.
if(BOARD_REVISION MATCHES "^([0-9]+)\\.([0-9]+)")
  set(DUO_BOARD_NAME "DUO_BRAINS_${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
else()
  message(FATAL_ERROR "Cannot derive the DUO board name from revision '${BOARD_REVISION}'")
endif()

foreach(var DUO_GIT_TAG DUO_GIT_HASH DUO_GIT_BRANCH DUO_BOARD_NAME)
  duo_usb_string(${var})
endforeach()

configure_file(src/platform/build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/build_info.h @ONLY)
