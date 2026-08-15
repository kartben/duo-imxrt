/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pin muxing for the analog multiplexer I/O pads. Kept in a C translation unit
 * because the i.MX RT pinctrl macros build their pin descriptors with braced
 * initialisers that C++ rejects as narrowing conversions.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum duo_mux_pinctrl_state {
	/* All three multiplexer pads routed to the ADC. */
	DUO_MUX_PINCTRL_ADC = 0,
	/* The "syn" pad routed to GPIO with its pull-up enabled. */
	DUO_MUX_PINCTRL_SYN_GPIO = 1,
};

int duo_mux_pinctrl_apply(enum duo_mux_pinctrl_state state);

#ifdef __cplusplus
}
#endif
