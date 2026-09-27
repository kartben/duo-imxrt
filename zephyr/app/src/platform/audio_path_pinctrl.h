/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Applies the audio path's default pinctrl state, which on boards with a
 * strapped amplifier shutdown line releases it with the pad keeper disabled.
 * Does nothing on boards without one.
 */
int duo_audio_path_pinctrl_apply(void);

#ifdef __cplusplus
}
#endif
