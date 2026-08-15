/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mux_pinctrl.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>

#define MUX_NODE DT_NODELABEL(analog_mux)

/* Identifier for the node's custom "syn-gpio" pinctrl state. */
#define PINCTRL_STATE_SYN_GPIO PINCTRL_STATE_PRIV_START

PINCTRL_DT_DEFINE(MUX_NODE);

int duo_mux_pinctrl_apply(enum duo_mux_pinctrl_state state)
{
	const uint8_t id = (state == DUO_MUX_PINCTRL_SYN_GPIO) ? PINCTRL_STATE_SYN_GPIO
							       : PINCTRL_STATE_DEFAULT;

	return pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(MUX_NODE), id);
}
