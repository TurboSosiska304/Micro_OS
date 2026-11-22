#!/usr/bin/env python3
"""Micro OS — the image of the disk of the x86-64 port.

The machine has no programs in its kernel any more: `/bin` and `/etc` are
objects of a MicroFS volume on the disk, and the boot mounts that volume and
runs `/bin/hello` through `execve`. This script calls the host tool that
writes the volume (`tools/mkfs_mfs.c`, which links `components/mfs`), so the
format has one implementation and not a second one in Python.

The image is a whole disk: an MBR, one Linux partition at LBA 2048 and a
MicroFS volume in it, with the `.mox` images of the build in `/bin`, an
empty `/etc/init.d` and the settings of the machine in `/etc/setting`.

    tools/x86_image.py [--image PATH] [--build DIR] [--mkfs PATH]
                       [--size-mb N] [--source-date-epoch N] [--verbose]

The exit status is 0 when the image was written, 2 when the tool or one of
the images of a program is missing, and 1 when the tool failed.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

# The programs of /bin, in the order of the list of the port. The name is
# the file in the build directory and the file of the volume.
PROGRAMS = [
    "hello",
    "crash",
    "fork",
    "badptr",
    "signals",
    "cat",
    "echo",
    "ls",
    "wc",
    "head",
    "grep",
    "mkdir",
    "rm",
    "pwd",
    "spawn",
    "nx",
    "pcre2grep",
]

# The license of PCRE2 travels with the machine that carries its program: the
# text of third_party/pcre2/LICENSE is a file of /etc/licenses, so a person
# with the disk can read the terms of the one third-party component of the
# image without the sources.
PCRE2_LICENSE = "third_party/pcre2/LICENSE"

# The one file of /etc the smoke test reads. The tool writes the text and a
# newline; the first line is the word `cat`, `grep` and `wc` expect, the two
# after it are the settings a machine of this port has (the config component
# is not linked here, so the file is data of the image).
SETTING = "hello\nhostname=x86_64\nconsole=vga"

IMAGE_MB = 8
START_LBA = 2048
# The moment of the records when SOURCE_DATE_EPOCH does not say another one.
# Zero is the convention of a reproducible build and "unknown" in MicroFS:
# an image is not made by a clock, and two runs have to give the same bytes.
SOURCE_DATE_EPOCH = 0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")


def build_image(
    image: str,
    build_dir: str = DEFAULT_BUILD,
    mkfs: str = DEFAULT_MKFS,
    size_mb: int = IMAGE_MB,
    epoch: int = SOURCE_DATE_EPOCH,
    verbose: bool = False,
    extra: list | None = None,
) -> int:
    """Write the image with the programs of the build and /etc/setting.

    `extra` is a list of the arguments of the tool that a caller needs on top
    of the tree of the machine: `--file PATH=HOST`, `--text PATH=TEXT` and
    `--dir PATH`. The stress tools use it to put a filler on the volume and
    reach `ENOSPC` in a boot; the image of a person stays the default tree.
    """
    if not os.path.isfile(mkfs):
        print(f"missing {mkfs}: run make build", file=sys.stderr)
        return 2

    command = [
        mkfs,
        "--image",
        image,
        "--size-mb",
        str(size_mb),
        "--start-lba",
        str(START_LBA),
        "--source-date-epoch",
        str(epoch),
        "--dir",
        "/bin",
        "--dir",
        "/etc",
        "--dir",
        "/etc/init.d",
        "--dir",
        "/etc/licenses",
    ]

    for name in PROGRAMS:
        path = os.path.join(build_dir, f"{name}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        command += ["--program", f"/bin/{name}={path}"]

    command += ["--text", f"/etc/setting={SETTING}"]

    license_path = os.path.join(ROOT, PCRE2_LICENSE)
    if not os.path.isfile(license_path):
        print(f"missing {license_path}", file=sys.stderr)
        return 2
    command += ["--file", f"/etc/licenses/pcre2={license_path}"]

    command += extra or []

    os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)

    if verbose:
        print(" ".join(command))

    result = subprocess.run(command, check=False)
    return result.returncode


def parse_epoch(text: str) -> int:
    """The seconds of SOURCE_DATE_EPOCH, or a value argparse refuses."""
    try:
        value = int(text, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text}") from error
    if value < 0:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text}")
    return value


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk image of the x86-64 port")
    parser.add_argument("--image", default=os.path.join(DEFAULT_BUILD, "programs.img"))
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the .mox images")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool that writes the volume")
    parser.add_argument("--size-mb", type=int, default=IMAGE_MB)
    parser.add_argument(
        "--source-date-epoch",
        type=parse_epoch,
        default=os.environ.get("SOURCE_DATE_EPOCH", str(SOURCE_DATE_EPOCH)),
        metavar="N",
        help="the moment of the records (default: $SOURCE_DATE_EPOCH or 0)",
    )
    parser.add_argument(
        "--file",
        action="append",
        default=[],
        metavar="PATH=HOST",
        help="write a file of the host into the volume (mode 0644)",
    )
    parser.add_argument(
        "--text",
        action="append",
        default=[],
        metavar="PATH=TEXT",
        help="write TEXT and a newline into the volume",
    )
    parser.add_argument(
        "--dir",
        action="append",
        default=[],
        metavar="PATH",
        help="make a directory of the volume",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    extra: list = []
    for pair in args.file:
        extra += ["--file", pair]
    for pair in args.text:
        extra += ["--text", pair]
    for path in args.dir:
        extra += ["--dir", path]

    return build_image(
        args.image,
        args.build,
        args.mkfs,
        args.size_mb,
        args.source_date_epoch,
        args.verbose,
        extra,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
