#!/usr/bin/env python3
"""Micro OS — the image of the `progs` partition of the board.

The programs of the ESP32 are files of a partition of their own (ADR 0018
§2, ADR 0022), not blobs of the firmware. This tool collects the `.mox`
images that the build of the port makes into the table that
`components/port_esp32/include/progs_format.h` describes:

    the last 4 KiB  the directory: magic, version, count, slot, reserved,
                    and one record per file: name[24], offset, size, reserved
    0x00000         the first file
    0x10000         the second file

The directory is at the end and every file is on a 64 KiB boundary from the
start of the partition: that is the address `esp_partition_mmap` maps, and
the first flash page carries a program and nothing else. The partition holds
two programs; a third one of 1.x is a new image, not a reserve slot. The
image is padded with 0xFF, like the flash, and is deterministic: the same
programs give the same bytes, which is what the reproducible build of the
tree asks for. The loader of 1.1 copies the bytes into the windows of the
port; the alignment is kept for a later loader.

    tools/progs_image.py --output progs.bin [--partition-size 0x21000]
                         [--slot-size 0x10000] NAME=FILE [NAME=FILE ...]

The exit status is 0 when the image is written, 2 for a wrong argument and
1 when a program does not fit or is not a program of this machine.
"""

from __future__ import annotations

import argparse
import struct
import sys

MAGIC = b"MOPROGS1"
VERSION = 1
DIRECTORY_SIZE = 0x1000
NAME_MAX = 23
MAX_FILES = 2
ENTRY_SIZE = 36
HEADER_SIZE = 24
PARTITION_SIZE = 0x21000
SLOT_SIZE = 0x10000

# The header of a program (kernel/exec.h): 'M', 'O', 'X', '1', version 2 and
# the machine in the field at offset 28. A file that is not a program of the
# board is refused before it reaches the flash.
MOX_MAGIC = b"MOX1"
MOX_VERSION = 2
MOX_ARCH_XTENSA = 1

NAME_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


def parse_number(text: str) -> int:
    """A size of the command line: 4096, 0x1000 or 0X1000."""
    try:
        return int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}") from error


def check_name(name: str) -> None:
    if not 1 <= len(name) <= NAME_MAX:
        raise ValueError(f"the name {name!r} is not 1..{NAME_MAX} characters")
    if name in (".", ".."):
        raise ValueError(f"the name {name!r} is one of the tree")
    if not set(name) <= NAME_CHARS:
        raise ValueError(f"the name {name!r} carries a character a path cannot")


def read_program(path: str) -> bytes:
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < 32 or data[0:4] != MOX_MAGIC:
        raise ValueError(f"{path} is not a MOX1 program")
    version = struct.unpack_from("<I", data, 4)[0]
    if version != MOX_VERSION:
        raise ValueError(f"{path} is MOX1 version {version}, not {MOX_VERSION}")
    arch = struct.unpack_from("<I", data, 28)[0]
    if arch != MOX_ARCH_XTENSA:
        raise ValueError(f"{path} is a program of arch {arch}, not Xtensa (1)")

    return data


def build_image(programs: list[tuple[str, str]], partition_size: int, slot_size: int) -> bytes:
    if partition_size <= DIRECTORY_SIZE or slot_size <= 0:
        raise ValueError("the sizes of the partition and of a slot have to be positive")
    if slot_size % 0x1000 != 0 or slot_size < DIRECTORY_SIZE:
        raise ValueError("the slot has to hold the directory and a 4 KiB erase sector")
    if slot_size % DIRECTORY_SIZE != 0:
        raise ValueError("the slot has to be a multiple of the 4 KiB erase sector")

    area = partition_size - DIRECTORY_SIZE
    slots = area // slot_size
    if slots < 1 or len(programs) > slots:
        raise ValueError(
            f"{len(programs)} program(s) do not fit {slots} slot(s) of "
            f"{slot_size} bytes in {partition_size - DIRECTORY_SIZE} bytes"
        )
    if len(programs) > MAX_FILES:
        raise ValueError(f"at most {MAX_FILES} programs fit the table")

    image = bytearray(b"\xFF" * partition_size)
    directory = partition_size - DIRECTORY_SIZE
    image[directory : directory + len(MAGIC)] = MAGIC
    struct.pack_into("<IIII", image, directory + 8, VERSION, len(programs), slot_size, 0)

    names = set()
    for index, (name, path) in enumerate(programs):
        check_name(name)
        if name in names:
            raise ValueError(f"two programs are called {name!r}")
        names.add(name)

        data = read_program(path)
        if len(data) > slot_size:
            raise ValueError(f"{path} is {len(data)} bytes, a slot is {slot_size}")

        offset = index * slot_size
        record = directory + HEADER_SIZE + index * ENTRY_SIZE
        raw = name.encode("ascii") + b"\0"
        image[record : record + 24] = raw + b"\xFF" * (24 - len(raw))
        struct.pack_into("<III", image, record + 24, offset, len(data), 0)
        image[offset : offset + len(data)] = data

    return bytes(image)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="collect the programs of the board into the progs partition"
    )
    parser.add_argument("--output", "-o", required=True, help="the image to write")
    parser.add_argument(
        "--partition-size",
        type=parse_number,
        default=PARTITION_SIZE,
        help=f"the size of the partition (default 0x{PARTITION_SIZE:X})",
    )
    parser.add_argument(
        "--slot-size",
        type=parse_number,
        default=SLOT_SIZE,
        help=f"the slot of a program (default 0x{SLOT_SIZE:X})",
    )
    parser.add_argument(
        "programs",
        nargs="*",
        metavar="NAME=FILE",
        help="the name in /bin and the .mox of the build",
    )
    args = parser.parse_args(argv)

    programs = []
    for item in args.programs:
        name, separator, path = item.partition("=")
        if not separator or not name or not path:
            parser.error(f"a program is NAME=FILE, not {item!r}")
        programs.append((name, path))

    try:
        image = build_image(programs, args.partition_size, args.slot_size)
    except (OSError, ValueError) as error:
        print(f"progs_image: {error}", file=sys.stderr)
        return 1

    with open(args.output, "wb") as handle:
        handle.write(image)

    names = ", ".join(f"/bin/{name}" for name, _ in programs) if programs else "nothing"
    print(
        f"progs_image: {args.output}: {len(programs)} program(s) ({names}), "
        f"{args.partition_size} bytes"
    )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
