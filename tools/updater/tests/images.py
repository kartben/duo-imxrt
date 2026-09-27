"""Synthetic DUO firmware images for the tests.

The images are laid out like real ones - FCB at 0x60000400, IVT at 0x60001000,
vector table at 0x60002000 - with just enough filled in to satisfy the checks
the boot ROM makes.
"""

import struct

FLASH_BASE = 0x60000000


def make_image(
    layout="zephyr",
    size=0x3000,
    boot_data_size=None,
    ivt_self=0x60001000,
    entry=0x60002000,
    initial_sp=0x20209700,
    reset=0x60002101,
    board=b"DUO_BRAINS_2.1",
    padding=0x00,
):
    """Returns an image as a build would emit it, starting at 0x60000000 (zephyr) or 0x60000400 (legacy)."""
    data = bytearray([0xFF] * size)
    data[0:0x400] = bytes([padding]) * 0x400

    fcb = 0x400
    data[fcb : fcb + 0x200] = bytes(0x200)
    data[fcb : fcb + 4] = b"FCFB"
    struct.pack_into("<I", data, fcb + 0x04, 0x56010400)
    data[fcb + 0x44] = 1  # serial NOR
    data[fcb + 0x45] = 4  # quad
    struct.pack_into("<I", data, fcb + 0x50, 16 * 1024 * 1024)
    struct.pack_into("<I", data, fcb + 0x80, 0x0A1804EB)  # read sequence

    if boot_data_size is None:
        boot_data_size = size if layout == "zephyr" else 16 * 1024 * 1024

    ivt = 0x1000
    struct.pack_into(
        "<8I", data, ivt, 0x412000D1, entry, 0, 0, 0x60001020, ivt_self, 0, 0
    )
    struct.pack_into("<3I", data, 0x1020, FLASH_BASE, boot_data_size, 0)

    struct.pack_into("<2I", data, 0x2000, initial_sp, reset)

    if board:
        data[0x2800 : 0x2800 + len(board) + 1] = board + b"\0"

    if layout == "legacy":
        return bytes(data[0x400:])
    return bytes(data)
