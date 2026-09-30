/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

/* Starts the I2S stream to the PT8211 DAC and the thread that fills it. */
int audio_out_start(void);

/* Stops the stream, used when powering down. */
void audio_out_stop(void);

/* How long rendering takes, against the time one block of audio lasts. */
struct audio_load {
	uint32_t cpu_mhz;
	uint32_t block_frames;
	/* Mean and worst time to render one block, in CPU cycles. */
	uint32_t render_avg_cycles;
	uint32_t render_max_cycles;
	/* Blocks the figures above cover. */
	uint32_t blocks;
	/* Times the DAC ran dry, since start-up. */
	uint32_t underruns;
};

/* Reads the render statistics and starts a new measurement window. */
void audio_out_read_load(struct audio_load *load);
