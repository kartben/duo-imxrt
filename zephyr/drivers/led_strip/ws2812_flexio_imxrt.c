/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS2812 / SK6812 LED strip driver using the i.MX RT FlexIO peripheral.
 *
 * This is a Zephyr led_strip driver built from the FlexIO configuration in
 * brains2/core/lib/flexio_led_driver.h, which in turn is a C++ port by Valter
 * Sundstrom of the Rust ws2812-flexio driver by Finomnis
 * (https://github.com/Finomnis/ws2812-flexio).
 *
 * How it works
 * ------------
 * FlexIO produces the WS2812 line code without CPU involvement:
 *
 *   shifter 0     streams pixel bits out on internal FlexIO pin 0
 *   timer 0       clocks the shifter, one WS2812 bit period per shift,
 *                 and mirrors its activity on internal pin 4
 *   timer 2       triggered by timer 0, emits a "0 code" pulse on the
 *                 output pin every bit period
 *   timer 3       triggered by the shifter output, stretches the pulse on
 *                 the output pin into a "1 code" whenever the data bit is 1
 *   timer 1       triggered by timer 0, times the trailing reset gap so the
 *                 driver knows when the strip has latched
 *
 * Both bit timers drive the same output pin, so the pin sees a "0 code" for
 * every bit and a "1 code" wherever the data says so.
 *
 * The FlexIO clock is 16 MHz, giving a 20-cycle (1.25 us) bit period with
 * T0H = 5 cycles (312 ns) and T1H = 15 cycles (937 ns).
 *
 * Each colour byte is expanded to a 32-bit shifter word in which the eight
 * data bits sit in bit 3 of each nibble; the shifter then presents one data
 * bit per bit period. eDMA feeds those words to the shifter buffer, so a
 * strip update costs no CPU time beyond the encoding pass.
 */

#define DT_DRV_COMPAT dato_ws2812_flexio_imxrt

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <soc.h>
#include <fsl_clock.h>

LOG_MODULE_REGISTER(ws2812_flexio, CONFIG_LED_STRIP_LOG_LEVEL);

/* FlexIO resource assignment. Fixed, since this driver owns FlexIO1. */
#define DATA_SHIFTER      0U
#define SHIFT_TIMER       0U
#define IDLE_TIMER        1U
#define LOW_BIT_TIMER     2U
#define HIGH_BIT_TIMER    3U
#define SHIFTER_PIN       0U /* internal FlexIO pin driven by the shifter */
#define SHIFT_TIMER_PIN   4U /* internal FlexIO pin driven by the shift timer */

/*
 * A FlexIO timer in "dual 8-bit counters PWM" mode toggles on every counter
 * expiry, so one full bit period is twice the configured divider.
 */
#define CLOCK_DIVIDER      10U
#define CYCLE_LENGTH       (CLOCK_DIVIDER * 2U)
#define LOW_BIT_CYCLES_ON  5U
#define HIGH_BIT_CYCLES_ON 15U
#define LOW_BIT_CYCLES_OFF (CYCLE_LENGTH - LOW_BIT_CYCLES_ON)
#define HIGH_BIT_CYCLES_OFF (CYCLE_LENGTH - HIGH_BIT_CYCLES_ON)

/* Shifter reload period, in half bit periods. */
#define CYCLES_PER_SHIFTBUFFER 16U

/* Words appended after the pixel data; without these the last LED glitches. */
#define TRAILING_WORDS 3U

struct ws2812_flexio_config {
	FLEXIO_Type *base;
	const struct pinctrl_dev_config *pcfg;
	const struct device *dma_dev;
	uint32_t dma_channel;
	uint8_t flexio_pin;
	uint8_t num_colors;
	const uint8_t *color_mapping;
	uint16_t length;
	uint16_t reset_delay_bits;
	uint8_t clock_pre_div;
	uint8_t clock_div;
};

struct ws2812_flexio_data {
	struct k_mutex lock;
	struct k_sem dma_done;
	bool transfer_active;
	uint32_t *buf;
	size_t buf_words;
};

/*
 * Spread the eight bits of a colour byte into bit 3 of each nibble of a
 * 32-bit word. SHIFTBUFBIS shifts out most significant bit first, so the
 * byte's MSB leaves the pin first, as WS2812 requires.
 */
static inline uint32_t spread4(uint32_t x)
{
	x = (x | (x << 12)) & 0x000F000FU;
	x = (x | (x << 6)) & 0x03030303U;
	x = (x | (x << 3)) & 0x11111111U;

	return x << 3;
}

static void configure_shifter(FLEXIO_Type *base)
{
	base->SHIFTCTL[DATA_SHIFTER] =
		FLEXIO_SHIFTCTL_TIMSEL(SHIFT_TIMER) |
		FLEXIO_SHIFTCTL_TIMPOL(0) |	/* shift on positive edge */
		FLEXIO_SHIFTCTL_PINCFG(3) |	/* pin is an output */
		FLEXIO_SHIFTCTL_PINSEL(SHIFTER_PIN) |
		FLEXIO_SHIFTCTL_PINPOL(0) |	/* active high */
		FLEXIO_SHIFTCTL_SMOD(2);	/* transmit mode */

	base->SHIFTCFG[DATA_SHIFTER] =
		FLEXIO_SHIFTCFG_PWIDTH(3) |
		FLEXIO_SHIFTCFG_INSRC(0) |
		FLEXIO_SHIFTCFG_SSTOP(0) |	/* no stop bit */
		FLEXIO_SHIFTCFG_SSTART(1);	/* load data on the first shift */
}

static void configure_shift_timer(FLEXIO_Type *base)
{
	base->TIMCMP[SHIFT_TIMER] =
		((CYCLES_PER_SHIFTBUFFER - 1U) << 8) | (CLOCK_DIVIDER - 1U);

	base->TIMCTL[SHIFT_TIMER] =
		FLEXIO_TIMCTL_TRGSEL((DATA_SHIFTER * 4U) + 1U) |
		FLEXIO_TIMCTL_TRGSRC(1) |	/* internal trigger */
		FLEXIO_TIMCTL_TRGPOL(1) |	/* trigger active low */
		FLEXIO_TIMCTL_PINCFG(3) |	/* pin is an output */
		FLEXIO_TIMCTL_PINSEL(SHIFT_TIMER_PIN) |
		FLEXIO_TIMCTL_PINPOL(0) |
		FLEXIO_TIMCTL_TIMOD(1);		/* dual 8-bit baud/bit counter */

	base->TIMCFG[SHIFT_TIMER] =
		FLEXIO_TIMCFG_TIMOUT(1) |	/* output zero when enabled */
		FLEXIO_TIMCFG_TIMDEC(0) |	/* count the FlexIO clock */
		FLEXIO_TIMCFG_TIMRST(0) |	/* never reset */
		FLEXIO_TIMCFG_TIMDIS(2) |	/* disable on compare */
		FLEXIO_TIMCFG_TIMENA(2) |	/* enable on trigger high */
		FLEXIO_TIMCFG_TSTOP(0) |
		FLEXIO_TIMCFG_TSTART(0);
}

static void configure_idle_timer(FLEXIO_Type *base, uint16_t reset_delay_bits)
{
	base->TIMCMP[IDLE_TIMER] = CYCLE_LENGTH * reset_delay_bits;

	base->TIMCTL[IDLE_TIMER] =
		FLEXIO_TIMCTL_TRGSEL(SHIFT_TIMER_PIN * 2U) |
		FLEXIO_TIMCTL_TRGPOL(0) |
		FLEXIO_TIMCTL_TRGSRC(1) |
		FLEXIO_TIMCTL_PINSEL(0) |
		FLEXIO_TIMCTL_PINCFG(0) |	/* no pin output */
		FLEXIO_TIMCTL_PINPOL(0) |
		FLEXIO_TIMCTL_TIMOD(3);		/* single 16-bit counter */

	base->TIMCFG[IDLE_TIMER] =
		FLEXIO_TIMCFG_TIMOUT(2) |
		FLEXIO_TIMCFG_TIMDEC(0) |
		FLEXIO_TIMCFG_TIMRST(6) |	/* reset on trigger rising edge */
		FLEXIO_TIMCFG_TIMDIS(2) |	/* disable on compare */
		FLEXIO_TIMCFG_TIMENA(6) |	/* enable on trigger rising edge */
		FLEXIO_TIMCFG_TSTOP(0) |
		FLEXIO_TIMCFG_TSTART(0);
}

/*
 * Emits the WS2812 "0 code" once per bit period: enabled by the shift timer's
 * rising edge, disabled when it reaches its own compare value.
 */
static void configure_low_bit_timer(FLEXIO_Type *base, uint8_t output_pin)
{
	base->TIMCMP[LOW_BIT_TIMER] =
		((LOW_BIT_CYCLES_OFF - 1U) << 8) | (LOW_BIT_CYCLES_ON - 1U);

	base->TIMCTL[LOW_BIT_TIMER] =
		FLEXIO_TIMCTL_TRGSEL(SHIFT_TIMER_PIN * 2U) |
		FLEXIO_TIMCTL_TRGPOL(0) |
		FLEXIO_TIMCTL_TRGSRC(1) |
		FLEXIO_TIMCTL_PINCFG(3) |
		FLEXIO_TIMCTL_PINSEL(output_pin) |
		FLEXIO_TIMCTL_PINPOL(0) |
		FLEXIO_TIMCTL_TIMOD(2);		/* dual 8-bit PWM */

	base->TIMCFG[LOW_BIT_TIMER] =
		FLEXIO_TIMCFG_TIMOUT(0) |
		FLEXIO_TIMCFG_TIMDEC(0) |
		FLEXIO_TIMCFG_TIMRST(0) |
		FLEXIO_TIMCFG_TIMDIS(2) |	/* disable on compare */
		FLEXIO_TIMCFG_TIMENA(6) |	/* enable on trigger rising edge */
		FLEXIO_TIMCFG_TSTOP(0) |
		FLEXIO_TIMCFG_TSTART(0);
}

/*
 * Stretches the pulse into a "1 code" for as long as the shifter output is
 * high, i.e. for every data bit that is set.
 */
static void configure_high_bit_timer(FLEXIO_Type *base, uint8_t output_pin)
{
	base->TIMCMP[HIGH_BIT_TIMER] =
		((HIGH_BIT_CYCLES_OFF - 1U) << 8) | (HIGH_BIT_CYCLES_ON - 1U);

	base->TIMCTL[HIGH_BIT_TIMER] =
		FLEXIO_TIMCTL_TRGSEL(SHIFTER_PIN * 2U) |
		FLEXIO_TIMCTL_TRGPOL(0) |
		FLEXIO_TIMCTL_TRGSRC(1) |
		FLEXIO_TIMCTL_PINCFG(3) |
		FLEXIO_TIMCTL_PINSEL(output_pin) |
		FLEXIO_TIMCTL_PINPOL(0) |
		FLEXIO_TIMCTL_TIMOD(2);

	base->TIMCFG[HIGH_BIT_TIMER] =
		FLEXIO_TIMCFG_TIMOUT(0) |
		FLEXIO_TIMCFG_TIMDEC(0) |
		FLEXIO_TIMCFG_TIMRST(0) |
		FLEXIO_TIMCFG_TIMDIS(6) |	/* disable on trigger falling edge */
		FLEXIO_TIMCFG_TIMENA(6) |	/* enable on trigger rising edge */
		FLEXIO_TIMCFG_TSTOP(0) |
		FLEXIO_TIMCFG_TSTART(0);
}

static inline bool shifter_empty(FLEXIO_Type *base)
{
	return (base->SHIFTSTAT & BIT(DATA_SHIFTER)) != 0U;
}

static inline bool latch_complete(FLEXIO_Type *base)
{
	return (base->TIMSTAT & BIT(IDLE_TIMER)) != 0U;
}

static void dma_callback(const struct device *dma_dev, void *user_data,
			 uint32_t channel, int status)
{
	struct ws2812_flexio_data *data = user_data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);

	if (status < 0) {
		LOG_ERR("LED strip DMA error %d", status);
	}

	k_sem_give(&data->dma_done);
}

