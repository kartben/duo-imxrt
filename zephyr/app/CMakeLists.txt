# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(duo_firmware LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/tempo.cpp

  # Sequencer core, shared with the Brains 1 firmware and compiled as is.
  ${CMAKE_CURRENT_LIST_DIR}/../../shared/duo/seq.cpp

  src/dsp/audio_out.cpp
  src/dsp/voice.cpp

  src/platform/FastLED.cpp
  src/platform/audio_path.cpp
  src/platform/keys.cpp
  src/platform/midi.cpp
  src/platform/mux_pinctrl.c
  src/platform/midi_parser.cpp
  src/platform/panel_leds.cpp
  src/platform/pins.cpp
  src/platform/power.cpp
  src/platform/sync.cpp
  src/platform/usb_midi.cpp

  # WS2812 driver for the panel. Zephyr has no FlexIO abstraction, so this
  # lives with the firmware rather than in the Zephyr tree.
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/led_strip/ws2812_flexio_imxrt.c
)

target_include_directories(app PRIVATE
  src
  src/compat

  # The sequencer, tempo, pitch and MIDI logic shared with the Brains 1
  # firmware is compiled straight out of the repository root.
  ${CMAKE_CURRENT_LIST_DIR}/../..
)
