#!/usr/bin/env python3
# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0
"""Synthesises the DUO's sampled drum sounds.

Every sound is modelled here from scratch - sine and square oscillators,
noise, filters and envelopes, after the circuits of the classic drum
machines - so the samples carry no third-party rights and are covered by the
firmware's own licence. The build runs this script and links the result into
the firmware, where the sounds play straight out of flash.

Each pad gets one sound per keyboard key. The first key keeps the DUO's own
synthesised drum, which the firmware renders itself, so it has no sample here.

    drumkit.py --out drum_samples.cpp     C++ source for the firmware
    drumkit.py --wav DIR                  one WAV per sound, to listen to
    drumkit.py --list                     the sounds, in key order

Only the Python standard library is used, so the build needs nothing extra,
and the output is the same on every run.
"""

import argparse
import math
import os
import random
import struct
import wave

SR = 44100
TWO_PI = 2.0 * math.pi


def samples(seconds):
    return int(round(seconds * SR))


def times(seconds):
    return [i / SR for i in range(samples(seconds))]


# --- building blocks ---------------------------------------------------------

def decay(t, tau, attack=0.0005):
    """Exponential decay with a short linear attack, so nothing starts on a step."""
    rise = min(1.0, t / attack) if attack > 0 else 1.0
    return rise * math.exp(-t / tau)


def sweep_sine(length, f_start, f_end, tau, phase=0.0):
    """A sine whose frequency falls exponentially from f_start towards f_end."""
    out = []
    for t in times(length):
        f = f_end + (f_start - f_end) * math.exp(-t / tau)
        phase += f / SR
        out.append(math.sin(TWO_PI * phase))
    return out


def square(length, f, phase=0.0):
    out = []
    for _ in range(samples(length)):
        phase = (phase + f / SR) % 1.0
        out.append(1.0 if phase < 0.5 else -1.0)
    return out


def noise(length, seed):
    rng = random.Random(seed)
    return [2.0 * rng.random() - 1.0 for _ in range(samples(length))]


class Biquad:
    """Filters from the RBJ audio EQ cookbook; band pass has 0 dB peak gain."""

    def __init__(self, kind, f0, q=0.707):
        w0 = TWO_PI * f0 / SR
        alpha = math.sin(w0) / (2.0 * q)
        cw = math.cos(w0)
        if kind == "lp":
            b = ((1 - cw) / 2, 1 - cw, (1 - cw) / 2)
        elif kind == "hp":
            b = ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2)
        elif kind == "bp":
            b = (alpha, 0.0, -alpha)
        else:
            raise ValueError(kind)
        a0 = 1 + alpha
        self.b = [x / a0 for x in b]
        self.a = [-2 * cw / a0, (1 - alpha) / a0]

    def __call__(self, xs):
        b0, b1, b2 = self.b
        a1, a2 = self.a
        x1 = x2 = y1 = y2 = 0.0
        out = []
        for x in xs:
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out.append(y)
        return out


def lp(xs, f, q=0.707):
    return Biquad("lp", f, q)(xs)


def hp(xs, f, q=0.707):
    return Biquad("hp", f, q)(xs)


def bp(xs, f, q=0.707):
    return Biquad("bp", f, q)(xs)


def mul(xs, ys):
    return [x * y for x, y in zip(xs, ys)]


def mix(*parts):
    """Sums (gain, signal) pairs, padding the shorter signals with silence."""
    n = max(len(s) for _, s in parts)
    out = [0.0] * n
    for gain, s in parts:
        for i, x in enumerate(s):
            out[i] += gain * x
    return out


def env(length, tau, attack=0.0005, delay=0.0):
    return [decay(t - delay, tau, attack) if t >= delay else 0.0 for t in times(length)]


def drive(xs, amount):
    """Soft saturation, as an overdriven analogue output stage."""
    norm = math.tanh(amount)
    return [math.tanh(amount * x) / norm for x in xs]


