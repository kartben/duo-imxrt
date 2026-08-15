/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio output to the PT8211 DAC.
 *
 * The legacy firmware drives SAI1 by hand through the MCUXpresso SDK
 * (brains2/core/custom_teensy_audio/output_pt8211.cpp), including its own eDMA
 * setup and audio PLL configuration. Here Zephyr's I2S driver owns the
 * peripheral and the clocking comes from devicetree; what is left is choosing
 * the frame format the PT8211 needs and keeping the queue fed.
 *
 * The PT8211 has no MCLK input and latches its data on the word-clock
 * transition rather than one bit clock later, which is left-justified framing
 * with an inverted frame clock - the same TCR4 setup (FSE off, FSP inverted)
 * the legacy driver programs.
 */

#include "audio_out.h"
#include "voice.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_audio_out, CONFIG_DUO_LOG_LEVEL);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_ALIAS(audio_out));

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BLOCK_FRAMES  CONFIG_DUO_AUDIO_BLOCK_FRAMES
#define BLOCK_BYTES   (BLOCK_FRAMES * CHANNELS * sizeof(int16_t))
#define BLOCK_COUNT   CONFIG_DUO_AUDIO_BLOCK_COUNT

/*
 * The DMA reads straight out of these blocks, so they must not sit in
 * write-back cached memory.
 */
#ifdef CONFIG_NOCACHE_MEMORY
#define AUDIO_MEM_ATTR __nocache
#else
#define AUDIO_MEM_ATTR
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_slab, AUDIO_MEM_ATTR, BLOCK_BYTES, BLOCK_COUNT, 4);

static K_KERNEL_STACK_DEFINE(audio_stack, CONFIG_DUO_AUDIO_THREAD_STACK_SIZE);
static struct k_thread audio_thread;

static volatile bool running;

static int queue_block(void)
{
	void *block;
	int ret;

	ret = k_mem_slab_alloc(&audio_slab, &block, K_MSEC(200));
	if (ret < 0) {
		return ret;
	}

	duo::voice.render((int16_t *)block, BLOCK_FRAMES);

	ret = i2s_write(i2s_dev, block, BLOCK_BYTES);
	if (ret < 0) {
		k_mem_slab_free(&audio_slab, block);
	}

	return ret;
}

static void audio_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (running) {
		int ret = queue_block();

		if (ret >= 0) {
			continue;
		}

		LOG_ERR("audio underrun (%d), restarting stream", ret);

		/*
		 * Yield before retrying. This thread is cooperative, so a
		 * failure that returns immediately - a stream that will not
		 * come back, say - would otherwise lock the system out.
		 */
		k_msleep(1);

		if (i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE) < 0) {
			continue;
		}

		/* Refill before restarting so the DAC never runs dry. */
		bool refilled = true;

		for (int i = 0; i < BLOCK_COUNT - 1 && running; i++) {
			if (queue_block() < 0) {
				refilled = false;
				break;
			}
		}

		if (refilled) {
			i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		}
	}
}

int audio_out_start(void)
{
	struct i2s_config cfg = {};
	int ret;

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device not ready");
		return -ENODEV;
	}

	cfg.word_size = 16;
	cfg.channels = CHANNELS;
	cfg.format = I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED | I2S_FMT_CLK_IF_NB;
	cfg.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
	cfg.frame_clk_freq = SAMPLE_RATE;
	cfg.block_size = BLOCK_BYTES;
	cfg.mem_slab = &audio_slab;
	cfg.timeout = 200;

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure failed (%d)", ret);
		return ret;
	}

	running = true;

	/* Prime the queue so the first frames are already there when it starts. */
	for (int i = 0; i < BLOCK_COUNT - 1; i++) {
		ret = queue_block();
		if (ret < 0) {
			LOG_ERR("failed to prime audio queue (%d)", ret);
			running = false;
			return ret;
		}
	}

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("i2s start failed (%d)", ret);
		running = false;
		return ret;
	}

	k_thread_create(&audio_thread, audio_stack, K_KERNEL_STACK_SIZEOF(audio_stack),
			audio_thread_fn, NULL, NULL, NULL,
			CONFIG_DUO_AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&audio_thread, "duo_audio");

	return 0;
}

void audio_out_stop(void)
{
	running = false;
	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
}
