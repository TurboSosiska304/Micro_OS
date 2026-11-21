#!/usr/bin/env python3
"""Micro OS — the image of the disk of the x86-64 port.

The machine has no programs in its kernel any more: `/bin` and `/etc` are
objects of a MicroFS volume on the disk, and the boot mounts that volume and
runs `/bin/hello` through `execve`. This script calls the host tool that
writes the volume (`tools/mkfs_mfs.c`, which links `components/mfs`), so the
format has one implementation and not a second one in Python.

The image is a whole disk: an MBR, one Linux partition at LBA 2048 and a
MicroFS volume in it, with the fifteen `.mox` images of the build in `/bin`,
an empty `/etc/init.d` and the settings of the machine in `/etc/setting`.

    tools/x86_image.py [--image PATH] [--build DIR] [--mkfs PATH]
                       [--size-mb N] [--verbose]

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
]

# The one file of /etc the smoke test reads. The tool writes the text and a
# newline; the first line is the word `cat`, `grep` and `wc` expect, the two
# after it are the settings a machine of this port has (the config component
# is not linked here, so the file is data of the image).
SETTING = "hello\nhostname=x86_64\nconsole=vga"

IMAGE_MB = 8
START_LBA = 2048

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")


def build_image(
    image: str,
    build_dir: str = DEFAULT_BUILD,
    mkfs: str = DEFAULT_MKFS,
    size_mb: int = IMAGE_MB,
    verbose: bool = False,
) -> int:
    """Write the image with the fifteen programs and /etc/setting."""
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
        "--dir",
        "/bin",
        "--dir",
        "/etc",
        "--dir",
        "/etc/init.d",
    ]

    for name in PROGRAMS:
        path = os.path.join(build_dir, f"{name}.mox")
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2
        command += ["--program", f"/bin/{name}={path}"]

    command += ["--text", f"/etc/setting={SETTING}"]

    os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)

    if verbose:
        print(" ".join(command))

    result = subprocess.run(command, check=False)
    return result.returncode


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk image of the x86-64 port")
    parser.add_argument("--image", default=os.path.join(DEFAULT_BUILD, "programs.img"))
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the .mox images")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool that writes the volume")
    parser.add_argument("--size-mb", type=int, default=IMAGE_MB)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    return build_image(args.image, args.build, args.mkfs, args.size_mb, args.verbose)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
