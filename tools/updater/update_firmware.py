#!/usr/bin/env python3

import argparse
import sys
import time
import rtmidi
from rtmidi.midiutil import open_midioutput
from os.path import basename,dirname,abspath
from spsdk.sdp import SDP
import spsdk.sdp.interfaces.usb as sdp_usb
import spsdk.mboot.interfaces.usb as mboot_usb
from spsdk.mboot import McuBoot
from firmware_info import get_firmware_info, print_firmware_info
import duo_image
from duo_image import ImageError

SDP_USB_ID = "0x1FC9,0x0145"
MBOOT_USB_ID = "0x15A2,0x0073"

FLASHLOADER_ADDRESS = 0x20205800

# How long the DUO gets to re-enumerate after each reboot.
ENUMERATION_TIMEOUT_S = 10


class UpdateError(Exception):
    pass


def find_duo_midi_port():
    for (index, name) in enumerate(rtmidi.MidiOut().get_ports()):
        if "duo" in name.lower():
            print(f"Found {name} connected to MIDI")
            return index
    return None


def enter_bootloader():
    duo_port = find_duo_midi_port()

    if duo_port is None:
        print("Could not detect DUO midi port.")
        return False
    else:
        midiout, portname = open_midioutput(duo_port, use_virtual=False)
        print(f"Sending reset signal to {portname} at port {duo_port}")
        reset_syx = [0xF0, 0x7d, 0x64, 0x0b, 0xF7]
        midiout.send_message(reset_syx)
        return True


def _scan(scan, usb_id):
    interfaces = scan(usb_id)
    if len(interfaces) > 1:
        raise UpdateError("More than one DUO in bootloader mode is connected; connect only one.")
    return interfaces[0] if interfaces else None


def find_sdp_interface():
    return _scan(sdp_usb.scan_usb, SDP_USB_ID)


def wait_for(find, what, timeout=ENUMERATION_TIMEOUT_S):
    deadline = time.monotonic() + timeout
    while True:
        interface = find()
        if interface is not None:
            return interface
        if time.monotonic() > deadline:
            raise UpdateError(f"No DUO appeared in {what} within {timeout} s.")
        time.sleep(0.25)


def check_board(image, info, interactive, force):
    """Warns before flashing an image built for a different board revision."""
    if info is None:
        return

    running = info.board

    if image.board is None or running is None:
        if image.board is None:
            print("Could not tell which board revision the image was built for.")
        else:
            print(f"Image is built for {image.board}; the DUO did not report its board revision.")
        return

    if image.board == running:
        return

    print()
    print(f"WARNING: this firmware is built for {image.board}, but the DUO reports {running}.")
    print("It will boot, but hardware that differs between the revisions (such as the")
    print("speaker amplifier) may not work until firmware for the right board is flashed.")

    if force:
        print("Continuing anyway (--force).")
        return
    if not interactive:
        raise UpdateError("Board revision mismatch; pass --force to flash anyway.")
    if input("Flash it anyway? [y/N] ").strip().lower() not in ("y", "yes"):
        raise UpdateError("Update cancelled.")


def load_flashloader(interface, data_path):
    with open(f"{data_path}/ivt_flashloader.bin", "rb") as f:
        flashloader_bytes = f.read()

    print("Sending flashloader")
    with SDP(interface) as s:
        if not s.write_file(FLASHLOADER_ADDRESS, flashloader_bytes):
            raise UpdateError("Could not send the flashloader to the DUO.")
        if not s.jump_and_run(FLASHLOADER_ADDRESS):
            raise UpdateError("Could not start the flashloader.")


def write_and_verify(mboot, address, data, what, verify):
    print(f"Writing {what} ({len(data)} bytes) ... ", end="", flush=True)
    if not mboot.write_memory(address, data):
        print("failed")
        raise UpdateError(f"Writing {what} failed ({mboot.status_string}).")
    print("done")

    if not verify:
        return

    print(f"Verifying {what} ... ", end="", flush=True)
    readback = mboot.read_memory(address, len(data))
    if readback != data:
        print("failed")
        if readback is None:
            raise UpdateError(f"Reading back {what} failed ({mboot.status_string}).")
        first = next(i for i in range(min(len(data), len(readback))) if readback[i] != data[i])
        raise UpdateError(f"{what.capitalize()} does not match the image, first at 0x{address + first:08x}.")
    print("done")


