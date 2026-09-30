#!/usr/bin/env python3
"""Checks that a file is a bootable DUO firmware image before it is flashed.

The i.MX RT1011 on the DUO boots from its QSPI flash only if it finds, at fixed
offsets, a FlexSPI configuration block (FCB) describing the flash, and an image
vector table (IVT) pointing at the firmware. If either is missing or points
somewhere wrong, the boot ROM falls back to the USB serial downloader and the
DUO looks dead until it is reflashed. Everything here is about refusing such an
image on the host instead of discovering the problem on the instrument.

Two layouts of the same image are in circulation:

* The legacy MCUXpresso build (brains2/) links its first section, the FCB, at
  0x60000400, so its .bin starts with the FCB.
* A Zephyr build (zephyr/) starts at the beginning of flash, 0x60000000, so its
  zephyr.bin has 1 KiB of padding in front of the FCB.

The updater writes at 0x60000400, so a Zephyr image has to lose its padding
first. load_image() recognises both and returns the image in the updater's
layout; it is also what the Zephyr build uses to produce duo_firmware.bin, so a
build that would not boot fails at build time.

This module deliberately depends on nothing outside the standard library, so
that the build can run it without the updater's environment.
"""

import re
import struct
import sys
from dataclasses import dataclass, field

FLASH_BASE = 0x60000000
FLASH_SIZE = 16 * 1024 * 1024
SECTOR_SIZE = 4096

# Where the updater writes, and where a legacy image starts.
LOAD_ADDRESS = 0x60000400
FCB_ADDRESS = 0x60000400
IVT_ADDRESS = 0x60001000

FCB_TAG = b"FCFB"
FCB_SIZE = 512
IVT_TAG = 0xD1
IVT_SIZE = 32

# Regions the reset stack pointer may point into (one past the top is fine,
# the stack grows down). DTCM and OCRAM at their reset sizes.
RAM_REGIONS = (
    (0x20000000, 0x20008000),
    (0x20200000, 0x20210000),
)

# The firmware reports its board revision as a USB string descriptor, so the
# name is in the image as plain text in both builds.
BOARD_RE = re.compile(rb"DUO_BRAINS_[0-9][0-9A-Za-z.\-]*")


class ImageError(Exception):
    """The file would not boot on a DUO."""


@dataclass
class FirmwareImage:
    """A validated image, laid out to be written at LOAD_ADDRESS."""

    data: bytes
    layout: str
    entry: int
    initial_sp: int
    reset_handler: int
    boards: list = field(default_factory=list)

    @property
    def end_address(self):
        return LOAD_ADDRESS + len(self.data)

    def header(self):
        """The FCB and everything up to the IVT, which the updater writes last."""
        return self.data[: IVT_ADDRESS - LOAD_ADDRESS]

    def body(self):
        """The IVT onwards, written first so an interrupted update is not bootable."""
        return self.data[IVT_ADDRESS - LOAD_ADDRESS :]

    @property
    def board(self):
        """The board revision the image was built for, if it names exactly one."""
        return self.boards[0] if len(self.boards) == 1 else None


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _offset(address, data, what, length=4):
    """Converts an absolute address into an offset into data, bounds-checked."""
    offset = address - LOAD_ADDRESS
    if offset < 0 or offset + length > len(data):
        raise ImageError(
            f"{what} at 0x{address:08x} is outside the image "
            f"(0x{LOAD_ADDRESS:08x}-0x{LOAD_ADDRESS + len(data):08x})"
        )
    return offset


def _normalise(raw):
    if raw[:4] == FCB_TAG:
        return bytes(raw), "legacy"

    pad = FCB_ADDRESS - FLASH_BASE
    if raw[pad : pad + 4] == FCB_TAG:
        padding = raw[:pad]
        # The padding is never written, so it had better not hold anything.
        if padding.count(0) != pad and padding.count(0xFF) != pad:
            raise ImageError(
                "image starts at the beginning of flash but has data in front "
                "of the FlexSPI configuration block, which the updater would drop"
            )
        return bytes(raw[pad:]), "zephyr"

    raise ImageError(
        "no FlexSPI configuration block at the start of the image or at offset "
        f"0x{pad:x}; this is not a DUO firmware image (was an .elf or .hex "
        "passed instead of the .bin?)"
    )


