"""The updater's write sequence, against a simulated flashloader.

The fake behaves like NOR flash: erasing sets a sector to 0xFF, and programming
can only clear bits, so a write to flash that was not erased shows up as
corrupted data rather than silently succeeding.
"""

import os
import sys
import types
import unittest
from unittest import mock

from spsdk.mboot.exceptions import McuBootConnectionError

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import duo_image
import update_firmware
from firmware_info import FirmwareInfo
from images import make_image

SECTOR = duo_image.SECTOR_SIZE


class FakeMcuBoot:
    def __init__(self, fail=None, corrupt_at=None, short_read=False, disconnect_on=None):
        self.flash = bytearray(b"\x5a" * (64 * 1024))  # an old image, not erased
        self.calls = []
        self.fail = fail or set()
        self.corrupt_at = corrupt_at
        self.short_read = short_read
        self.disconnect_on = disconnect_on
        self.status_string = "OK"

    # McuBoot(interface) is used as a context manager
    def __call__(self, interface):
        return self

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def _ok(self, name):
        self.calls.append(name)
        # Once the device has gone, it stays gone.
        if name[0] == self.disconnect_on:
            self.disconnect_on = "everything"
        if self.disconnect_on == "everything":
            raise McuBootConnectionError("device went away")
        if name[0] in self.fail:
            self.status_string = "kStatus_Fail"
            return False
        return True

    def _offset(self, address):
        return address - duo_image.FLASH_BASE

    def get_property(self, tag, index=0):
        return [1] if self._ok(("get_property",)) else None

    def fill_memory(self, address, length, pattern=0xFFFFFFFF):
        return self._ok(("fill_memory", address))

    def configure_memory(self, address, mem_id):
        return self._ok(("configure_memory", mem_id))

    def flash_erase_region(self, address, length, mem_id=0):
        if not self._ok(("erase", address, length)):
            return False
        assert address % SECTOR == 0 and length % SECTOR == 0
        o = self._offset(address)
        self.flash[o : o + length] = b"\xff" * length
        return True

    def write_memory(self, address, data, mem_id=0, progress_callback=None):
        if not self._ok(("write", address, len(data))):
            return False
        o = self._offset(address)
        for i, b in enumerate(data):
            if self.corrupt_at == address + i:
                b ^= 0x01
            self.flash[o + i] &= b
        return True

    def read_memory(self, address, length, mem_id=0, progress_callback=None, fast_mode=False):
        # Over USB HID, which is how the DUO is reached, spsdk reports a
        # failed read as no data and a read cut short as a prefix.
        if not self._ok(("read", address, length)):
            return b""
        o = self._offset(address)
        if self.short_read:
            length //= 2
        return bytes(self.flash[o : o + length])

    def reset(self, timeout=2000, reopen=True):
        return self._ok(("reset",))

    def bootable(self):
        """Whether the boot ROM would find a valid image in the flash."""
        # Like the ROM, only look as far as the boot data says the image goes;
        # whatever the erase did not reach beyond that is of no interest.
        size = int.from_bytes(self.flash[0x1024:0x1028], "little")
        try:
            duo_image.load_image(bytes(self.flash[0x400:size]))
            return True
        except duo_image.ImageError:
            return False

    def names(self):
        return [c[0] for c in self.calls]


