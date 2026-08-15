/*
 * Copyright (c) 2025 Dato Musical Instruments
 * SPDX-License-Identifier: Apache-2.0
 *
 * Analog front end of the DUO panel, ported from
 * brains2/core/boards/DUO_BRAINS_2.1/pins.cpp.
 *
 * Three 8:1 multiplexers share one set of address lines. Selecting a channel
 * means driving the three address GPIOs and waiting for the mux output and the
 * RC filtering on the pot wipers to settle, then sampling the corresponding
 * ADC input.
 *
 * Channels that carry a switch rather than a pot are sampled with the same ADC
 * and thresholded, which is electrically what a high-impedance digital input
 * would do. The two channels that need a pull-up (GLIDE and DELAY) are the
 * exception: for those the "syn" pad is re-muxed to GPIO, read, and muxed back.
 */

#include "pins.h"

#include "mux_pinctrl.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(duo_pins, CONFIG_DUO_LOG_LEVEL);

#define MUX_NODE DT_NODELABEL(analog_mux)

BUILD_ASSERT(DT_NODE_HAS_STATUS(MUX_NODE, okay), "analog_mux node is missing");

/* Multiplexer identifiers, matching the io-channel-names in devicetree. */
enum MuxId {
	MUX_SYN,
	MUX_SYN2,
	MUX_BRN,
	MUX_COUNT,
};

static const struct adc_dt_spec mux_adc[MUX_COUNT] = {
	ADC_DT_SPEC_GET_BY_NAME(MUX_NODE, syn),
	ADC_DT_SPEC_GET_BY_NAME(MUX_NODE, syn2),
	ADC_DT_SPEC_GET_BY_NAME(MUX_NODE, brn),
};

