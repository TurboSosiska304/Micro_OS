#!/usr/bin/env python3
"""Micro OS — the ESP of a UEFI machine: one FAT volume with the three files.

A machine whose firmware boots Micro OS reads one file from a FAT-formatted
partition - `\\EFI\\BOOT\\BOOTX64.EFI`, the default path of the specification -
and the loader of that file reads the two others from the same volume. This
tool writes that volume:

    EFI/BOOT/BOOTX64.EFI   the loader (ports/port_x86_64/loader/uefi)
    microos64.bin          the kernel, signed (ADR 0020)
    disk.img               the volume of the machine (tools/x86_image.py)

Nothing of the machine is written into the kernel: the disk image is a file of
the ESP, the loader copies it to the memory before the kernel runs, and the
kernel mounts it there. That is why a machine of this kind boots without a
driver for the controller the disk is on (Loader, step 1 of the UEFI lane).

    tools/uefi_esp.py --image build-uefi/esp.img --build build-x86
                      [--size-mb N] [--verbose]
    tools/uefi_esp.py --image build-uefi/esp.img --list

The second form reads the volume back and prints what is in it: the smoke test
of the boot asks it rather than mounting the image.

The tools of the host do the writing: `mkfs.fat` of dosfstools makes the
filesystem and `mmd`/`mcopy` of mtools put the files in it, which is the same
pair the tree uses for the FAT32 volume of the x86-64 image (tools/x86_image.py).
A machine without them is told so and the run stops with 2.

The exit status is 0 when the volume was written (or listed), 2 when a tool or
a file is missing, and 1 when one of them failed.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")

# The three files of the volume, and the name of each in the volume.
LOADER = "BOOTX64.EFI"
KERNEL = "microos64.bin"
DISK = "disk.img"

# The room the files need beyond their own size: the tables of the filesystem,
# the two copies of the FAT, and the cluster the last byte of a file rounds up
# to. 8 MiB is far more than the three of them need and small enough that the
# image is a file a person copies to a USB stick in a second.
SLACK_MB = 8

# The smallest volume the specification calls a FAT32: fewer clusters than this
# and a firmware may read the filesystem as the one of a floppy (and a firmware
# of QEMU does not read it at all).
FAT32_MIN_MB = 48


def tool(name: str) -> str | None:
    return shutil.which(name)


def run(command: list, verbose: bool) -> int:
    if verbose:
        print("+ " + " ".join(command), file=sys.stderr)

    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    if result.returncode != 0 and verbose:
        print(result.stdout, file=sys.stderr)

    return result.returncode


def size_mb(path: str) -> int:
    return (os.path.getsize(path) + (1 << 20) - 1) >> 20


def write(image: str, loader: str, kernel: str, disk: str, total_mb: int, verbose: bool) -> int:
    for name in ("mkfs.fat", "mmd", "mcopy"):
        if tool(name) is None:
            print(f"missing {name}: install dosfstools and mtools", file=sys.stderr)
            return 2

    for path in (loader, kernel, disk):
        if not os.path.isfile(path):
            print(f"missing {path}: build the machine first (make x86-build x86-disk)", file=sys.stderr)
            return 2

    directory = os.path.dirname(os.path.abspath(image))
    os.makedirs(directory, exist_ok=True)

    # The volume is a file of zeros of its size: `mkfs.fat` writes the boot
    # sector, the two tables and the root directory, and `mcopy` the files.
    with open(image, "wb") as file:
        file.truncate(total_mb << 20)

    command = ["mkfs.fat", "-F", "32", "-n", "MICROOS"]

    # A volume of the same contents is the same filesystem: the serial number
    # of the volume is the one dosfstools derives, not the clock of the run.
    if tool("mkfs.fat") is not None:
        command.append("--invariant")

    command.append(image)

    if run(command, verbose) != 0:
        print("mkfs.fat failed", file=sys.stderr)
        return 1

    if run(["mmd", "-i", image, "::/EFI", "::/EFI/BOOT"], verbose) != 0:
        print("mmd failed", file=sys.stderr)
        return 1

    for source, target in (
        (loader, f"::/EFI/BOOT/{LOADER}"),
        (kernel, f"::/{KERNEL}"),
        (disk, f"::/{DISK}"),
    ):
        if run(["mcopy", "-i", image, source, target], verbose) != 0:
            print(f"mcopy {source} failed", file=sys.stderr)
            return 1

    return 0


def listing(image: str) -> int:
    if tool("mdir") is None:
        print("missing mdir: install mtools", file=sys.stderr)
        return 2

    if not os.path.isfile(image):
        print(f"missing {image}", file=sys.stderr)
        return 2

    result = subprocess.run(["mdir", "-i", image, "::/", "::/EFI/BOOT"], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    print(result.stdout, end="")

    return 0 if result.returncode == 0 else 1


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="write the ESP of a UEFI machine")
    parser.add_argument("--image", default=os.path.join(ROOT, "build-uefi", "esp.img"))
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel and its loader")
    parser.add_argument("--loader", default=None, help=f"the EFI application (default: BUILD/{LOADER})")
    parser.add_argument("--kernel", default=None, help=f"the kernel (default: BUILD/{KERNEL})")
    parser.add_argument("--disk", default=None,
                        help="the volume of the machine (default: BUILD/programs.img)")
    parser.add_argument("--size-mb", type=int, default=None,
                        help=f"the size of the volume (default: the files and {SLACK_MB} MB, at least "
                             f"{FAT32_MIN_MB} MB for FAT32)")
    parser.add_argument("--list", action="store_true", help="read the volume back and print it")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.list:
        return listing(args.image)

    loader = args.loader or os.path.join(args.build, LOADER)
    kernel = args.kernel or os.path.join(args.build, KERNEL)
    disk = args.disk or os.path.join(args.build, "programs.img")

    total = args.size_mb

    if total is None:
        needed = size_mb(loader) + size_mb(kernel) + size_mb(disk) + SLACK_MB
        total = max(needed, FAT32_MIN_MB)

    if total < size_mb(disk):
        print(f"--size-mb {total} does not hold the disk image of {size_mb(disk)} MB", file=sys.stderr)
        return 2

    status = write(args.image, loader, kernel, disk, total, args.verbose)

    if status == 0:
        print(f"uefi: {args.image}, {total} MB, {LOADER}, {KERNEL} and {DISK}")

    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