# --- the sounds --------------------------------------------------------------
#
# Lengths are the longest a sound may run; each is trimmed where it has faded
# out and ends on a short fade, so no sound stops on a step.

def kick_808():
    length = 0.9
    body = mul(sweep_sine(length, 130, 49, 0.035), env(length, 0.2, attack=0.001))
    click = mul(sweep_sine(length, 1200, 1200, 1.0), env(length, 0.0015))
    return drive(mix((1.0, body), (0.15, click)), 1.2)


def kick_909():
    length = 0.5
    body = mul(sweep_sine(length, 280, 52, 0.012), env(length, 0.16))
    click = mul(hp(noise(length, 909), 3000), env(length, 0.001))
    return drive(mix((1.0, body), (0.35, click)), 2.0)


def kick_acoustic():
    length = 0.4
    body = mul(sweep_sine(length, 85, 55, 0.02), env(length, 0.09, attack=0.001))
    shell = mul(sweep_sine(length, 115, 110, 0.05), env(length, 0.05))
    beater = mul(lp(noise(length, 7), 3000), env(length, 0.002))
    return mix((1.0, body), (0.2, shell), (0.5, beater))


def kick_lofi():
    length = 0.4
    body = mul(sweep_sine(length, 200, 60, 0.015), env(length, 0.13))
    body = drive(body, 2.5)
    # Hold every fourth sample and keep 6 bits: an early sampler's grit. The
    # low pass stands in for its output filter, which kept the images of the
    # 11 kHz rate from whining.
    out = []
    for i, x in enumerate(body):
        if i % 4 == 0:
            held = round(x * 31) / 31
        out.append(held)
    return lp(lp(out, 5000), 5000)


def zap():
    length = 0.35
    return mul(sweep_sine(length, 1800, 45, 0.02), env(length, 0.12))


def sub_drop():
    length = 1.2
    return mul(sweep_sine(length, 95, 28, 0.35), env(length, 0.3, attack=0.005))


def tom(f_start, f_end, pitch_tau, tau, length, seed):
    body = mul(sweep_sine(length, f_start, f_end, pitch_tau), env(length, tau, attack=0.001))
    skin = mul(bp(noise(length, seed), f_end * 4, 1.0), env(length, 0.03))
    return mix((1.0, body), (0.12, skin))


def tom_low():
    return tom(150, 95, 0.08, 0.18, 0.6, 41)


def tom_high():
    return tom(230, 150, 0.06, 0.14, 0.45, 43)


def conga():
    length = 0.3
    f = sweep_sine(length, 360, 320, 0.01)
    overtone = sweep_sine(length, 530, 470, 0.01)
    slap = mul(bp(noise(length, 11), 2500, 1.5), env(length, 0.004))
    return mix((1.0, mul(f, env(length, 0.07))), (0.25, mul(overtone, env(length, 0.04))),
               (0.3, slap))


# The six square oscillators of the TR-808's cymbal and hi-hat section.
METAL_HZ = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)


def metal(length):
    oscillators = [square(length, f, phase=i / 7) for i, f in enumerate(METAL_HZ)]
    return [sum(x) / len(oscillators) for x in zip(*oscillators)]


def hat(length, tau):
    tone = hp(bp(metal(length), 7500, 0.8), 6000)
    fizz = hp(noise(length, 808), 9000)
    return mul(mix((1.0, tone), (0.15, fizz)), env(length, tau))


def hat_closed():
    return hat(0.12, 0.02)


def hat_open():
    return hat(0.7, 0.16)


def snare_909():
    length = 0.35
    low = mul(sweep_sine(length, 205, 185, 0.03), env(length, 0.045))
    high = mul(sweep_sine(length, 360, 330, 0.03), env(length, 0.03))
    rattle = mul(lp(hp(noise(length, 99), 1500), 9000), env(length, 0.11))
    return mix((0.6, low), (0.35, high), (0.55, rattle))


