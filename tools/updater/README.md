# Installation
Make sure `uv` [is installed](https://docs.astral.sh/uv/#installation).

To install necessary depencencies:
`uv sync`

# Usage
`uv run update_firmware.py [firmware-file-path]`

If `firmware-file-path` is not supplied, the updater defaults to `duo_firmware.bin` in the current directory.

Both legacy firmware images and Zephyr builds (`duo_firmware.bin`, or `zephyr.bin`, which the updater lays out the same way) are accepted.

| Option | Effect |
| --- | --- |
| `--check` | Only check that the file is a bootable DUO firmware image, and print what it contains; nothing is sent to the DUO |
| `--force` | Flash even if the image is built for a different board revision than the DUO reports |
| `--no-verify` | Skip reading the flash back after writing it |
| `-c`, `--continuous` | Keep polling for DUOs and update each one, without asking questions |
| `-f`, `--factory` | As `--continuous`, for DUOs that are already in bootloader mode |

`uv run firmware_info.py` prints what the connected DUO reports about its firmware and board revision.

`uv run audio_load.py` prints how much of the CPU the Zephyr firmware spends rendering audio (mean and worst time per block against the block's length) and how many times the audio output has run dry since start-up. The legacy firmware does not report this.

# How an update stays safe
Before anything is sent to the DUO, the file is checked the way the boot ROM will check it: FlexSPI configuration block, image vector table, boot data and vector table, and that it fits (`duo_image.py`, which the Zephyr build also runs on every image). A file that would not boot — an `.elf` or `.hex` instead of the `.bin`, a truncated download, an image linked for another address — is refused. If the image names a different board revision than the DUO reports, the updater asks first.

It then erases the flash, writes the firmware from the image vector table onwards and reads it back, and only once that matches writes and verifies the boot header in front of it. Until then the flash holds no valid boot header, so an update that is interrupted leaves the DUO starting up in bootloader mode. If a step fails, the updater erases the boot header again and restarts the DUO, which comes back in bootloader mode; if it cannot even do that, the flashloader is left running. Either way, run the updater again: it recognises a DUO that is in bootloader mode or still running the flashloader, and carries on without asking.

If a DUO running the Zephyr firmware does not respond to the updater, switch it on while holding both arrow buttons to put it in bootloader mode (see `zephyr/README.md`).

Run the tests with `uv run python -m unittest discover -s tests`.


# Issues
On some Linux systems, firmware update might in way that seems like the updater cannot find the Duo USB device.\
This is most probably a permission issue, and can be solved either by running the command with `sudo` (quick fix, but not the best), or by making sure the user has permission to access relevant USB block device (more difficult and system dependent).
