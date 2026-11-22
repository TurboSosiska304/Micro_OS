#!/usr/bin/env python3
"""Micro OS — update the board over USB into the inactive application slot.

The board has two application slots and the bootloader selects one of them
with the otadata partition (ADR 0022). An update writes the new image and its
signature trailer into the slot that is not running, sets the state of that
slot to NEW in otadata and resets the board. The bootloader asks the new
image to confirm itself: `app_main` checks the signature and the shell calls
`esp_ota_mark_app_valid_cancel_rollback()` when the system is up. A boot that
never confirms itself (a panic, a watchdog, a halt, a refused signature) is
marked ABORTED on the next reset and the previous slot starts again.

The tool never writes the bootloader, the partition table, the NVS or
MicroFS: the settings of the board and the data of the user survive an
update. It writes the application of the bundle (`micro_os.bin`), its
trailer (`micro_os.bin.sig`) at the last 4 KiB of the slot, and otadata.

    tools/ota_usb.py [--port PORT] [--dir dist] [--slot auto|ota_0|ota_1]

The path of the bundle is the directory of this script when it is not given,
so a person who unpacked the release can run `python3 ota_usb.py`.

The exit status is 0 when the board was updated, 2 when the port or the
bundle is missing or an argument is wrong, and 1 when the image does not
pass its own check or a write failed.
"""

from __future__ import annotations

import argparse
import binascii
import glob
import os
import shlex
import struct
import subprocess
import sys
import tempfile

ESP_PARTITION_MAGIC = 0x50AA
TABLE_ENTRY_SIZE = 32
OTA_STATE_NEW = 0
SECTOR = 0x1000
TRAILER_SIZE = 120
SLOT_NAMES = ("ota_0", "ota_1")


