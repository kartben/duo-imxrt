# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(duo_drumkit_tests LANGUAGES C CXX)

set(DUO_APP ${CMAKE_CURRENT_LIST_DIR}/../../app)

target_sources(app PRIVATE src/test_drumkit.cpp)

# The samples under test, synthesised exactly as the firmware build does.
include(${DUO_APP}/drumkit/drumkit.cmake)
duo_drumkit_sources(app)

target_include_directories(app PRIVATE ${DUO_APP}/src)
