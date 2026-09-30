/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kept in C for the same reason as mux_pinctrl.c: the pinctrl devicetree
 * macros are written for C.
 */

#include "audio_path_pinctrl.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>

#define AUDIO_PATH_NODE DT_NODELABEL(audio_path)

#if DT_PINCTRL_HAS_NAME(AUDIO_PATH_NODE, default)

PINCTRL_DT_DEFINE(AUDIO_PATH_NODE);

int duo_audio_path_pinctrl_apply(void)
{
	return pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(AUDIO_PATH_NODE),
				   PINCTRL_STATE_DEFAULT);
}

#else

int duo_audio_path_pinctrl_apply(void)
{
	return 0;
}

#endif