/*
 * Block until the strip has finished consuming the previous update and has
 * latched it. Called before overwriting the encode buffer, which means the
 * caller normally finds everything long since idle.
 */
static int wait_for_idle(const struct device *dev)
{
	const struct ws2812_flexio_config *cfg = dev->config;
	struct ws2812_flexio_data *data = dev->data;
	int ret;

	if (!data->transfer_active) {
		return 0;
	}

	ret = k_sem_take(&data->dma_done, K_MSEC(100));
	if (ret < 0) {
		LOG_ERR("timed out waiting for LED strip DMA");
		dma_stop(cfg->dma_dev, cfg->dma_channel);
		data->transfer_active = false;
		return ret;
	}

	data->transfer_active = false;

	/* The shifter still holds the last words handed over by the DMA. */
	while (!shifter_empty(cfg->base)) {
	}

	while (!latch_complete(cfg->base)) {
	}

	return 0;
}

static int ws2812_flexio_update_rgb(const struct device *dev,
				    struct led_rgb *pixels, size_t count)
{
	const struct ws2812_flexio_config *cfg = dev->config;
	struct ws2812_flexio_data *data = dev->data;
	struct dma_block_config block = {0};
	struct dma_config dma_cfg = {0};
	size_t words;
	int ret;

	if (count > cfg->length) {
		count = cfg->length;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = wait_for_idle(dev);
	if (ret < 0) {
		goto out;
	}

	words = 0;
	for (size_t i = 0; i < count; i++) {
		for (uint8_t c = 0; c < cfg->num_colors; c++) {
			uint8_t value;

			switch (cfg->color_mapping[c]) {
			case LED_COLOR_ID_RED:
				value = pixels[i].r;
				break;
			case LED_COLOR_ID_GREEN:
				value = pixels[i].g;
				break;
			case LED_COLOR_ID_BLUE:
				value = pixels[i].b;
				break;
			default:
				value = 0;
				break;
			}

			data->buf[words++] = spread4(value);
		}
	}

	/* Flush the strip's shift register out of the last LED. */
	for (size_t i = 0; i < TRAILING_WORDS; i++) {
		data->buf[words++] = 0;
	}

	sys_cache_data_flush_range(data->buf, words * sizeof(uint32_t));

	/* Arm the latch detector before any new bits go out. */
	cfg->base->TIMSTAT = BIT(IDLE_TIMER);

	block.source_address = (uintptr_t)data->buf;
	block.dest_address = (uintptr_t)&cfg->base->SHIFTBUFBIS[DATA_SHIFTER];
	block.block_size = words * sizeof(uint32_t);
	block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;

	dma_cfg.dma_slot = 0; /* FlexIO1 shifter request */
	dma_cfg.channel_direction = MEMORY_TO_PERIPHERAL;
	dma_cfg.source_data_size = sizeof(uint32_t);
	dma_cfg.dest_data_size = sizeof(uint32_t);
	dma_cfg.source_burst_length = sizeof(uint32_t);
	dma_cfg.dest_burst_length = sizeof(uint32_t);
	dma_cfg.block_count = 1;
	dma_cfg.head_block = &block;
	dma_cfg.user_data = data;
	dma_cfg.dma_callback = dma_callback;
	dma_cfg.complete_callback_en = 0;

	ret = dma_config(cfg->dma_dev, cfg->dma_channel, &dma_cfg);
	if (ret < 0) {
		LOG_ERR("LED strip dma_config failed (%d)", ret);
		goto out;
	}

	/* Let the shifter raise DMA requests as it drains. */
	cfg->base->SHIFTSDEN = BIT(DATA_SHIFTER);

	k_sem_reset(&data->dma_done);

	ret = dma_start(cfg->dma_dev, cfg->dma_channel);
	if (ret < 0) {
		LOG_ERR("LED strip dma_start failed (%d)", ret);
		goto out;
	}

	data->transfer_active = true;

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static size_t ws2812_flexio_length(const struct device *dev)
{
	const struct ws2812_flexio_config *cfg = dev->config;

	return cfg->length;
}

static int ws2812_flexio_init(const struct device *dev)
{
	const struct ws2812_flexio_config *cfg = dev->config;
	struct ws2812_flexio_data *data = dev->data;
	FLEXIO_Type *base = cfg->base;
	int ret;

	if (!device_is_ready(cfg->dma_dev)) {
		LOG_ERR("DMA device not ready");
		return -ENODEV;
	}

	for (uint8_t i = 0; i < cfg->num_colors; i++) {
		switch (cfg->color_mapping[i]) {
		case LED_COLOR_ID_RED:
		case LED_COLOR_ID_GREEN:
		case LED_COLOR_ID_BLUE:
			break;
		default:
			LOG_ERR("unsupported color-mapping entry %u",
				cfg->color_mapping[i]);
			return -EINVAL;
		}
	}

	k_mutex_init(&data->lock);
	k_sem_init(&data->dma_done, 0, 1);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/*
	 * 480 MHz pll3_sw_clk / pre_div / div. The default 5 and 6 give the
	 * 16 MHz the bit timings above are written for.
	 */
	CLOCK_SetMux(kCLOCK_Flexio1Mux, 3);
	CLOCK_SetDiv(kCLOCK_Flexio1PreDiv, cfg->clock_pre_div - 1U);
	CLOCK_SetDiv(kCLOCK_Flexio1Div, cfg->clock_div - 1U);
	CLOCK_EnableClock(kCLOCK_Flexio1);

	base->CTRL = FLEXIO_CTRL_SWRST(1);
	base->CTRL = FLEXIO_CTRL_SWRST(0);

	configure_shifter(base);
	configure_shift_timer(base);
	configure_idle_timer(base, cfg->reset_delay_bits);
	configure_low_bit_timer(base, cfg->flexio_pin);
	configure_high_bit_timer(base, cfg->flexio_pin);

	base->CTRL = FLEXIO_CTRL_FLEXEN(1);

	return 0;
}

static DEVICE_API(led_strip, ws2812_flexio_api) = {
	.update_rgb = ws2812_flexio_update_rgb,
	.length = ws2812_flexio_length,
};

#define WS2812_FLEXIO_NUM_COLORS(n) DT_INST_PROP_LEN(n, color_mapping)

#define WS2812_FLEXIO_DEVICE(n)								\
	PINCTRL_DT_INST_DEFINE(n);							\
											\
	static const uint8_t ws2812_flexio_mapping_##n[] = DT_INST_PROP(n, color_mapping); \
											\
	static uint32_t ws2812_flexio_buf_##n[DT_INST_PROP(n, chain_length) *		\
					      WS2812_FLEXIO_NUM_COLORS(n) + TRAILING_WORDS]; \
											\
	static struct ws2812_flexio_data ws2812_flexio_data_##n = {			\
		.buf = ws2812_flexio_buf_##n,						\
		.buf_words = ARRAY_SIZE(ws2812_flexio_buf_##n),				\
	};										\
											\
	static const struct ws2812_flexio_config ws2812_flexio_config_##n = {		\
		.base = (FLEXIO_Type *)DT_INST_REG_ADDR(n),				\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),				\
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx)),		\
		.dma_channel = DT_INST_DMAS_CELL_BY_NAME(n, tx, mux),			\
		.flexio_pin = DT_INST_PROP(n, flexio_pin),				\
		.num_colors = WS2812_FLEXIO_NUM_COLORS(n),				\
		.color_mapping = ws2812_flexio_mapping_##n,				\
		.length = DT_INST_PROP(n, chain_length),				\
		.reset_delay_bits = DT_INST_PROP(n, reset_delay_bits),			\
		.clock_pre_div = DT_INST_PROP(n, clock_pre_div),			\
		.clock_div = DT_INST_PROP(n, clock_div),				\
	};										\
											\
	DEVICE_DT_INST_DEFINE(n, ws2812_flexio_init, NULL,				\
			      &ws2812_flexio_data_##n, &ws2812_flexio_config_##n,	\
			      POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,		\
			      &ws2812_flexio_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_FLEXIO_DEVICE)