static const struct gpio_dt_spec addr_gpio[] = {
	GPIO_DT_SPEC_GET_BY_IDX(MUX_NODE, addr_gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(MUX_NODE, addr_gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(MUX_NODE, addr_gpios, 2),
};

static const struct gpio_dt_spec syn_gpio = GPIO_DT_SPEC_GET(MUX_NODE, syn_gpios);

static const uint32_t settle_time_us = DT_PROP(MUX_NODE, settle_time_us);

/*
 * The legacy firmware works in 10-bit units everywhere (Arduino analogRead),
 * and so do the pot ranges baked into the shared synth code, so the 12-bit
 * conversions are scaled down rather than the ranges scaled up.
 */
static const uint32_t ADC_FULL_SCALE = 1023;

/* Anything above ~70% of full scale counts as a logic high. */
static const uint32_t DIGITAL_THRESHOLD = 700;

static uint16_t sample_buffer;

static struct adc_sequence sequence = {
	.buffer = &sample_buffer,
	.buffer_size = sizeof(sample_buffer),
};

static struct k_mutex mux_lock;

static void select_channel(uint8_t channel)
{
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_dt(&addr_gpio[i], (channel >> i) & 0x1);
	}

	k_busy_wait(settle_time_us);
}

static uint32_t mux_analog_read(uint8_t channel, MuxId mux)
{
	uint32_t value = 0;

	k_mutex_lock(&mux_lock, K_FOREVER);

	select_channel(channel);

	int ret = adc_sequence_init_dt(&mux_adc[mux], &sequence);

	if (ret == 0) {
		ret = adc_read_dt(&mux_adc[mux], &sequence);
	}

	if (ret == 0) {
		const uint32_t full_scale = BIT(mux_adc[mux].resolution) - 1;

		value = ((uint32_t)sample_buffer * ADC_FULL_SCALE) / full_scale;
	} else {
		LOG_WRN("ADC read failed on mux %d channel %u (%d)", mux, channel, ret);
	}

	k_mutex_unlock(&mux_lock);

	return value;
}

/*
 * Reads a switch that pulls the multiplexer input to ground, returning the raw
 * logic level on the pad. The pad is temporarily taken away from the ADC so
 * that its pull-up can hold the line high while the switch is open.
 */
static int mux_digital_read_pullup(uint8_t channel)
{
	int value;

	k_mutex_lock(&mux_lock, K_FOREVER);

	duo_mux_pinctrl_apply(DUO_MUX_PINCTRL_SYN_GPIO);
	/*
	 * The pull-up has to be requested here as well as in the pinctrl
	 * state: configuring an i.MX RT pad as a plain input puts it in keeper
	 * mode, which would clear it again.
	 */
	gpio_pin_configure_dt(&syn_gpio, GPIO_INPUT | GPIO_PULL_UP);

	select_channel(channel);
	value = gpio_pin_get_dt(&syn_gpio);

	duo_mux_pinctrl_apply(DUO_MUX_PINCTRL_ADC);

	k_mutex_unlock(&mux_lock);

	return value;
}

static bool mux_digital_read(uint8_t channel, MuxId mux)
{
	return mux_analog_read(channel, mux) > DIGITAL_THRESHOLD;
}

uint32_t potRead(const Pot pot)
{
	switch (pot) {
	case FILTER_RES_POT:
		return mux_analog_read(0, MUX_SYN);
	case TEMPO_POT:
		return ADC_FULL_SCALE - mux_analog_read(4, MUX_BRN);
	case GATE_POT:
		return ADC_FULL_SCALE - mux_analog_read(6, MUX_BRN);
	case AMP_POT:
		return mux_analog_read(3, MUX_SYN);
	case FILTER_FREQ_POT:
		return mux_analog_read(5, MUX_SYN);
	case OSC_PW_POT:
		return mux_analog_read(7, MUX_SYN);
	case OSC_DETUNE_POT:
		return mux_analog_read(6, MUX_SYN);
	case AMP_ENV_POT:
		return mux_analog_read(1, MUX_SYN);
	default:
		return 500;
	}
}

bool pinRead(const Pin pin)
{
	switch (pin) {
	case ACCENT_PIN:
		return !mux_digital_read(0, MUX_SYN2);
	case GLIDE_PIN:
		return mux_digital_read_pullup(4) == 0;
	case DELAY_PIN:
		return mux_digital_read_pullup(2) != 0;
	case BITC_PIN:
		return !mux_digital_read(1, MUX_SYN2);
	case HP_DETECT_PIN:
		return mux_digital_read(3, MUX_BRN);
	case SYNC_DETECT_PIN:
		/*
		 * The pull-down on this net is too weak for a clean logic
		 * level, so it is measured as an analog value instead.
		 */
		return mux_analog_read(2, MUX_BRN) >= DIGITAL_THRESHOLD;
	case HAT_PAD_L_PIN:
		return !mux_digital_read(3, MUX_SYN2);
	case HAT_PAD_M_PIN:
		return !mux_digital_read(2, MUX_SYN2);
	case HAT_PAD_R_PIN:
		return !mux_digital_read(4, MUX_SYN2);
	case KICK_PAD_L_PIN:
		return !mux_digital_read(5, MUX_SYN2);
	case KICK_PAD_M_PIN:
		return !mux_digital_read(7, MUX_SYN2);
	case KICK_PAD_R_PIN:
		return !mux_digital_read(6, MUX_SYN2);
	default:
		return false;
	}
}

bool headphone_jack_detected()
{
	return pinRead(HP_DETECT_PIN);
}

int pins_init()
{
	int ret;

	k_mutex_init(&mux_lock);

	for (int i = 0; i < 3; i++) {
		if (!gpio_is_ready_dt(&addr_gpio[i])) {
			LOG_ERR("mux address GPIO %d not ready", i);
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&addr_gpio[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}

	for (int i = 0; i < MUX_COUNT; i++) {
		if (!adc_is_ready_dt(&mux_adc[i])) {
			LOG_ERR("ADC for mux %d not ready", i);
			return -ENODEV;
		}

		ret = adc_channel_setup_dt(&mux_adc[i]);
		if (ret < 0) {
			LOG_ERR("ADC channel setup for mux %d failed (%d)", i, ret);
			return ret;
		}
	}

	return duo_mux_pinctrl_apply(DUO_MUX_PINCTRL_ADC);
}