def flash_image(boot_interface, image, verify):
    with McuBoot(boot_interface) as mboot:
        if mboot.get_property(1, 0) is None:
            raise UpdateError("The flashloader is not responding.")

        # Probe the QSPI flash: option 0xC0000007 asks the flashloader to read
        # its parameters from the chip itself (quad, 133 MHz).
        if not (mboot.fill_memory(0x20202000, 4, 0xC0000007)
                and mboot.fill_memory(0x20202004, 4, 0)
                and mboot.configure_memory(0x20202000, 9)):
            raise UpdateError(f"Could not configure the flash ({mboot.status_string}).")

        start = duo_image.FLASH_BASE
        end = -(-image.end_address // duo_image.SECTOR_SIZE) * duo_image.SECTOR_SIZE

        # From here until the header is written back the DUO has no valid
        # boot header, so if anything goes wrong it comes back up in the
        # serial downloader and the update can simply be run again.
        try:
            print("Erasing flash ... ", end="", flush=True)
            if not mboot.flash_erase_region(start, end - start):
                print("failed")
                raise UpdateError(f"Erasing the flash failed ({mboot.status_string}).")
            print("done")

            write_and_verify(mboot, duo_image.IVT_ADDRESS, image.body(), "firmware", verify)

            # The boot header goes last: only a complete, verified image is
            # ever marked bootable.
            write_and_verify(mboot, duo_image.LOAD_ADDRESS, image.header(), "boot header", verify)
        except UpdateError as e:
            raise UpdateError(
                f"{e}\nThe DUO has not been marked bootable, so it will start up in "
                "bootloader mode; run the updater again."
            ) from e

        print("Resetting")
        if not mboot.reset(reopen=False):
            print("The DUO did not reset; switch it off and on again to start the new firmware.")


def update_firmware(image, data_path, interactive, skip_enter_bootloader=False, verify=True, force=False):
    try:
        if find_sdp_interface() is not None:
            print("DUO is already in bootloader mode")
        elif not skip_enter_bootloader:
            info = get_firmware_info()
            print_firmware_info(info)
            check_board(image, info, interactive, force)

            if not enter_bootloader():
                if interactive:
                    input("Please enter bootloader manually, then press Enter.")
                else:
                    print("Continuing. [Continuous mode]")
        else:
            print("not entering bootloader as skip_enter_bootloader is set")

        try:
            interface = wait_for(find_sdp_interface, "SDP host mode")
        except UpdateError as e:
            raise UpdateError(
                f"{e} A DUO running the Zephyr firmware can be put in bootloader mode by "
                "switching it on while holding both arrow buttons."
            ) from e
        print(f"Found {interface.product_name}")

        load_flashloader(interface, data_path)

        print("Sent flashloader. Rebooting.")
        boot_interface = wait_for(lambda: _scan(mboot_usb.scan_usb, MBOOT_USB_ID), "MBOOT mode")

        flash_image(boot_interface, image, verify)
    except UpdateError as e:
        print(e)
        return False

    print("Update complete.")
    return True


def main():
    script_path = abspath(dirname(sys.argv[0]))
    data_path = f"{script_path}/data"

    parser = argparse.ArgumentParser(prog="DUO firmware updater")
    parser.add_argument('firmware_path', nargs='?', default=f"{script_path}/duo_firmware.bin", )
    parser.add_argument(
        '-c', '--continuous', action='store_true',
        help="Disable user interaction and keep polling after successful or failed updates."
    )
    parser.add_argument(
        '-f', '--factory', action='store_true',
        help="Only flash firmware if a blank chip is detected."
    )
    parser.add_argument(
        '--check', action='store_true',
        help="Only check that the file is a bootable DUO firmware image; do not flash it."
    )
    parser.add_argument(
        '--force', action='store_true',
        help="Flash even if the image is built for a different board revision than the DUO reports."
    )
    parser.add_argument(
        '--no-verify', action='store_true',
        help="Skip reading the flash back after writing it."
    )
    args = parser.parse_args()

    try:
        image = duo_image.load_file(args.firmware_path)
    except FileNotFoundError:
        print(f"Firmware file {args.firmware_path} not found. Please specify a file location.")
        sys.exit(1)
    except ImageError as e:
        print(f"Refusing to flash {basename(args.firmware_path)}: {e}")
        sys.exit(1)

    print(f"{basename(args.firmware_path)}:")
    print(duo_image.describe(image))
    print()

    if args.check:
        sys.exit(0)

    verify = not args.no_verify

    if args.continuous:
        print("Polling [continuous mode].")
        print()
        while True:
            time.sleep(3)
            update_firmware(image, data_path, False, verify=verify, force=args.force)
            print()
    elif args.factory:
        print("Factory flashing [continuous mode].")
        print()
        while True:
            time.sleep(1)
            update_firmware(image, data_path, False, True, verify=verify, force=args.force)
            print()
    else:
        ok = update_firmware(image, data_path, True, verify=verify, force=args.force)
        print()
        sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
