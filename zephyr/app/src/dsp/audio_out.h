/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Starts the I2S stream to the PT8211 DAC and the thread that fills it. */
int audio_out_start(void);

/* Stops the stream, used when powering down. */
void audio_out_stop(void);