def find_esptool() -> str:
    """The esptool of the ESP-IDF environment, of the PATH or of pip."""
    for pattern in ("~/.espressif/python_env/*/bin/python", "~/.espressif/python_env/*/bin/python3"):
        for candidate in sorted(glob.glob(os.path.expanduser(pattern))):
            if os.access(candidate, os.X_OK):
                if (
                    subprocess.run(
                        [candidate, "-c", "import esptool"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        check=False,
                    ).returncode
                    == 0
                ):
                    return f"{candidate} -m esptool"

    for name in ("esptool", "esptool.py"):
        if subprocess.run(
            ["sh", "-c", f"command -v {name}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0:
            return name

    return ""


def find_port() -> str:
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        candidates = sorted(glob.glob(pattern))
        if candidates:
            return candidates[0]
    return ""


def parse_partition_table(path: str) -> dict[str, dict]:
    """The entries of partition-table.bin by name."""
    with open(path, "rb") as handle:
        data = handle.read()

    entries = {}
    for offset in range(0, len(data) - TABLE_ENTRY_SIZE + 1, TABLE_ENTRY_SIZE):
        magic, ptype, subtype, address, size = struct.unpack_from("<HBBII", data, offset)
        if magic != ESP_PARTITION_MAGIC:
            break
        label = data[offset + 12 : offset + 28].split(b"\0", 1)[0].decode("ascii", "replace")
        entries[label] = {
            "type": ptype,
            "subtype": subtype,
            "offset": address,
            "size": size,
        }
    return entries


def otadata_state(data: bytes, slot: int) -> dict:
    """One sector of otadata: the sequence, its state and whether it is valid."""
    sector = data[slot * SECTOR : (slot + 1) * SECTOR]
    if len(sector) < 32:
        return {"valid": False, "seq": 0xFFFFFFFF, "state": 0xFFFFFFFF}
    seq, = struct.unpack_from("<I", sector, 0)
    state, = struct.unpack_from("<I", sector, 24)
    crc, = struct.unpack_from("<I", sector, 28)
    expected = binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    valid = seq != 0xFFFFFFFF and crc == expected and state not in (0x3, 0x4)
    return {"valid": valid, "seq": seq, "state": state}


def active_slot(data: bytes) -> tuple[int, int]:
    """The running slot and the highest sequence, or (-1, 0) when blank."""
    entries = [otadata_state(data, i) for i in (0, 1)]
    valid = [entry for entry in entries if entry["valid"]]
    if not valid:
        return -1, 0

    best = max(valid, key=lambda entry: entry["seq"])
    return (best["seq"] - 1) % 2, best["seq"]


def next_sequence(base: int, target: int) -> int:
    """The lowest sequence above `base` that selects `target`."""
    if base <= 0:
        return target + 1

    sequence = base + 1
    while (sequence - 1) % 2 != target:
        sequence += 1
    return sequence


def build_otadata(data: bytes, active: int, target: int, sequence: int) -> bytes:
    """The 8 KiB of otadata with the new entry in the sector not in use."""
    image = bytearray(data)
    if len(image) < 2 * SECTOR:
        image.extend(b"\xFF" * (2 * SECTOR - len(image)))

    sector = 0 if active != 0 else 1
    start = sector * SECTOR
    image[start : start + 32] = b"\xFF" * 32
    struct.pack_into("<I", image, start, sequence)
    struct.pack_into("<I", image, start + 24, OTA_STATE_NEW)
    crc = binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF
    struct.pack_into("<I", image, start + 28, crc)

    return bytes(image)


def microos_sign(directory: str) -> str:
    """The verifier of the bundle, when the folder or the build has one."""
    candidates = [
        os.environ.get("MICROOS_SIGNER", ""),
        os.path.join(directory, "microos_sign"),
        os.path.join(os.path.dirname(directory), "build-host", "tools", "microos_sign"),
        os.path.join(os.path.dirname(directory), "build-x86", "microos_sign"),
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "build-host", "tools", "microos_sign"),
    ]
    for candidate in candidates:
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    return ""


def verify_signature(directory: str, image: str, trailer: str) -> bool:
    signer = microos_sign(directory)
    public = os.path.join(directory, "microos-sign.pub")
    if not signer:
        print("ota_usb: no microos_sign in the bundle, the board will check it", file=sys.stderr)
        return True
    if not os.path.isfile(public):
        print("ota_usb: no microos-sign.pub in the bundle, the board will check it", file=sys.stderr)
        return True

    result = subprocess.run(
        [signer, "verify", "--pub", public, "--trailer", trailer, image],
        check=False,
    )
    return result.returncode == 0


def run(command: list[str], dry_run: bool) -> int:
    print("+ " + " ".join(shlex.quote(part) for part in command))
    if dry_run:
        return 0
    return subprocess.run(command, check=False).returncode


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="update the board over USB into the inactive application slot"
    )
    parser.add_argument("--port", "-p", default="", help="the serial port (default: the first)")
    parser.add_argument("--dir", default=os.path.dirname(os.path.abspath(__file__)),
                        help="the bundle (default: the directory of this script)")
    parser.add_argument("--image", default="micro_os.bin", help="the application of the bundle")
    parser.add_argument("--sig", default="micro_os.bin.sig", help="the trailer of the application")
    parser.add_argument("--table", default="partition-table.bin", help="the partition table")
    parser.add_argument("--slot", default="auto", choices=("auto", "ota_0", "ota_1"),
                        help="the slot to write (default: the one that is not running)")
    parser.add_argument("--baud", "-b", default="460800", help="the baud rate of the update")
    parser.add_argument("--force", action="store_true",
                        help="write an image whose trailer does not verify (a test)")
    parser.add_argument("--dry-run", action="store_true", help="print the commands, write nothing")
    args = parser.parse_args(argv)

    image = os.path.join(args.dir, args.image)
    trailer = os.path.join(args.dir, args.sig)
    table = os.path.join(args.dir, args.table)

    for path in (image, trailer, table):
        if not os.path.isfile(path):
            print(f"ota_usb: {path} is missing", file=sys.stderr)
            return 2

    if os.path.getsize(trailer) != TRAILER_SIZE:
        print(f"ota_usb: {trailer} is not {TRAILER_SIZE} bytes", file=sys.stderr)
        return 1

    if not args.force:
        if not verify_signature(args.dir, image, trailer):
            print("ota_usb: the trailer does not verify, the image is not written", file=sys.stderr)
            print("ota_usb: use --force only to test the rollback of the board", file=sys.stderr)
            return 1

    partitions = parse_partition_table(table)
    for name in ("otadata",) + SLOT_NAMES:
        if name not in partitions:
            print(f"ota_usb: {table} has no partition {name}", file=sys.stderr)
            return 2

    slots = partitions
    if slots[SLOT_NAMES[0]]["size"] != slots[SLOT_NAMES[1]]["size"]:
        print("ota_usb: the two application slots are not equal", file=sys.stderr)
        return 2

    esptool = find_esptool()
    if not esptool:
        print("ota_usb: esptool not found (set PATH or use the ESP-IDF environment)", file=sys.stderr)
        return 2

    port = args.port or find_port()
    if not port:
        print("ota_usb: no serial port (use --port /dev/ttyACM0)", file=sys.stderr)
        return 2

    base = shlex.split(esptool) + [
        "--chip",
        "esp32",
        "-p",
        port,
        "-b",
        str(args.baud),
        "--before",
        "default_reset",
    ]

    # The otadata partition is small and read first: the active slot decides
    # where the image goes, and a board that has never been updated has none.
    otadata_offset = slots["otadata"]["offset"]
    otadata_size = max(slots["otadata"]["size"], 2 * SECTOR)

    with tempfile.TemporaryDirectory(prefix="microos-ota.") as work:
        otadata_file = os.path.join(work, "otadata.bin")
        rc = run(
            base + ["read_flash", hex(otadata_offset), hex(otadata_size), otadata_file],
            args.dry_run,
        )
        if rc != 0:
            return 1

        if args.dry_run:
            data = b"\xFF" * otadata_size
        else:
            with open(otadata_file, "rb") as handle:
                data = handle.read()

        active, sequence = active_slot(data)
        if args.slot == "auto":
            target = 0 if active != 0 else 1
        else:
            target = SLOT_NAMES.index(args.slot)
        next_seq = next_sequence(sequence, target)
        new_otadata = build_otadata(data, active, target, next_seq)

        if active == target and not args.force:
            print(
                f"ota_usb: {SLOT_NAMES[target]} is the running slot; "
                "the image would replace the one the board boots",
                file=sys.stderr,
            )
            print("ota_usb: use --force only to test the board", file=sys.stderr)
            return 2

        slot = slots[SLOT_NAMES[target]]
        limit = slot["size"] - SECTOR
        size = os.path.getsize(image)
        if size > limit:
            print(
                f"ota_usb: {image} is {size} bytes, the slot leaves {limit}", file=sys.stderr
            )
            return 1

        trailer_offset = slot["offset"] + slot["size"] - SECTOR
        otadata_new = os.path.join(work, "otadata-new.bin")
        with open(otadata_new, "wb") as handle:
            handle.write(new_otadata)

        print(
            f"ota_usb: the running slot is {SLOT_NAMES[active] if active >= 0 else 'none'}, "
            f"the image goes to {SLOT_NAMES[target]} at 0x{slot['offset']:X}, "
            f"sequence {next_seq}"
        )

        # The image first, the trailer after it, otadata last: a write that
        # stops in the middle leaves the board on the slot it runs now.
        rc = run(
            base
            + [
                "--after",
                "no_reset",
                "write_flash",
                "--flash_mode",
                "dio",
                "--flash_freq",
                "40m",
                "--flash_size",
                "8MB",
                hex(slot["offset"]),
                image,
                hex(trailer_offset),
                trailer,
            ],
            args.dry_run,
        )
        if rc != 0:
            print("ota_usb: the application is not written, the board still boots its slot",
                  file=sys.stderr)
            return 1

        rc = run(
            base + ["write_flash", hex(otadata_offset), otadata_new],
            args.dry_run,
        )
        if rc != 0:
            print("ota_usb: otadata is not written, the board still boots its slot",
                  file=sys.stderr)
            return 1

    print(
        f"ota_usb: {SLOT_NAMES[target]} is next; the board restarts and keeps it "
        "when the boot reaches the shell"
    )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
