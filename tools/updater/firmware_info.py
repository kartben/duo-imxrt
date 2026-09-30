#!/usr/bin/env python

import usb
from typing import NamedTuple, Optional

DUO_VID = 0x16D0
DUO_PID = 0x10A7

class FirmwareInfo(NamedTuple):
    serial_no: Optional[str]
    tag: Optional[str]
    branch: Optional[str]
    commit: Optional[str]
    board: Optional[str]

def _get_string(dev, index):
    # Older firmware may not provide every string, and on Linux reading them
    # at all needs access to the USB device. Neither is a reason to give up
    # on the update, so a missing string is reported as None.
    try:
        return usb.util.get_string(dev, index)
    except (usb.core.USBError, ValueError, NotImplementedError):
        return None

def get_firmware_info():
    try:
        dev = usb.core.find(idVendor=DUO_VID, idProduct=DUO_PID)
    except usb.core.NoBackendError:
        print('No USB backend available to query the DUO (is libusb installed?)')
        return None

    if dev is None:
        print('No DUO connected.')
        return None
    else:
        return FirmwareInfo(
                serial_no = _get_string(dev, 3),
                tag = _get_string(dev, 4),
                branch = _get_string(dev, 5),
                commit = _get_string(dev, 6),
                board = _get_string(dev, 7)
            )

def print_firmware_info(info=None):
    if info is None:
        info = get_firmware_info()
    if info is not None:
        for k, v in info._asdict().items():
            print(f"{k}: {v if v is not None else '(not reported)'}")

if __name__ == "__main__":
    print_firmware_info()
