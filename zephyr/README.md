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

The file to flash is **`build/zephyr/duo_firmware.bin`**, not `zephyr.bin`.
`zephyr.bin` starts at the beginning of flash, while the updater — like the
legacy build's output — starts at the FlexSPI configuration block 1 KiB in;
written as is, it would leave the DUO without a boot header. Every build turns
`zephyr.bin` into `duo_firmware.bin` and runs the updater's image checks on it
(`tools/updater/duo_image.py`), so an image the boot ROM would reject fails the
build.

Brains 2.1 is the default. For a Brains 2.3 board build with
`-b duo_brains2@2.3.0`: the two differ in the speaker amplifier's shutdown
line, which 2.3 straps to whichever level mutes the amplifier fitted (see
[Board revisions](#board-revisions)).

Useful variants:

| Command | Effect |
| --- | --- |
| `-b duo_brains2@2.3.0` | Brains 2.3 |
| `-- -DCONFIG_DUO_DEV_MODE=y` | Long-pressing play enters the serial downloader instead of powering off |
| `-- -DEXTRA_CONF_FILE=rtt-debug.conf` | Logging over SEGGER RTT (the only UART is taken by MIDI); faults halt instead of rebooting |

CI builds the release firmware for both revisions and the developer-mode
variant on every push, uploads `duo_firmware.bin` for each release build, and
runs the test suites; see `.github/workflows/zephyr-build.yml`.

## Flashing a DUO

This firmware has **not been run on a DUO yet** (see [Status](#status)), so
flash it expecting to need a way back. There is one, and it does not depend on
the new firmware working.

1. Find out which board you have. With the DUO on USB, run
   `uv run firmware_info.py` in `tools/updater`; `board` is `DUO_BRAINS_2.1`
   or `DUO_BRAINS_2.3`. Build for that revision. The revisions differ only in
   how the speaker amplifier is switched, so the wrong build leaves the
   speaker silent (or on when it should be off) until the right one is
   flashed; if the board is not reported, start with 2.1.
2. Check the image without touching the DUO:
   `uv run update_firmware.py --check path/to/duo_firmware.bin`.
3. Flash it: `uv run update_firmware.py path/to/duo_firmware.bin`. The updater
   reboots the DUO into the i.MX RT boot ROM's USB serial downloader over MIDI,
   exactly as for a legacy release.

The updater is careful about the order it writes things in. It checks the file
is a bootable DUO image (FlexSPI configuration block, image vector table, boot
data, vector table, size) and that it is built for the board revision the DUO
reports, before it reboots anything. It then erases, writes and reads back
everything from the image vector table onwards, and only then writes and
verifies the boot header. Until that last step the flash holds no valid boot
header, so if the update is interrupted the boot ROM falls back to the USB
serial downloader on the next power-up. If a step fails to write or verify,
the updater erases the boot header again and restarts the DUO into the serial
downloader (or, if even that fails, leaves NXP's flashloader running). Either
way the update can simply be run again: the updater notices a DUO in the serial
downloader or the flashloader and carries on without asking.

**If the new firmware does not come up** — no sound, no LEDs, or not visible
over USB MIDI so the updater cannot reboot it — switch the DUO off, hold
**both arrow buttons**, and switch it on (or plug it in) while holding them.
After half a second the DUO drops into the serial downloader, and the three
small indicator LEDs should light up; either way the updater will find it. Run
it again, with the firmware you want. This
check runs right after the GPIO drivers start, before USB, audio, the ADC or the
LEDs are touched, so it works even if one of those is what is broken. The
firmware also reboots rather than hangs on a fault, so a crash at start-up does
not stop it being reached.

**To go back to the legacy firmware**, flash a release from
[datomusic/duo-imxrt](https://github.com/datomusic/duo-imxrt/releases) the
same way; the updater takes both kinds of image.

Only if none of that works (for example if the boot ROM itself cannot reach
the flash) does the DUO need opening, to force the boot mode pins into serial
downloader mode.

## Tests

The MIDI and DSP code is pure logic, so it is tested on the host rather than on
a DUO:

```
west twister -T duo-imxrt/zephyr/tests -p native_sim/native/64
```

| Suite | Covers |
| --- | --- |
| `zephyr/tests/midi` | The MIDI 1.0 parser (running status, channel filtering, sysex framing and overflow, soft thru) and the UMP translation, including byte-for-byte round trips |
| `zephyr/tests/scales` | The keyboard scales: the default is the legacy keyboard note for note, every scale stays playable at full transposition, and switching scales carries a pattern across and back |
| `zephyr/tests/tempo` | The internal clock generator: the TEMPO pot mapping, and that ticks neither drift over a minute nor jitter further than one poll interval, survive the microsecond counter's 32-bit wrap, and catch up after a late poll |
| `zephyr/tests/dsp` | Each DSP primitive against the behaviour the voice relies on, plus the assembled voice: silence when idle, sound on a note, decay after release, the delay repeating one tap time later, and no wrapping at extreme gain. Also pins the numerical shortcuts — the fast `sin`/`exp2` against libm, the drum decay against an exact exponential, filter stability across its whole cutoff and resonance range, and that the output carries no DC. The sample player plays exactly what is stored, fades a retriggered hit out rather than cutting it off, and a sampled pad sounds instead of its synthesised drum and feeds the delay as it does |
| `zephyr/tests/duophonic` | The held-note bookkeeping behind duophonic mode: the pair is the lowest and highest note held, a note held from a key and over MIDI counts once, and stray releases after All Notes Off change nothing |
| `zephyr/tests/drumkit` | The sampled drum sounds, synthesised as for the firmware: every key but the first has one, none of them starts or ends on a click, clips or carries DC, and together they stay under a megabyte |

The suites compile the firmware sources directly, so they break when the
firmware does. The delay-line and envelope timings in particular are asserted
against wall-clock milliseconds, which is what makes a change to the tap time or
an envelope rate visible.

The updater has its own tests, which check the image validation against good
and damaged images and drive the write sequence against a simulated
flashloader with NOR-flash semantics (failed writes, corrupted writes, failed
read-back):

```
cd duo-imxrt/tools/updater && uv run python -m unittest discover -s tests
```

## Layout

```
west.yml                        west manifest (T2 workspace application)
zephyr/module.yml               exposes boards/ and dts/ to the build system
zephyr/boards/dato/duo_brains2  board definition for the Brains 2.1 hardware
zephyr/dts/bindings             bindings for the DUO-specific hardware blocks
zephyr/drivers/led_strip        WS2812 driver using FlexIO + eDMA
zephyr/app                      the firmware
zephyr/app/drumkit              drumkit.py, which synthesises the sampled drums
tools/updater                   the updater, and duo_image.py, which checks images
```

Inside `zephyr/app/src`:

| Path | Contents |
| --- | --- |
| `main.cpp` | Control loop, key handling, power management |
| `globals.h`, `duo_synth.h`, `duo_drums.h`, `duo_leds.h`, `tempo.cpp` | Ports of the matching files under `brains2/apps/duo` |
| `tempo_clock.cpp` | The clock generator's arithmetic, kept apart from `tempo.cpp` so it can be tested without the MIDI and sync transports |
| `scales.cpp` | The keyboard scales |
| `drum_kits.h` | The drum pads' sounds, generated from `../drumkit/drumkit.py` at build time |
| `held_notes.h` | The notes held down, for duophonic mode |
| `platform/recovery.cpp` | The power-on recovery keys (both arrows) into the serial downloader |
| `compat/` | Arduino-flavoured helpers and MIDI type definitions that the shared code expects |
| `platform/` | Panel inputs, key matrix, LEDs, MIDI transports, sync jacks, power |
| `platform/ump_convert.cpp` | MIDI 1.0 to Universal MIDI Packet translation, kept apart from the USB transport so it can be tested on the host |
| `dsp/` | The synth voice and the I2S output stream |

## Scales

The legacy keyboard plays one pentatonic scale, the black keys from C♯3. Here
that is the first of eight:

| Step | Scale | Notes from C♯ |
| --- | --- | --- |
| 1 | Pentatonic (the DUO's own, the default) | C♯ D♯ F♯ G♯ A♯ |
| 2 | Major | C♯ D♯ F F♯ G♯ A♯ C |
| 3 | Minor | C♯ D♯ E F♯ G♯ A B |
| 4 | Dorian | C♯ D♯ E F♯ G♯ A♯ B |
| 5 | Mixolydian | C♯ D♯ F F♯ G♯ A♯ B |
| 6 | Harmonic minor | C♯ D♯ E F♯ G♯ A C |
| 7 | Blues | C♯ E F♯ G G♯ B |
| 8 | Chromatic | every semitone |

Every scale starts on the same lowest key, C♯3, so the arrow buttons transpose
them exactly as before (one press down puts all of them in C), and the default
is the legacy keyboard note for note.

- **At power-on**, hold a step button while switching the DUO on, the same way
  a keyboard key picks the MIDI channel. The step lights up during the startup
  animation. Like the MIDI channel, the choice lasts until the DUO is switched
  off; nothing is written to flash.
- **Over MIDI**, CC 3 on the DUO's channel selects a scale while playing:
  values 0–15 pick the first, 16–31 the second, and so on to 112–127 for the
  eighth. CC 3 is undefined in the MIDI specification and the DUO never sends
  it.

When the scale changes, the sequence goes with it: each step keeps its place in
the scale, so a pattern played on the first, third and fifth keys still plays
the first, third and fifth keys' notes. Steps holding notes from outside the
old scale — entered over MIDI — are left alone. The key LEDs keep their legacy
colours: black-key notes have a colour each and the others show grey, so the
keyboard also shows which notes a scale uses.

## Drum sounds

Each drum pad can play ten sounds, one per keyboard key. The first key's is the
DUO's own synthesised drum, which is where both pads start, so the DUO sounds
exactly as before until another is picked:

| Key | Kick pad | Hat pad |
| --- | --- | --- |
| 1 | DUO kick (synthesised, the default) | DUO hi-hat (synthesised, the default) |
| 2 | 808 kick | 808 closed hat |
| 3 | 909 kick | 808 open hat |
| 4 | Acoustic kick | 909 snare |
| 5 | Lo-fi kick | 808 clap |
| 6 | Electro zap | Rimshot |
| 7 | Sub drop | Cowbell |
| 8 | Low tom | Shaker |
| 9 | High tom | Tambourine |
| 10 | Conga | Clave |

- **On the panel**, hold a pad and press a key: the pad takes that key's sound
  and plays it straight away. Holding a pad for a moment also lights its
  current sound's key white and dims the others; a plain hit is too short to
  change the keys. While a pad is held the keys pick sounds rather than play
  notes. Holding both pads sets both. Like the scale, the choice lasts until
  the DUO is switched off.
- **Over MIDI**, CC 14 and CC 15 on the DUO's channel pick the kick and hat
  pads' sounds: values 0–12 the first key's, 13–25 the second's, and so on to
  116–127 for the tenth. Both CCs are undefined in the MIDI specification and
  the DUO never sends them.

The pads play the samples as they play the synthesised drums: the touch
segments set the velocity, which here sets the level; the kick pad still ducks
the pulse oscillator; and with the delay on, the hat pad's sound is sent to it
too. Samples are one-shots: they play to the end whatever the pad does, and a
new hit fades the last one out over 2 ms rather than cutting it off.

The samples are not recordings. `app/drumkit/drumkit.py` synthesises each one
from oscillators, noise, filters and envelopes, modelled on the circuits of the
drum machines they are named after, so they carry no third-party rights. The
build runs it, using only the Python standard library, and links the result
into the firmware: about 700 KB of 16 bit, 44.1 kHz audio that plays straight
out of flash, and the reason the image is 800 KB rather than 100 KB. To listen
to them without flashing:

```
python3 duo-imxrt/zephyr/app/drumkit/drumkit.py --wav drum-sounds
```

To change a sound, edit its function in `drumkit.py`, or replace it with any
other generator that returns samples in -1 to 1.

## Duophonic mode

The DUO has two oscillators, a pulse and a saw, and normally both play the
same note, the saw at the interval the DETUNE pot sets. In duophonic mode, with
the sequencer stopped and two or more notes held, the lowest plays on the pulse
oscillator and the highest on the saw, the way the classic duophonic synths
share out their two oscillators. Hold a bass note and play a melody over it,
or hold an interval.

- **To turn it on**, hold **double speed** while switching the DUO on; the play
  button lights purple during the startup animation. It lasts until the DUO is
  switched off.
- **Over MIDI**, CC 127 (Poly Mode On) on the DUO's channel turns it on and
  CC 126 (Mono Mode On) turns it off. As the MIDI specification has it, both
  also let go of every note.

With a single note held, or with the sequencer running, the DUO plays exactly
as it does without the mode: the sequencer, and the recording of notes into
it, stay monophonic, and so does what the DUO sends over MIDI (the last note
played). The two notes share the filter and the envelopes, so each new note
retriggers both, and GLIDE slides each oscillator to its own note. While the
saw plays a note of its own the DETUNE pot has no effect, and the saw is
brought up to about 3 dB below the pulse so both notes carry. Notes from the
keyboard and over MIDI count alike, so a DAW can play two-note harmonies.

## Board revisions

| Revision | Build with | Speaker amplifier shutdown line |
| --- | --- | --- |
| 2.1 | `-b duo_brains2` | High mutes; released to the amplifier's pull-up while muted, as the legacy 2.1 firmware does |
| 2.3 | `-b duo_brains2@2.3.0` | Strapped to the mute level of whichever amplifier is fitted; read once at start-up with the pad keeper off, then driven, as the legacy 2.3 firmware does |

Everything else is shared. The revision is also reported in USB string
descriptor 7, as `DUO_BRAINS_2.1` or `DUO_BRAINS_2.3`, which is how the updater
tells an image built for the wrong board. The legacy tree also has 2.0 and
2.0-beta1 boards (capacitive drum pads on a BS814A, different indicator LED
pins, and on the beta an MQS rather than PT8211 output); those are not ported.

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
| TinyUSB with hand-written descriptors | Zephyr USB device stack and USB MIDI 2.0 class, at full and high speed, keeping the same VID/PID, `bcdDevice`, string descriptors and serial number |
| Arduino MIDI Library | A small MIDI 1.0 parser (`platform/midi_parser.cpp`), one instance per transport as before, with the library's default soft thru from DIN in to DIN out |
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

The thread times every block it renders, and `tools/updater/audio_load.py`
reads the figures back over MIDI. On a Brains 2.3, measured with that and with
one-off benchmarks of each block, in CPU cycles per stereo frame (a frame lasts
about 11,300 cycles at 500 MHz):

| What | Cycles per frame |
| --- | --- |
| The whole voice, drums idle | 900–1,000 (8–9% of the CPU) |
| A synthesised kick or hi-hat, while it rings | about 500 more each, nearly all of it `sinf()` |
| A second complete synth voice (both oscillators, filter, both envelopes, crusher) | about 470 |
| A sampled drum streaming from flash, every cache line cold | about 60 |

So a sampled drum costs about a tenth of a synthesised one, and there is room
for much more. DTCM, which the delay line nearly fills, runs out well before
the CPU does.

### Known differences

- `blend()` in the legacy FastLED adapter returns its first argument unchanged,
  so the crossfade on the current sequencer step never actually happens. This
  port implements the real crossfade.
- Oscillator band limiting uses PolyBLEP rather than the Teensy library's
  band-limited step tables, and the drum voices are re-derived from
  `AudioSynthSimpleDrum`'s behaviour rather than its exact implementation. The
  patch topology and all parameter mappings are identical, but the two firmwares
  will not be sample-for-sample identical.
- USB MIDI is presented through the USB MIDI 2.0 class. The VID, PID,
  `bcdDevice`, the string descriptors at the same indices (manufacturer,
  product, the serial number in the legacy `CFG0-CFG1` form, then git tag,
  branch, commit and board, which `tools/updater/firmware_info.py` reads) and
  the bus-powered 500 mA configuration match the legacy firmware, so a host
  recognises the same device. The port is named differently, though: a MIDI
  2.0 host takes the names from UMP Stream discovery, which the firmware
  answers with the labels in `app.overlay`, so Linux lists the port as
  `Group 1 (Dato DUO MIDI)` where the legacy firmware's was `Dato DUO MIDI 1`.
  Zephyr's class does not implement the MIDI 1.0 alternate setting that hosts
  without MIDI 2.0 support fall back to — it declares no jacks — so on those
  hosts the DUO has no MIDI port.
- Sysex replies (firmware version, serial number, identity) go out on DIN
  framed once, `F0 … F7`. The legacy firmware passes the framed buffer to the
  Arduino library without saying so, which frames it again (`F0 F0 … F7 F7`).
- The bitcrusher holds each sample for a fractional 17.7 samples, carried
  across audio blocks; the Teensy library's holds a whole 18 and restarts every
  128-sample block. The crushed sound differs slightly.
- In developer mode, holding play turns the play button blue and the DUO enters
  the serial downloader when it is released (the legacy firmware enters it as
  soon as the hold registers).
- Scale selection, the sampled drum sounds, duophonic mode (all above) and the
  power-on recovery keys are new.
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
- The internal clock reads `micros()` rather than `millis()`. Its accumulator
  was always in microseconds, so driving it from a millisecond clock pinned
  every tick to a millisecond boundary — 0.83 ms of jitter at 120 BPM, exported
  on the MIDI clock and the sync jack. `micros()` reads the 64-bit cycle
  counter, so it is exact rather than rounded to the 100 µs kernel tick, and
  the clock is serviced on every pass of the control loop instead of being
  skipped during the panel repaint.

Measured and deliberately left alone: PolyBLEP aliasing on the oscillators is
around −30 dB relative to the fundamental at the top of the DUO's range, but the
voice always plays through its lowpass, and at the filter output that becomes
−63 dB at a 2 kHz cutoff and −85 dB at 541 Hz. Only with the filter wide open at
8.6 kHz on the highest note does it reach −36 dB. Oversampling the oscillators
would be an expensive fix for something the filter already handles.

## Status

The firmware builds clean and the devicetree describes the real Brains 2.1 and
2.3 hardware, but **it has not been run on a DUO**. Everything that touches the
outside world — the FlexIO LED timings, the PT8211 frame format, the ADC
thresholds for the multiplexed switches, USB enumeration — is transcribed from
the working firmware rather than measured, and wants a bring-up pass on real
hardware before it can be called finished.

What has been checked without hardware: the boot header is byte for byte the
one the legacy firmware writes; every build's image passes the same checks the
updater applies; the USB descriptors, the MIDI thru and the scales are covered
by the tests above; and the recovery check is placed ahead of the USB, audio,
ADC, PWM and LED drivers in the init order (asserted at build time).

A sensible order for a first bring-up, so that the way back is proven before
anything else is relied on:

1. Flash, then check the power-on recovery keys reach the serial downloader
   and the updater can reflash from there.
2. Check the DUO shows up over USB MIDI and `firmware_info.py` reads its
   strings, and that the updater's sysex reboot works from the new firmware.
3. Then panel, LEDs, audio, DIN MIDI and sync.