class FlashImageTest(unittest.TestCase):
    def setUp(self):
        self.image = duo_image.load_image(make_image("zephyr", size=0x5000))

    def flash(self, mcuboot, verify=True):
        self.interface = types.SimpleNamespace(timeout=2000)
        with mock.patch.object(update_firmware, "McuBoot", mcuboot):
            update_firmware.flash_image(self.interface, self.image, verify)

    def test_successful_update(self):
        fake = FakeMcuBoot()
        self.flash(fake)

        end = duo_image.LOAD_ADDRESS - duo_image.FLASH_BASE + len(self.image.data)
        self.assertEqual(bytes(fake.flash[0x400:end]), self.image.data)
        self.assertEqual(bytes(fake.flash[:0x400]), b"\xff" * 0x400)
        self.assertTrue(fake.bootable())
        self.assertEqual(fake.names()[-1], "reset")

    def test_erase_covers_whole_sectors_from_start_of_flash(self):
        fake = FakeMcuBoot()
        self.flash(fake)

        erase = next(c for c in fake.calls if c[0] == "erase")
        self.assertEqual(erase[1], duo_image.FLASH_BASE)
        self.assertGreaterEqual(erase[1] + erase[2], self.image.end_address)

    def test_boot_header_is_written_last_and_after_verifying_the_rest(self):
        fake = FakeMcuBoot()
        self.flash(fake)

        sequence = [c for c in fake.calls if c[0] in ("erase", "write", "read", "reset")]
        steps = [(c[0], c[1] if len(c) > 1 else None) for c in sequence]
        self.assertEqual(
            steps,
            [
                ("erase", duo_image.FLASH_BASE),
                ("write", duo_image.IVT_ADDRESS),
                ("read", duo_image.IVT_ADDRESS),
                ("write", duo_image.LOAD_ADDRESS),
                ("read", duo_image.LOAD_ADDRESS),
                ("reset", None),
            ],
        )

    def assert_left_in_bootloader(self, fake):
        self.assertFalse(fake.bootable())
        # If it was reset, the boot header had been erased first.
        if "reset" in fake.names():
            invalidate = ("erase", duo_image.FLASH_BASE, duo_image.SECTOR_SIZE)
            self.assertIn(invalidate, fake.calls)
            self.assertLess(fake.calls.index(invalidate), fake.names().index("reset"))

    def test_failed_write_leaves_the_duo_in_bootloader_mode(self):
        fake = FakeMcuBoot(fail={"write"})

        with self.assertRaisesRegex(update_firmware.UpdateError,
                                    "(?s)Writing firmware failed.*restarted in bootloader mode"):
            self.flash(fake)

        self.assertNotIn(("write", duo_image.LOAD_ADDRESS, len(self.image.header())), fake.calls)
        self.assert_left_in_bootloader(fake)
        self.assertIn("reset", fake.names())

    def test_failed_header_write_erases_the_header_again(self):
        # Everything else was written and verified; only the header is bad.
        fake = FakeMcuBoot(corrupt_at=duo_image.LOAD_ADDRESS + 0x10)

        with self.assertRaisesRegex(update_firmware.UpdateError, "Boot header does not match"):
            self.flash(fake)

        self.assert_left_in_bootloader(fake)
        self.assertEqual(bytes(fake.flash[: duo_image.SECTOR_SIZE]), b"\xff" * duo_image.SECTOR_SIZE)

    def test_lost_connection_is_an_update_error(self):
        fake = FakeMcuBoot(disconnect_on="write")

        with self.assertRaisesRegex(update_firmware.UpdateError, "(?s)device went away.*flashloader"):
            self.flash(fake)

        self.assertNotIn("reset", fake.names())
        self.assertFalse(fake.bootable())

    def test_short_read_back_is_an_error(self):
        fake = FakeMcuBoot(short_read=True)

        with self.assertRaisesRegex(update_firmware.UpdateError, "Reading back firmware failed"):
            self.flash(fake)

        self.assert_left_in_bootloader(fake)

    def test_failed_reset_is_not_a_failed_update(self):
        fake = FakeMcuBoot(fail={"reset"})
        self.flash(fake)

        self.assertTrue(fake.bootable())

    def test_corrupted_write_is_caught_by_verification(self):
        fake = FakeMcuBoot(corrupt_at=duo_image.IVT_ADDRESS + 0x1234)

        with self.assertRaisesRegex(update_firmware.UpdateError, "does not match .* 0x60002234"):
            self.flash(fake)

        self.assert_left_in_bootloader(fake)

    def test_failed_read_back_is_an_error(self):
        fake = FakeMcuBoot(fail={"read"})

        with self.assertRaisesRegex(update_firmware.UpdateError, "Reading back"):
            self.flash(fake)

        self.assert_left_in_bootloader(fake)

    def test_failed_erase_writes_nothing_and_does_not_reset(self):
        fake = FakeMcuBoot(fail={"erase"})

        with self.assertRaisesRegex(update_firmware.UpdateError, "(?s)Erasing.*still in its flashloader"):
            self.flash(fake)

        # The flash is in an unknown state, so the DUO must not be reset into it.
        self.assertNotIn("write", fake.names())
        self.assertNotIn("reset", fake.names())

    def test_erase_gets_time_for_every_sector(self):
        fake = FakeMcuBoot()
        self.flash(fake)

        sectors = -(-self.image.end_address // SECTOR) - duo_image.FLASH_BASE // SECTOR
        self.assertGreaterEqual(self.interface.timeout, sectors * update_firmware.SECTOR_ERASE_MAX_MS)
        self.assertGreaterEqual(self.interface.timeout, 2000)

    def test_unconfigured_flash_is_not_touched(self):
        fake = FakeMcuBoot(fail={"configure_memory"})

        with self.assertRaisesRegex(update_firmware.UpdateError, "configure.*nothing was written"):
            self.flash(fake)

        # Nothing was erased, so it restarts into its old firmware.
        self.assertNotIn("erase", fake.names())
        self.assertEqual(bytes(fake.flash[:0x100]), b"\x5a" * 0x100)
        self.assertEqual(fake.names()[-1], "reset")

    def test_silent_flashloader_is_not_touched(self):
        fake = FakeMcuBoot(fail={"get_property"})

        with self.assertRaisesRegex(update_firmware.UpdateError, "not responding"):
            self.flash(fake)

        self.assertEqual(fake.names(), ["get_property"])

    def test_verification_can_be_skipped(self):
        fake = FakeMcuBoot()
        self.flash(fake, verify=False)

        self.assertNotIn("read", fake.names())
        self.assertTrue(fake.bootable())


def info(board):
    return FirmwareInfo(serial_no="1-2", tag=None, branch=None, commit=None, board=board)


class BoardCheckTest(unittest.TestCase):
    def setUp(self):
        self.image = duo_image.load_image(make_image(board=b"DUO_BRAINS_2.1"))

    def test_matching_board(self):
        update_firmware.check_board(self.image, info("DUO_BRAINS_2.1"), False, False)

    def test_unknown_board_is_not_an_error(self):
        update_firmware.check_board(self.image, info(None), False, False)
        update_firmware.check_board(self.image, None, False, False)

    def test_mismatch_is_refused_without_a_user(self):
        with self.assertRaisesRegex(update_firmware.UpdateError, "mismatch"):
            update_firmware.check_board(self.image, info("DUO_BRAINS_2.3"), False, False)

    def test_mismatch_can_be_forced(self):
        update_firmware.check_board(self.image, info("DUO_BRAINS_2.3"), False, True)

    def test_mismatch_asks_the_user(self):
        with mock.patch("builtins.input", return_value="n"):
            with self.assertRaisesRegex(update_firmware.UpdateError, "cancelled"):
                update_firmware.check_board(self.image, info("DUO_BRAINS_2.3"), True, False)

        with mock.patch("builtins.input", return_value="y"):
            update_firmware.check_board(self.image, info("DUO_BRAINS_2.3"), True, False)


class UpdateFlowTest(unittest.TestCase):
    def setUp(self):
        self.image = duo_image.load_image(make_image(board=b"DUO_BRAINS_2.1"))

    def test_duo_already_in_bootloader_mode_is_flashed_without_asking(self):
        flashloader = object()
        with mock.patch.object(update_firmware, "find_mboot_interface", side_effect=[None, flashloader]), \
             mock.patch.object(update_firmware, "find_sdp_interface", return_value=mock.Mock(product_name="SE Blank")), \
             mock.patch.object(update_firmware, "enter_bootloader") as enter, \
             mock.patch.object(update_firmware, "load_flashloader") as load, \
             mock.patch.object(update_firmware, "flash_image") as flash, \
             mock.patch("builtins.input", side_effect=AssertionError("prompted")):
            self.assertTrue(update_firmware.update_firmware(self.image, "data", True))

        enter.assert_not_called()
        load.assert_called_once()
        flash.assert_called_once_with(flashloader, self.image, True)

    def test_flashloader_left_running_is_picked_up(self):
        flashloader = object()
        with mock.patch.object(update_firmware, "find_mboot_interface", return_value=flashloader), \
             mock.patch.object(update_firmware, "find_sdp_interface") as sdp, \
             mock.patch.object(update_firmware, "load_flashloader") as load, \
             mock.patch.object(update_firmware, "flash_image") as flash, \
             mock.patch("builtins.input", side_effect=AssertionError("prompted")):
            self.assertTrue(update_firmware.update_firmware(self.image, "data", True))

        sdp.assert_not_called()
        load.assert_not_called()
        flash.assert_called_once_with(flashloader, self.image, True)

    def test_failure_is_reported_not_raised(self):
        with mock.patch.object(update_firmware, "find_mboot_interface", return_value=None), \
             mock.patch.object(update_firmware, "find_sdp_interface", return_value=mock.Mock(product_name="SE Blank")), \
             mock.patch.object(update_firmware, "load_flashloader",
                               side_effect=update_firmware.UpdateError("Could not send the flashloader")), \
             mock.patch.object(update_firmware, "flash_image") as flash:
            self.assertFalse(update_firmware.update_firmware(self.image, "data", False))

        flash.assert_not_called()

    def test_lost_connection_is_reported_not_raised(self):
        with mock.patch.object(update_firmware, "find_mboot_interface", return_value=None), \
             mock.patch.object(update_firmware, "find_sdp_interface", return_value=mock.Mock(product_name="SE Blank")), \
             mock.patch.object(update_firmware, "load_flashloader",
                               side_effect=McuBootConnectionError("unplugged")):
            self.assertFalse(update_firmware.update_firmware(self.image, "data", False))

    def test_board_mismatch_stops_before_rebooting_the_duo(self):
        with mock.patch.object(update_firmware, "find_mboot_interface", return_value=None), \
             mock.patch.object(update_firmware, "find_sdp_interface", return_value=None), \
             mock.patch.object(update_firmware, "get_firmware_info", return_value=info("DUO_BRAINS_2.3")), \
             mock.patch.object(update_firmware, "enter_bootloader") as enter:
            self.assertFalse(update_firmware.update_firmware(self.image, "data", False))

        enter.assert_not_called()

if __name__ == "__main__":
    unittest.main()
