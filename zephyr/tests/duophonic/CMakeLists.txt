# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(duo_duophonic_tests LANGUAGES C CXX)

target_sources(app PRIVATE src/test_held_notes.cpp)

# Unit under test, header-only, straight out of the firmware.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../app/src)
