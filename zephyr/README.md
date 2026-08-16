# Dato DUO Brains 2 on Zephyr

A port of the Brains 2 firmware from bare-metal MCUXpresso SDK + Arduino
compatibility layer to [Zephyr](https://zephyrproject.org). The original
firmware in `brains2/` is untouched and still builds; this is a parallel
implementation of the same instrument.

## Building

The repository doubles as a Zephyr [T2 workspace
application](https://docs.zephyrproject.org/latest/develop/west/workspaces.html):
`west.yml` at the repository root is the manifest, and `zephyr/module.yml`
exposes the board definition and devicetree bindings below to the build system.

```
pip install west
west init -m https://github.com/datomusic/duo-imxrt --mr main duo-ws
cd duo-ws
west update
west sdk install -t arm-zephyr-eabi     # once, if you have no Zephyr SDK yet

west build -b duo_brains2 duo-imxrt/zephyr/app
```

The result is `build/zephyr/zephyr.bin`, which the updater in `tools/updater`
can load the same way as a binary from the legacy build.

Useful variants:

| Command | Effect |
| --- | --- |
| `-- -DCONFIG_DUO_DEV_MODE=y` | Long-pressing play enters the serial downloader instead of powering off |
| `-- -DEXTRA_CONF_FILE=rtt-debug.conf` | Logging over SEGGER RTT (the only UART is taken by MIDI) |

CI builds both the release and developer-mode variants on every push and runs
the test suites; see `.github/workflows/zephyr-build.yml`.

## Tests

The MIDI and DSP code is pure logic, so it is tested on the host rather than on
a DUO:

```
west twister -T duo-imxrt/zephyr/tests -p native_sim/native/64
```

| Suite | Covers |
| --- | --- |
| `zephyr/tests/midi` | The MIDI 1.0 parser (running status, channel filtering, sysex framing and overflow) and the UMP translation, including byte-for-byte round trips |
| `zephyr/tests/dsp` | Each DSP primitive against the behaviour the voice relies on, plus the assembled voice: silence when idle, sound on a note, decay after release, the delay repeating one tap time later, and no wrapping at extreme gain. Also pins the numerical shortcuts — the fast `sin`/`exp2` against libm, the drum decay against an exact exponential, filter stability across its whole cutoff and resonance range, and that the output carries no DC |

Both suites compile the firmware sources directly, so they break when the
firmware does. The delay-line and envelope timings in particular are asserted
against wall-clock milliseconds, which is what makes a change to the tap time or
an envelope rate visible.

## Layout

```
west.yml                        west manifest (T2 workspace application)
zephyr/module.yml               exposes boards/ and dts/ to the build system
zephyr/boards/dato/duo_brains2  board definition for the Brains 2.1 hardware
zephyr/dts/bindings             bindings for the DUO-specific hardware blocks
zephyr/drivers/led_strip        WS2812 driver using FlexIO + eDMA
zephyr/app                      the firmware
```

Inside `zephyr/app/src`:

| Path | Contents |
| --- | --- |
| `main.cpp` | Control loop, key handling, power management |
| `globals.h`, `duo_synth.h`, `duo_drums.h`, `duo_leds.h`, `tempo.cpp` | Ports of the matching files under `brains2/apps/duo` |
| `compat/` | Arduino-flavoured helpers and MIDI type definitions that the shared code expects |
| `platform/` | Panel inputs, key matrix, LEDs, MIDI transports, sync jacks, power |
| `platform/ump_convert.cpp` | MIDI 1.0 to Universal MIDI Packet translation, kept apart from the USB transport so it can be tested on the host |
| `dsp/` | The synth voice and the I2S output stream |

## What carries over unchanged

The sequencer, tempo, pitch and MIDI logic in `shared/duo/` is shared with the
Brains 1 firmware, and this port compiles it as is — `seq.cpp`, `seq.h`,
`note_stack.h`, `Sequencer.h`, `TempoHandler.h`, `MidiFunctions.h`, `Pitch.h`,
`synth_params.h` and `envelope.h`. Everything those files need from Arduino
(`millis()`, `map()`, `random()`, `elapsedMillis`, the `midi::` enumerations) is
provided by `src/compat/`, so the musical behaviour of the instrument comes from
exactly the same code as before.

## What was rewritten, and why

| Legacy | Zephyr port |
| --- | --- |
| MCUXpresso SDK peripheral setup in `core/lib/board.c`, `pin_mux.c`, `clock_config.c` | Devicetree, plus Zephyr's GPIO, ADC, PWM, UART, I2S and DMA drivers |
| Arduino `Keypad` library polling GPIOs | `gpio-kbd-matrix` input driver; only the press/hold/release state machine and the key map remain in the app |
| Hand-rolled FlexIO WS2812 driver in `core/lib/flexio_led_driver.h` | Same FlexIO configuration, repackaged as a Zephyr `led_strip` driver (`zephyr/drivers/led_strip/ws2812_flexio_imxrt.c`) |
| TinyUSB with hand-written descriptors | Zephyr USB device stack and USB MIDI 2.0 class, keeping the same VID/PID and descriptor strings |
| Arduino MIDI Library | A small MIDI 1.0 parser (`platform/midi_parser.cpp`), one instance per transport as before |
| Teensy Audio Library patch in `duo-firmware/src/Synth.h` | Standalone DSP in `dsp/`, block for block |
| Direct SAI + eDMA programming in `output_pt8211.cpp` | Zephyr I2S, with the PT8211's framing expressed as left-justified with an inverted frame clock |

### The audio engine

The Teensy Audio Library is tied to the Teensy core's block and DMA machinery,
so it could not come along. `dsp/dsp.h` provides equivalents for each object the
DUO patch uses — band-limited oscillators, a state variable filter, the linear
envelope from `shared/duo/envelope.h`, a sample-rate reducer, a delay line, a
simple drum voice and white noise — and `dsp/voice.h` wires them into the same
graph, with the same gains, envelope times and filter settings the legacy
firmware programs.

Audio runs in a cooperative thread rather than from the codec DMA interrupt.
Four blocks of 128 stereo frames give about 12 ms of buffering, enough to absorb
an LED refresh or a burst of MIDI. The 350 ms delay line is 31 KB and lives in
DTCM.

### Known differences

- `blend()` in the legacy FastLED adapter returns its first argument unchanged,
  so the crossfade on the current sequencer step never actually happens. This
  port implements the real crossfade.
- Oscillator band limiting uses PolyBLEP rather than the Teensy library's
  band-limited step tables, and the drum voices are re-derived from
  `AudioSynthSimpleDrum`'s behaviour rather than its exact implementation. The
  patch topology and all parameter mappings are identical, but the two firmwares
  will not be sample-for-sample identical.
- USB MIDI is presented through the USB MIDI 2.0 class, which enumerates as a
  MIDI 1.0 endpoint on hosts that do not speak MIDI 2.0. The VID, PID and
  descriptor strings match the legacy firmware.
- The pulse oscillator subtracts its own DC offset. A pulse of duty *d* has a
  mean of `2d - 1`, and since the DUO's pulse width runs to 0.95 and the filter
  passes DC at unity gain, the legacy firmware gates up to 0.14 of full scale of
  constant offset through the amp envelope — wasted headroom, and a step at
  every note on and note off. Removing it leaves the audible content unchanged
  (AC RMS is identical to five decimal places) but the DUO will be marginally
  louder before clipping and will not thump at wide pulse widths.
- The filter's cutoff modulation, the drum decay envelope and the output sample
  conversion use polynomial approximations and incremental updates in place of
  `exp2f()`, `expf()` and a truncating cast. All three are pinned against the
  exact functions in the DSP tests: the filter cutoff is within 0.15 cents, the
  drum decay within a quarter of an LSB at 16 bit, and the sample conversion now
  rounds to nearest instead of towards zero.
- The hi-hat noise generator is seeded from the entropy source at init. The
  legacy firmware starts from a fixed constant, so it replays an identical
  sequence of hi-hats on every power cycle.
- Envelope stages now last as long as they say they do. `LinearEnvelope`
  computes a truncated integer rate, `int(full_scale / samples)`, and against
  the legacy full scale of 2^16 that collapses at long times: a 500 ms release
  came out at 743 ms, and the whole top third of the release pot's travel shared
  three distinct values, so the knob barely did anything up there. The port
  raises its own full scale to 2^24 — 512 distinct release times instead of 46,
  and within 0.13% of nominal across the pot. Long releases are therefore
  *shorter* than on the legacy firmware. This is the port's own constant; the
  legacy `AudioEffectCustomEnvelope` declares its own and is untouched.

Measured and deliberately left alone: PolyBLEP aliasing on the oscillators is
around −30 dB relative to the fundamental at the top of the DUO's range, but the
voice always plays through its lowpass, and at the filter output that becomes
−63 dB at a 2 kHz cutoff and −85 dB at 541 Hz. Only with the filter wide open at
8.6 kHz on the highest note does it reach −36 dB. Oversampling the oscillators
would be an expensive fix for something the filter already handles.

## Status

The firmware builds clean and the devicetree describes the real Brains 2.1
hardware, but **it has not been run on a DUO**. Everything that touches the
outside world — the FlexIO LED timings, the PT8211 frame format, the ADC
thresholds for the multiplexed switches, USB enumeration — is transcribed from
the working firmware rather than measured, and wants a bring-up pass on real
hardware before it can be called finished.