def clap_808():
    length = 0.45
    burst = bp(noise(length, 1808), 1100, 1.2)
    shape = [0.0] * samples(length)
    for onset in (0.0, 0.011, 0.022):
        for i, x in enumerate(env(length, 0.003, delay=onset)):
            shape[i] += x
    for i, x in enumerate(env(length, 0.09, attack=0.002, delay=0.03)):
        shape[i] += 0.7 * x
    return mul(burst, shape)


def rimshot():
    length = 0.12
    ring = mix((1.0, mul(sweep_sine(length, 1710, 1700, 1.0), env(length, 0.007))),
               (0.6, mul(sweep_sine(length, 455, 450, 1.0), env(length, 0.01))))
    click = mul(hp(noise(length, 5), 2000), env(length, 0.0008))
    return hp(mix((1.0, ring), (0.4, click)), 300)


def cowbell():
    length = 0.5
    tone = bp(mix((1.0, square(length, 540)), (1.0, square(length, 800))), 800, 0.9)
    tone = lp(tone, 5000)
    shape = mix((0.7, env(length, 0.015)), (0.3, env(length, 0.15)))
    return mul(tone, shape)


def shaker():
    length = 0.18
    grain = bp(hp(noise(length, 21), 4000), 7000, 0.7)
    shape = [min(1.0, t / 0.015) * math.exp(-max(0.0, t - 0.015) / 0.045) for t in times(length)]
    return mul(grain, shape)


def tambourine():
    length = 0.45
    rng = random.Random(33)
    # The jingles: inharmonic partials that ring on after the hit, each
    # struck a few times in quick succession as the frame shakes.
    ring = [0.0] * samples(length)
    for f, tau in ((5300, 0.12), (6100, 0.09), (7300, 0.11), (8900, 0.07), (10200, 0.06),
                   (11700, 0.05)):
        partial = sweep_sine(length, f, f, 1.0, phase=rng.random())
        for onset, level in ((0.0, 1.0), (0.009, 0.7), (0.02, 0.5)):
            for i, x in enumerate(env(length, tau, delay=onset)):
                ring[i] += level * x * partial[i]
    rattle = mul(hp(noise(length, 34), 5000), env(length, 0.03))
    return hp(mix((1.0, ring), (0.8, rattle)), 3000)


def clave():
    length = 0.12
    return mix((1.0, mul(sweep_sine(length, 2500, 2500, 1.0), env(length, 0.018))),
               (0.15, mul(sweep_sine(length, 5000, 5000, 1.0), env(length, 0.006))))


# Key order. The first key of each pad is the DUO's synthesised drum (None).
# Each entry: (C++ name, description, generator, peak level).
KICK_PAD = [
    None,
    ("kick_808", "808 kick", kick_808, 0.95),
    ("kick_909", "909 kick", kick_909, 0.95),
    ("kick_acoustic", "Acoustic kick", kick_acoustic, 0.95),
    ("kick_lofi", "Lo-fi kick", kick_lofi, 0.85),
    ("zap", "Electro zap", zap, 0.8),
    ("sub_drop", "Sub drop", sub_drop, 0.95),
    ("tom_low", "Low tom", tom_low, 0.85),
    ("tom_high", "High tom", tom_high, 0.8),
    ("conga", "Conga", conga, 0.75),
]

HAT_PAD = [
    None,
    ("hat_closed", "808 closed hat", hat_closed, 0.6),
    ("hat_open", "808 open hat", hat_open, 0.55),
    ("snare_909", "909 snare", snare_909, 0.85),
    ("clap_808", "808 clap", clap_808, 0.8),
    ("rimshot", "Rimshot", rimshot, 0.7),
    ("cowbell", "Cowbell", cowbell, 0.7),
    ("shaker", "Shaker", shaker, 0.5),
    ("tambourine", "Tambourine", tambourine, 0.65),
    ("clave", "Clave", clave, 0.6),
]

KEYS = 10
assert len(KICK_PAD) == KEYS and len(HAT_PAD) == KEYS


