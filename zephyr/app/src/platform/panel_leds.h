/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * The three single-colour indicator LEDs behind the panel graphics. The legacy
 * firmware writes them with Arduino analogWrite() through the macros in
 * brains2/core/boards/DUO_BRAINS_2.1/led_pins.h, which are inverted (255 is
 * off). The same curves are kept here, expressed as brightness in percent.
 */

#pragma once

#include <cstdint>

int panel_leds_init(void);

void write_env_led(float peak);
void write_filter_led(int filter);
void write_osc_led(int pulse_width);

void blank_env_led(void);
void blank_filter_led(void);
void blank_osc_led(void);