def _check_fcb(data):
    if len(data) < FCB_SIZE:
        raise ImageError("image is too short to hold a FlexSPI configuration block")

    version = _u32(data, 0x04)
    if version >> 24 != ord("V"):
        raise ImageError(f"FlexSPI configuration block has unknown version 0x{version:08x}")

    device_type = data[0x44]
    if device_type != 1:
        raise ImageError(
            f"FlexSPI configuration block describes device type {device_type}, "
            "not the serial NOR flash the DUO boots from"
        )

    flash_size = _u32(data, 0x50)
    if flash_size == 0 or flash_size > FLASH_SIZE:
        raise ImageError(f"FlexSPI configuration block gives a flash size of {flash_size} bytes")

    # The boot ROM reads the image through sequence 0 of the lookup table.
    if _u32(data, 0x80) == 0:
        raise ImageError("FlexSPI configuration block has no read sequence")


def _check_ivt(data):
    ivt = _offset(IVT_ADDRESS, data, "image vector table", IVT_SIZE)
    header, entry, _, dcd, boot_data, self_ptr, csf, _ = struct.unpack_from("<8I", data, ivt)

    tag = header & 0xFF
    length = ((header >> 8) & 0xFF) << 8 | ((header >> 16) & 0xFF)
    version = header >> 24
    if tag != IVT_TAG or length != IVT_SIZE or version >> 4 != 4:
        raise ImageError(f"no valid image vector table at 0x{IVT_ADDRESS:08x} (header 0x{header:08x})")

    if self_ptr != IVT_ADDRESS:
        raise ImageError(
            f"image vector table says it lives at 0x{self_ptr:08x} rather than "
            f"0x{IVT_ADDRESS:08x}; the image is linked for a different address"
        )

    for name, pointer in (("DCD", dcd), ("CSF", csf)):
        if pointer:
            _offset(pointer, data, name)

    bd = _offset(boot_data, data, "boot data", 12)
    start, size, plugin = struct.unpack_from("<3I", data, bd)
    if start != FLASH_BASE:
        raise ImageError(f"boot data places the image at 0x{start:08x}, not 0x{FLASH_BASE:08x}")
    if plugin != 0:
        raise ImageError("boot data marks the image as a boot plugin")
    # Legacy builds declare the whole flash, Zephyr builds the image itself;
    # either way it has to cover everything that gets written.
    if size < (LOAD_ADDRESS + len(data)) - FLASH_BASE:
        raise ImageError(f"boot data declares {size} bytes, less than the image holds")

    return entry


def _check_vectors(data, entry):
    vt = _offset(entry, data, "vector table", 8)
    initial_sp, reset = struct.unpack_from("<2I", data, vt)

    if initial_sp % 4 or not any(lo < initial_sp <= hi for lo, hi in RAM_REGIONS):
        raise ImageError(f"initial stack pointer 0x{initial_sp:08x} is not in RAM")

    if not reset & 1:
        raise ImageError(f"reset handler 0x{reset:08x} is not Thumb code")
    _offset(reset & ~1, data, "reset handler", 2)

    return initial_sp, reset


def load_image(raw):
    """Validates raw as a DUO firmware image, returning it in the updater's layout."""
    data, layout = _normalise(raw)

    if LOAD_ADDRESS + len(data) > FLASH_BASE + FLASH_SIZE:
        raise ImageError(f"image is {len(data)} bytes and does not fit in flash")

    _check_fcb(data)
    entry = _check_ivt(data)
    initial_sp, reset = _check_vectors(data, entry)

    boards = sorted({m.decode("ascii") for m in BOARD_RE.findall(data)})

    return FirmwareImage(
        data=data,
        layout=layout,
        entry=entry,
        initial_sp=initial_sp,
        reset_handler=reset,
        boards=boards,
    )


def load_file(path):
    with open(path, "rb") as f:
        return load_image(f.read())


def describe(image):
    lines = [
        f"layout:        {image.layout}",
        f"size:          {len(image.data)} bytes "
        f"(0x{LOAD_ADDRESS:08x}-0x{image.end_address:08x})",
        f"vector table:  0x{image.entry:08x}",
        f"reset handler: 0x{image.reset_handler:08x}",
        f"stack pointer: 0x{image.initial_sp:08x}",
        f"built for:     {', '.join(image.boards) or 'unknown board'}",
    ]
    return "\n".join(lines)


def main(argv):
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", help="firmware .bin (legacy duo_firmware.bin or Zephyr zephyr.bin)")
    parser.add_argument(
        "-o",
        "--output",
        help="write the image in the updater's layout (starting at the FCB) to this file",
    )
    args = parser.parse_args(argv)

    try:
        image = load_file(args.image)
    except (OSError, ImageError) as e:
        print(f"{args.image}: {e}", file=sys.stderr)
        return 1

    if args.output:
        with open(args.output, "wb") as f:
            f.write(image.data)
    else:
        print(describe(image))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
