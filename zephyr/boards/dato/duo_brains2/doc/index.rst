.. zephyr:board:: duo_brains2

Overview
********

Brains 2 is the main board of the `Dato DUO`_ synthesizer, built around an NXP
i.MX RT1011 (Cortex-M7 at 500 MHz) booting execute-in-place from a 16 Mbit QSPI
NOR flash. This board definition describes revision 2.1.

The DUO is a self-contained instrument, so the board carries the whole
instrument rather than a set of expansion headers:

- 4x6 button matrix: ten keyboard keys, eight sequencer steps, octave up/down,
  two sequencer mode buttons and a combined play/power button
- 19 SK6812 RGB pixels behind the keys and buttons, on a single data line
- three single-colour indicator LEDs, dimmed with FlexPWM
- eight potentiometers, four slide switches and six capacitive drum pad
  segments, all read through three analog multiplexers
- PT8211 stereo DAC feeding a speaker amplifier and a headphone driver
- MIDI DIN in and out
- USB device port
- analog clock sync in and out jacks

Hardware
********

Supported Features
==================

.. zephyr:board-supported-hw::

The following DUO-specific nodes are described by the board and consumed by the
firmware in ``zephyr/app``:

.. list-table::
   :header-rows: 1

   * - Node
     - Binding
     - Purpose
   * - ``keys``
     - ``gpio-kbd-matrix``
     - Button matrix, scanned and debounced by the input subsystem
   * - ``panel_leds``
     - ``dato,ws2812-flexio-imxrt``
     - RGB pixels, driven by FlexIO and eDMA
   * - ``analog_mux``
     - ``dato,duo-analog-mux``
     - Pots, switches and drum pads behind three 8:1 multiplexers
   * - ``audio_path``
     - ``dato,duo-audio-path``
     - Speaker amplifier and headphone driver enables
   * - ``sync``
     - ``dato,duo-sync``
     - Analog clock sync jacks
   * - ``panel_pwm_leds``
     - ``pwm-leds``
     - Oscillator, filter and envelope indicator LEDs

Pin assignment
==============

.. list-table::
   :header-rows: 1

   * - Pad
     - Signal
   * - GPIO_03 / GPIO_05 / GPIO_08
     - Envelope / filter / oscillator indicator LEDs (FlexPWM1)
   * - GPIO_04 / GPIO_06 / GPIO_07
     - SAI1 TX data, bit clock and word clock to the PT8211 DAC
   * - GPIO_09 / GPIO_10
     - LPUART1 RX and TX, MIDI DIN at 31250 baud
   * - GPIO_11 to GPIO_13, GPIO_SD_04
     - Button matrix rows
   * - GPIO_AD_04 to GPIO_AD_09
     - Button matrix columns
   * - GPIO_AD_00 / GPIO_AD_01
     - Sync input and output jacks
   * - GPIO_AD_02 / GPIO_AD_03 / GPIO_AD_14
     - Multiplexer I/O pads (ADC1 channels 2, 3 and 14)
   * - GPIO_AD_10 / GPIO_AD_11
     - Speaker amplifier shutdown, headphone driver enable
   * - GPIO_SD_00 to GPIO_SD_02
     - Multiplexer address lines
   * - GPIO_SD_05
     - FLEXIO1_IO11, RGB pixel data

Memory layout
=============

The FlexRAM banks are left at the SoC default of 32 KB ITCM, 32 KB DTCM and
64 KB OCRAM. The firmware puts its delay line in DTCM, so applications that
need DTCM for something else should check
``zephyr/app/src/dsp/voice.cpp`` first.

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

The DUO has no debug connector fitted in production. Firmware is normally
updated through the i.MX RT serial downloader over USB, which the updater in
``tools/updater`` speaks; hold the play button on a firmware built with
``CONFIG_DUO_DEV_MODE=y`` to reboot into it.

With a SWD probe attached to the debug pads, the usual runners work:

.. code-block:: console

   west build -b duo_brains2 zephyr/app
   west flash

Since LPUART1 is wired to the MIDI jacks there is no serial console. Build with
``-DEXTRA_CONF_FILE=rtt-debug.conf`` to get logging over SEGGER RTT instead.

References
**********

.. target-notes::

.. _Dato DUO:
   https://dato.mu/products/duo