def finish(xs, peak, floor_db=-60.0):
    """Trims the silent tail, fades out the end and scales to `peak` of full scale.

    The fade takes the last 15% of the sound, between 20 and 150 ms, so a sound
    cut short while still ringing dies away rather than stopping.
    """
    loudest = max(abs(x) for x in xs)
    floor = loudest * 10 ** (floor_db / 20)
    end = max(i for i, x in enumerate(xs) if abs(x) > floor) + 1
    xs = xs[:end]
    fade = min(max(samples(0.02), len(xs) * 15 // 100), samples(0.15), len(xs))
    for i in range(fade):
        xs[len(xs) - fade + i] *= 0.5 * (1 + math.cos(math.pi * (i + 1) / fade))
    scale = peak * 32767 / loudest
    return [max(-32768, min(32767, int(round(x * scale)))) for x in xs]


def render():
    """Returns [(pad name, [(entry, pcm) or None per key])]."""
    pads = []
    for pad_name, pad in (("KICK_PAD_SOUNDS", KICK_PAD), ("HAT_PAD_SOUNDS", HAT_PAD)):
        sounds = []
        for entry in pad:
            if entry is None:
                sounds.append(None)
            else:
                _, _, make, peak = entry
                sounds.append((entry, finish(make(), peak)))
        pads.append((pad_name, sounds))
    return pads


def write_cpp(path, pads):
    lines = [
        "/*",
        " * Generated by zephyr/app/drumkit/drumkit.py. Do not edit; change the",
        " * script instead. 16 bit mono at 44.1 kHz, one sound per keyboard key.",
        " */",
        "",
        '#include "drum_kits.h"',
        "",
        "namespace duo {",
        "",
    ]
    for _, sounds in pads:
        for sound in sounds:
            if sound is None:
                continue
            (name, description, _, _), pcm = sound
            lines.append(f"/* {description}: {len(pcm)} samples, {len(pcm) / SR * 1000:.0f} ms */")
            lines.append(f"static const int16_t {name}[] = {{")
            for i in range(0, len(pcm), 16):
                lines.append("\t" + ", ".join(str(v) for v in pcm[i:i + 16]) + ",")
            lines.append("};")
            lines.append("")
    for pad_name, sounds in pads:
        lines.append(f"const DrumSample {pad_name}[PAD_SOUNDS] = {{")
        for sound in sounds:
            if sound is None:
                lines.append("\t{nullptr, 0}, /* the DUO's synthesised drum */")
            else:
                (name, description, _, _), _ = sound
                lines.append(f"\t{{{name}, sizeof({name}) / sizeof({name}[0])}}, /* {description} */")
        lines.append("};")
        lines.append("")
    lines.append("} /* namespace duo */")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def write_wavs(directory, pads):
    os.makedirs(directory, exist_ok=True)
    for pad_name, sounds in pads:
        pad = "kick" if pad_name.startswith("KICK") else "hat"
        for key, sound in enumerate(sounds, start=1):
            if sound is None:
                continue
            (name, _, _, _), pcm = sound
            with wave.open(os.path.join(directory, f"{pad}-pad-{key:02d}-{name}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(SR)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", help="write the C++ source for the firmware here")
    parser.add_argument("--wav", metavar="DIR", help="write one WAV per sound into DIR")
    parser.add_argument("--list", action="store_true", help="list the sounds in key order")
    args = parser.parse_args()

    if args.list:
        for pad, entries in (("Kick pad", KICK_PAD), ("Hat pad", HAT_PAD)):
            print(pad)
            for key, entry in enumerate(entries, start=1):
                print(f"  key {key:2d}: {'DUO synth (built in)' if entry is None else entry[1]}")
    if not (args.out or args.wav):
        return

    pads = render()
    if args.out:
        write_cpp(args.out, pads)
    if args.wav:
        write_wavs(args.wav, pads)


if __name__ == "__main__":
    main()
