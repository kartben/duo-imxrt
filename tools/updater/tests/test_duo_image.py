import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import duo_image
from duo_image import ImageError, load_image
from images import make_image


class LayoutTest(unittest.TestCase):
    def test_zephyr_image_loses_its_padding(self):
        raw = make_image("zephyr")
        image = load_image(raw)

        self.assertEqual(image.layout, "zephyr")
        self.assertEqual(image.data, raw[0x400:])
        self.assertEqual(image.data[:4], b"FCFB")

    def test_legacy_image_is_taken_as_is(self):
        raw = make_image("legacy")
        image = load_image(raw)

        self.assertEqual(image.layout, "legacy")
        self.assertEqual(image.data, raw)

    def test_both_layouts_of_one_build_are_the_same_image(self):
        zephyr = make_image("zephyr", boot_data_size=16 * 1024 * 1024)
        legacy = make_image("legacy")

        self.assertEqual(load_image(zephyr).data, load_image(legacy).data)

    def test_erased_padding_is_accepted(self):
        load_image(make_image("zephyr", padding=0xFF))

    def test_padding_holding_data_is_refused(self):
        raw = bytearray(make_image("zephyr"))
        raw[0x10] = 0x42

        with self.assertRaisesRegex(ImageError, "data in front"):
            load_image(bytes(raw))

    def test_zephyr_image_shifted_by_the_old_updater_is_refused(self):
        # What the old updater would have written: zephyr.bin at 0x60000400,
        # putting the FCB 1 KiB too far into flash.
        shifted = bytes(0x400) + make_image("zephyr")

        with self.assertRaisesRegex(ImageError, "no FlexSPI configuration block"):
            load_image(shifted)

    def test_elf_is_refused(self):
        with self.assertRaisesRegex(ImageError, "not a DUO firmware image"):
            load_image(b"\x7fELF" + bytes(0x2000))

    def test_empty_file_is_refused(self):
        with self.assertRaises(ImageError):
            load_image(b"")

    def test_oversized_image_is_refused(self):
        raw = make_image("legacy") + bytes(16 * 1024 * 1024)

        with self.assertRaisesRegex(ImageError, "does not fit"):
            load_image(raw)


class HeaderTest(unittest.TestCase):
    def test_wrong_fcb_version(self):
        raw = bytearray(make_image("legacy"))
        raw[7] = 0

        with self.assertRaisesRegex(ImageError, "version"):
            load_image(bytes(raw))

    def test_fcb_without_read_sequence(self):
        raw = bytearray(make_image("legacy"))
        raw[0x80:0x84] = bytes(4)

        with self.assertRaisesRegex(ImageError, "read sequence"):
            load_image(bytes(raw))

    def test_missing_ivt(self):
        raw = bytearray(make_image("zephyr"))
        raw[0x1000] = 0xFF

        with self.assertRaisesRegex(ImageError, "image vector table"):
            load_image(bytes(raw))

    def test_image_linked_elsewhere(self):
        with self.assertRaisesRegex(ImageError, "linked for a different address"):
            load_image(make_image("zephyr", ivt_self=0x20001000))

    def test_boot_data_too_small(self):
        with self.assertRaisesRegex(ImageError, "less than the image holds"):
            load_image(make_image("zephyr", boot_data_size=0x1000))

    def test_entry_outside_image(self):
        with self.assertRaisesRegex(ImageError, "vector table .* outside the image"):
            load_image(make_image("zephyr", entry=0x60100000))

    def test_stack_pointer_not_in_ram(self):
        with self.assertRaisesRegex(ImageError, "stack pointer"):
            load_image(make_image("zephyr", initial_sp=0x60001000))

    def test_reset_handler_must_be_thumb(self):
        with self.assertRaisesRegex(ImageError, "Thumb"):
            load_image(make_image("zephyr", reset=0x60002100))

    def test_reset_handler_outside_image(self):
        with self.assertRaisesRegex(ImageError, "reset handler"):
            load_image(make_image("zephyr", reset=0x60200001))


class SplitTest(unittest.TestCase):
    def test_header_ends_where_the_ivt_starts(self):
        image = load_image(make_image("zephyr"))

        self.assertEqual(len(image.header()), duo_image.IVT_ADDRESS - duo_image.LOAD_ADDRESS)
        self.assertEqual(image.header()[:4], b"FCFB")
        self.assertEqual(image.body()[0], 0xD1)
        self.assertEqual(image.header() + image.body(), image.data)


class BoardTest(unittest.TestCase):
    def test_board_is_found(self):
        self.assertEqual(load_image(make_image(board=b"DUO_BRAINS_2.3")).board, "DUO_BRAINS_2.3")

    def test_beta_board_names(self):
        self.assertEqual(load_image(make_image(board=b"DUO_BRAINS_2.0-beta1")).board, "DUO_BRAINS_2.0-beta1")

    def test_no_board(self):
        self.assertIsNone(load_image(make_image(board=None)).board)

    def test_ambiguous_board(self):
        raw = bytearray(make_image(board=b"DUO_BRAINS_2.1"))
        raw[0x2900:0x290F] = b"DUO_BRAINS_2.3\0"

        image = load_image(bytes(raw))
        self.assertEqual(image.boards, ["DUO_BRAINS_2.1", "DUO_BRAINS_2.3"])
        self.assertIsNone(image.board)


class CommandLineTest(unittest.TestCase):
    def test_output_is_in_updater_layout(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "zephyr.bin")
            out = os.path.join(d, "duo_firmware.bin")
            with open(src, "wb") as f:
                f.write(make_image("zephyr"))

            self.assertEqual(duo_image.main([src, "-o", out]), 0)

            with open(out, "rb") as f:
                self.assertEqual(f.read(), make_image("zephyr")[0x400:])

    def test_bad_image_fails(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "zephyr.elf")
            with open(src, "wb") as f:
                f.write(b"\x7fELF" + bytes(0x100))

            self.assertEqual(duo_image.main([src]), 1)


if __name__ == "__main__":
    unittest.main()
