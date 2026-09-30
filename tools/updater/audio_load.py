#!/usr/bin/env python3
"""Reports how much of the DUO's CPU time audio rendering takes.

Asks the Zephyr firmware for its audio load over MIDI (sysex F0 7D 64 40 F7)
and prints how long rendering a block takes against how long that block
lasts. Each reading covers the time since the previous one. The legacy
firmware does not answer.
"""

import argparse
import time

import rtmidi

SAMPLE_RATE = 44100
AUDIO_LOAD_QUERY = [0xF0, 0x7D, 0x64, 0x40, 0xF7]
FIELDS = ("cpu_mhz", "block_frames", "render_avg_cycles", "render_max_cycles", "blocks", "underruns")


def open_duo_port(midi):
    for index, name in enumerate(midi.get_ports()):
        if "duo" in name.lower():
            midi.open_port(index)
            return name
    return None


def decode(message):
    """Returns the fields of an audio load reply, or None if it is not one."""
    if message[:4] != AUDIO_LOAD_QUERY[:4] or len(message) != 5 + 5 * len(FIELDS):
        return None
    values = []
    for i in range(len(FIELDS)):
        value = 0
        for byte in message[4 + 5 * i:9 + 5 * i]:
            value = (value << 7) | byte
        values.append(value)
    return dict(zip(FIELDS, values))


def query(midi_in, midi_out, timeout=1.0):
    while midi_in.get_message():
        pass
    midi_out.send_message(AUDIO_LOAD_QUERY)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        message = midi_in.get_message()
        if message:
            load = decode(message[0])
            if load:
                return load
        else:
            time.sleep(0.005)
    return None


def report(load):
    block_us = load["block_frames"] * 1e6 / SAMPLE_RATE
    avg_us = load["render_avg_cycles"] / load["cpu_mhz"]
    max_us = load["render_max_cycles"] / load["cpu_mhz"]
    per_frame = load["render_avg_cycles"] / load["block_frames"]
    print(f"render: mean {avg_us:6.1f} us ({100 * avg_us / block_us:4.1f}%), "
          f"worst {max_us:6.1f} us ({100 * max_us / block_us:4.1f}%) "
          f"of {block_us:.1f} us per block; "
          f"{per_frame:.0f} cycles per frame at {load['cpu_mhz']} MHz; "
          f"{load['blocks']} blocks, {load['underruns']} underruns since start-up")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-i", "--interval", type=float, default=1.0,
                        help="seconds each reading covers (default 1)")
    parser.add_argument("-n", "--count", type=int, default=5,
                        help="number of readings, 0 to run until interrupted (default 5)")
    args = parser.parse_args()

    midi_in = rtmidi.MidiIn()
    midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
    midi_out = rtmidi.MidiOut()
    if open_duo_port(midi_in) is None or open_duo_port(midi_out) is None:
        raise SystemExit("No DUO connected over MIDI.")

    # The first reading covers everything since start-up; start a fresh window.
    if query(midi_in, midi_out) is None:
        raise SystemExit("The DUO did not answer; is it running the Zephyr firmware?")

    readings = 0
    try:
        while args.count == 0 or readings < args.count:
            time.sleep(args.interval)
            load = query(midi_in, midi_out)
            if load is None:
                print("no reply")
            else:
                report(load)
            readings += 1
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
