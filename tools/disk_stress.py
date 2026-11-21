#!/usr/bin/env python3
"""Micro OS — a disk that is filled, written, repaired and booted again.

The smoke tests of the disk check one file across one reset. This is the
harder question of the gate of 1.0: after a volume has been filled to
`ENOSPC`, written, appended, taken down with `rm -r`, repaired by `fsck -y`
and booted a second time, is it either still the volume it was or a volume
the repair made whole? The answer has to be about the medium and not about
the process that wrote it.

Two filesystems are checked:

  * MicroFS, the root of the x86-64 image. The volume is filled by
    tools/soak_image.py (one `/filler` file) so that a write is refused by
    the filesystem itself; the repair and the second boot follow.
  * FAT, in the second partition of the same disk. The image is built with
    `MICRO_OS_FS_FAT32=ON`, `mkfs.vfat` writes the FAT16 volume and
    `mcopy` puts the filler in it. `--fs fat` and `--fs both` need
    dosfstools and mtools; when they are not installed the half is reported
    as skipped, not failed.

    tools/disk_stress.py [--fs mfs|fat|both] [--build DIR] [--fat-build DIR]
                         [--image PATH] [--keep] [--verbose]

The command runs `fsck` after the stress, and `fsck -y` before it; a volume
that reports a problem after the repair is a failure, a volume that says it
is consistent both times is a pass.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time

from qemu_session import Machine, MachineClosed, MachineError, MachineTimeout
from soak_image import build_full_image
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_FAT_BUILD = os.path.join(ROOT, "build-x86-fat")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
FAT_MB = 16
FAT_FILLER_TRIES = 4
SECTOR = 512


class Check:
    """One line typed into the shell and the words its answer must hold."""

    __slots__ = ("command", "expect", "forbid")

    def __init__(self, command: str, expect=(), forbid=()) -> None:
        self.command = command
        self.expect = list(expect)
        self.forbid = list(forbid)


def consistent(answer: str) -> bool:
    """The two answers a sound volume gives after a check or a repair."""
    if "the volume is consistent" in answer:
        return True
    if "0 problem(s) after the repair" in answer:
        return True
    return False


def run_steps(machine: Machine, steps: list, timeout: float, verbose: bool) -> list:
    """Type every line and return the checks of the run."""
    checks = []

    for step in steps:
        started = time.monotonic()
        try:
            answer = machine.run(step.command, timeout=timeout)
        except (MachineClosed, MachineTimeout) as error:
            checks.append((step.command, False, str(error), time.monotonic() - started))
            break

        ok = True
        detail = ""

        for expectation in step.expect:
            if isinstance(expectation, re.Pattern):
                if expectation.search(answer) is None:
                    ok = False
                    detail = f"no {expectation.pattern!r}"
                    break
            elif expectation not in answer:
                ok = False
                detail = f"no {expectation!r}"
                break

        if ok:
            for forbidden in step.forbid:
                if isinstance(forbidden, re.Pattern):
                    if forbidden.search(answer) is not None:
                        ok = False
                        detail = f"the answer holds {forbidden.pattern!r}"
                        break
                elif forbidden in answer:
                    ok = False
                    detail = f"the answer holds {forbidden!r}"
                    break

        if ok and step.command.startswith(("fsck", "fsck ")) and not consistent(answer):
            ok = False
            detail = "the volume is not consistent"

        if verbose or not ok:
            print(f"    {'ok ' if ok else 'FAIL'} {step.command}")
            if not ok and detail:
                print(f"         {detail}; the answer: {answer[-160:]!r}")

        checks.append((step.command, ok, detail, time.monotonic() - started))

    return checks


def mfs_steps() -> tuple:
    """The two boots of the MicroFS stress."""
    first = [
        Check("df", [re.compile(r"blocks:\s*0\s+of")]),
        Check("write /tmp/full hello", ["no space left on device"]),
        Check("df", [re.compile(r"blocks:\s*0\s+of")]),
        Check("rm /filler", forbid=("rm:",)),
        Check("fsck -y", ["consistent"]),
        Check("write /tmp/a hello", ["byte(s) written"]),
        Check("echo more >> /tmp/a", forbid=("cannot", "No space")),
        Check("cat /tmp/a", ["hello", "more"]),
        Check("mkdir /tmp/d", forbid=("mkdir:",)),
        Check("write /tmp/d/b body", ["byte(s) written"]),
        Check("cat /tmp/d/b", ["body"]),
        Check("rm -r /tmp/d", forbid=("rm:",)),
        Check("write /persist hello-disk", ["byte(s) written"]),
        Check("df", [re.compile(r"blocks:\s*[1-9]\d*\s+of")]),
        Check("fsck", ["consistent"]),
        Check("exec /bin/hello", ["PROGRAM: hello from /bin"]),
    ]

    second = [
        Check("df", [re.compile(r"blocks:\s*[1-9]\d*\s+of")]),
        Check("fsck", ["consistent"]),
        Check("cat /persist", ["hello-disk"]),
        Check("cat /etc/setting", ["hello"]),
        Check("exec /bin/hello", ["PROGRAM: hello from /bin", "the program exited with 0"]),
        Check("ls /bin", ["hello", "wc"]),
        Check("mkdir /tmp/d", forbid=("mkdir:",)),
        Check("rm -r /tmp/d", forbid=("rm:",)),
        Check("fsck -y", ["consistent"]),
    ]

    return first, second


def fat_steps() -> tuple:
    """The two boots of the FAT stress; the volume is already mounted."""
    first = [
        Check("mount ata0p2 /mnt fat32", ["is mounted"]),
        Check("df /mnt", [re.compile(r"blocks:\s*0\s+of")]),
        Check("write /mnt/full hello", ["no space left on device"]),
        Check("rm /mnt/filler", forbid=("rm:",)),
        Check("fsck fat /mnt -y", ["consistent"]),
        Check("write /mnt/a hello", ["byte(s) written"]),
        Check("cat /mnt/a", ["hello"]),
        Check("echo more >> /mnt/a", forbid=("cannot", "No space")),
        Check("cat /mnt/a", ["hello", "more"]),
        Check("mkdir /mnt/d", forbid=("mkdir:",)),
        Check("write /mnt/d/b body", ["byte(s) written"]),
        Check("cat /mnt/d/b", ["body"]),
        Check("rm /mnt/d/b", forbid=("rm:",)),
        Check("rm -r /mnt/d", forbid=("rm:",)),
        Check("write /mnt/keep survives", ["byte(s) written"]),
        Check("rm /mnt/a", forbid=("rm:",)),
        Check("fsck fat /mnt", ["consistent"]),
        Check("umount /mnt", ["is out of the tree"]),
    ]

    second = [
        Check("mount ata0p2 /mnt fat32", ["is mounted"]),
        Check("df /mnt", [re.compile(r"blocks:\s*[1-9]\d*\s+of")]),
        Check("fsck fat /mnt -y", ["consistent"]),
        Check("cat /mnt/keep", ["survives"]),
        Check("mkdir /mnt/d", forbid=("mkdir:",)),
        Check("rm -r /mnt/d", forbid=("rm:",)),
        Check("fsck fat /mnt", ["consistent"]),
        Check("umount /mnt", ["is out of the tree"]),
    ]

    return first, second


def boot_session(build: str, image: str, label: str, steps: list, timeout: float,
                 verbose: bool) -> list:
    print(f"  {label}: {len(steps)} step(s)")
    with Machine(build, image) as machine:
        machine.open(timeout)
        checks = run_steps(machine, steps, timeout, verbose)

    return checks


def load_mbr(image: str) -> tuple:
    with open(image, "rb") as handle:
        mbr = handle.read(SECTOR)
    start, count = struct.unpack_from("<II", mbr, 446 + 8)
    return start, count


def align_up(value: int, unit: int) -> int:
    return (value + unit - 1) // unit * unit


def make_fat_image(path: str, filler: int | None) -> bool:
    """A FAT16 volume with an optional filler file (mtools writes it)."""
    size_kb = FAT_MB * 1024

    for tool in ("mkfs.vfat", "mcopy"):
        if shutil.which(tool) is None:
            print(f"  FAT: skip ({tool} is not installed)")
            return False

    if os.path.exists(path):
        os.remove(path)

    result = subprocess.run(
        ["mkfs.vfat", "-F", "16", "-n", "SOAK", "-C", path, str(size_kb)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        print(f"  FAT: mkfs.vfat failed: {result.stderr.strip()}")
        return False

    if filler is None:
        return True

    source = path + ".filler"
    with open(source, "wb") as handle:
        handle.truncate(filler)

    result = subprocess.run(
        ["mcopy", "-i", path, source, "::/filler"],
        capture_output=True,
        text=True,
        check=False,
    )
    os.remove(source)

    if result.returncode != 0:
        print(f"  FAT: the filler of {filler} byte(s) did not fit: {result.stderr.strip()}")
        return False

    return True


def embed_fat(disk: str, base: str, fat: str) -> tuple:
    """Copy `base` to `disk`, add a second MBR entry and the FAT bytes."""
    shutil.copyfile(base, disk)
    start1, count1 = load_mbr(base)
    start2 = align_up(start1 + count1, 2048)
    fat_bytes = os.path.getsize(fat)
    fat_sectors = (fat_bytes + SECTOR - 1) // SECTOR

    with open(disk, "r+b") as handle:
        mbr = bytearray(handle.read(SECTOR))
        entry = bytearray(16)
        entry[0] = 0x80
        entry[1:4] = b"\xfe\xff\xff"
        entry[4] = 0x06  # FAT16 LBA
        entry[5:8] = b"\xfe\xff\xff"
        struct.pack_into("<II", entry, 8, start2, fat_sectors)
        mbr[446 + 16 : 446 + 32] = entry
        handle.seek(0)
        handle.write(mbr)
        handle.seek(start2 * SECTOR)
        with open(fat, "rb") as source:
            shutil.copyfileobj(source, handle)
        handle.truncate((start2 + fat_sectors) * SECTOR)

    return start2, count1, fat_sectors


def fat_free_clusters(build: str, image: str, timeout: float) -> tuple:
    """Boot, mount the second partition and read its free clusters."""
    with Machine(build, image) as machine:
        machine.open(timeout)
        machine.run("mount ata0p2 /mnt fat32", timeout=timeout)
        answer = machine.run("df /mnt", timeout=timeout)

    match = re.search(r"blocks:\s*(\d+)\s+of\s+(\d+)\s+free\s*\((\d+)\s*KB each\)", answer)
    if match is None:
        return 0, 0

    return int(match.group(1)), int(match.group(3)) * 1024


def build_fat_disk(args) -> tuple:
    """The root MicroFS of the FAT build and a FAT16 second partition.

    The first boot discovers how much the FAT volume holds; the filler is
    then sized to leave no cluster free, and a second boot verifies it. The
    number is of the medium and not of a constant of this script.
    """
    base = args.fat_image + ".base"
    fat = args.fat_image + ".fat"

    extra = ["--dir", "/tmp"]
    if build_image(base, build_dir=args.fat_build, mkfs=args.mkfs, size_mb=8,
                   extra=extra) != 0:
        return None

    if not make_fat_image(fat, None):
        return None

    embed_fat(args.fat_image, base, fat)
    free, cluster = fat_free_clusters(args.fat_build, args.fat_image, args.timeout)

    if free == 0 or cluster == 0:
        print("  FAT: the empty volume did not answer df")
        return None

    filler = 0
    for attempt in range(FAT_FILLER_TRIES):
        if free <= 1:
            break
        # Leave the file table its own room: the root directory is already
        # outside the free clusters, so a filler of every free cluster fits.
        filler = free * cluster
        if not make_fat_image(fat, filler):
            free -= 1
            continue

        embed_fat(args.fat_image, base, fat)
        left, cluster = fat_free_clusters(args.fat_build, args.fat_image, args.timeout)

        if args.verbose:
            print(f"  FAT: attempt {attempt + 1}, filler {filler}, free {left}")

        if left == 0:
            return filler
        free = left

    print("  FAT: the filler did not reach zero free clusters")
    return None


def run_mfs(args) -> list:
    print(f"MFS: {args.build}")
    image = args.image
    filler = build_full_image(args.build, args.mkfs, image, timeout=args.timeout,
                              verbose=args.verbose)
    if not filler:
        return [("<build>", False, "the full volume could not be built", 0.0)]

    work = image + ".run"
    shutil.copyfile(image, work)
    first, second = mfs_steps()

    checks = boot_session(args.build, work, "boot 1", first, args.timeout, args.verbose)
    if all(item[1] for item in checks):
        checks += boot_session(args.build, work, "boot 2", second, args.timeout, args.verbose)
    else:
        checks.append(("boot 2", False, "boot 1 did not pass", 0.0))

    if not args.keep:
        os.remove(work)
    else:
        print(f"  MFS: the image of the run is {work}")

    return checks


def run_fat(args) -> list:
    if not os.path.isfile(os.path.join(args.fat_build, "microos64.bin")):
        print(f"FAT: skip (no {args.fat_build}; run make x86-fat-build)")
        return []

    print(f"FAT: {args.fat_build}")
    filler = build_fat_disk(args)
    if filler is None:
        return [("<build>", False, "the full FAT volume could not be built", 0.0)]

    work = args.fat_image + ".run"
    shutil.copyfile(args.fat_image, work)
    first, second = fat_steps()

    checks = boot_session(args.fat_build, work, "boot 1", first, args.timeout, args.verbose)
    if all(item[1] for item in checks):
        checks += boot_session(args.fat_build, work, "boot 2", second, args.timeout,
                               args.verbose)
    else:
        checks.append(("boot 2", False, "boot 1 did not pass", 0.0))

    if not args.keep:
        os.remove(work)
        for temporary in (args.fat_image + ".base", args.fat_image + ".fat",
                          args.fat_image):
            if os.path.exists(temporary):
                os.remove(temporary)
    else:
        print(f"  FAT: the image of the run is {work}")

    return checks


def report(checks: list, args) -> int:
    bad = [item for item in checks if not item[1]]
    print("=" * 68)

    if bad:
        print(f"FAILED: {len(bad)} of {len(checks)} check(s)")
        for command, ok, detail, seconds in bad[:8]:
            del ok, seconds
            print(f"  {command}: {detail}")
        print("RESULT: FAIL")
        return 1

    if not checks:
        if args.require_fat and args.fs in ("fat", "both"):
            print("RESULT: FAIL (--require-fat and the FAT half was skipped)")
            return 1
        print("RESULT: SKIP (nothing was checked: the tools of the volume are missing)")
        return 0

    print(f"OK: {len(checks)} check(s), the volume is whole after the stress")
    print("RESULT: PASS")
    return 0


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk stress of the x86-64 port")
    parser.add_argument("--fs", choices=("mfs", "fat", "both"), default="mfs")
    parser.add_argument("--build", default=DEFAULT_BUILD)
    parser.add_argument("--fat-build", default=DEFAULT_FAT_BUILD)
    parser.add_argument("--mkfs", default=DEFAULT_MKFS)
    parser.add_argument("--image", default=None, help="the image of the run")
    parser.add_argument("--fat-image", default=None)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--keep", action="store_true", help="keep the images of the run")
    parser.add_argument("--require-fat", action="store_true",
                        help="fail when the FAT half cannot be checked")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)
    args.image = args.image or os.path.join(args.build, "disk-stress.img")
    args.fat_image = args.fat_image or os.path.join(args.fat_build, "fat-stress.img")

    for path in (os.path.join(args.build, "loader32.elf"),
                 os.path.join(args.build, "microos64.bin"),
                 args.mkfs):
        if not os.path.isfile(path):
            print(f"disk-stress: missing {path}: run make x86-disk build", file=sys.stderr)
            return 2

    checks: list = []

    try:
        if args.fs in ("mfs", "both"):
            checks += run_mfs(args)
        if args.fs in ("fat", "both"):
            checks += run_fat(args)
    except (MachineError, OSError) as error:
        print(f"disk-stress: {error}", file=sys.stderr)
        return 2

    return report(checks, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
