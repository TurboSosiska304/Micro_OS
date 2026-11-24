#!/usr/bin/env python3
"""Micro OS — the smoke test of the NVMe disk of the x86-64 port.

The other smoke tests of this machine boot the disk on the legacy IDE channel
(`make x86-smoke`), on a card of virtio, or on the SATA controller of a chipset
(`make ahci-smoke`). This one boots the same volume on the **NVMe** controller
of a modern laptop (ADR 0042): a function of the PCI bus with a window of
registers, an admin queue and a pair of I/O rings, and no channels at all.

Two boots, and the second one is the rule of ADR 0041:

  1. **a volume of this system** - the image `make x86-disk` builds, on the
     NVMe namespace 1. The boot finds the controller, enables it, identifies
     the controller and the namespace, creates the I/O queue pair, reads the
     partition table and mounts the MicroFS volume of `nvme0p1`; the shell
     lists `/bin`, reads `/etc/setting` and asks `df /`, and `/bin/hello` runs
     out of a file of the NVMe disk. A write is attempted on purpose and the
     driver refuses it with the sentence of the rule, and the SHA-256 of the
     whole image is the same before and after the boot: not one byte.

  2. **a volume of another system** - a copy of the same image with the magic
     of btrfs over the volume and no header of MicroFS left. The mount is
     refused with the name of the foreign system, the boot stops instead of
     running a shell over somebody else's bytes, and the SHA-256 of the image
     is again the same before and after.

The exit status is 0 when both boots are what they have to be, 2 when a tool
or an image is missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys

from qemu_session import Machine, MachineError, MachineTimeout, disk_name

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")

PROMPT = "microos> "

# The device the port prints for the disk of the run, and the first partition
# of it, which is the root of the boot.
DISK = disk_name("nvme")
ROOT_DEVICE = DISK + "p1"

# The line of the program of the image, from ring 3 out of a file of the
# volume. The same line `x86-smoke` reads from the IDE disk.
HELLO_LINE = "PROGRAM: hello from /bin"

# The first line of /etc/setting, written by tools/x86_image.py.
SETTING_LINE = "hostname=x86_64"

# A file the boot must not be able to write: the volume of a disk this system
# did not format by naming it in /etc/volumes is read-only.
WRITE_PATH = "/nvme.txt"
WRITE_TEXT = "the disk is read-only"

# The sentence of the refusal of the driver.
REFUSAL = "the write of"
REFUSAL_REASON = "the volume is not named in /etc/volumes"

# The magic of btrfs at the start of its superblock (the third copy, at
# 0x10040 of the volume), and the header of MicroFS at the end of it.
BTRFS_MAGIC_OFFSET = 0x10040
BTRFS_MAGIC = b"_BHRfS_M"
MFS_BLOCK = 1024
MFS_MAGIC = b"1SFM"  # the word 0x4D465331 of MicroFS, little endian on the mode

# The MBR of the image: four entries of 16 bytes at 446, and the start of the
# first partition at 8 bytes into an entry, in LBAs of 512 bytes.
MBR_ENTRIES = 446
MBR_START = 8
MBR_LBA = 512


def digest(path: str) -> str:
    """The SHA-256 of a whole file: the proof that not one byte changed."""
    h = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def partition_start(image: str) -> int:
    """The byte offset of the first partition of the MBR of `image`."""
    with open(image, "rb") as handle:
        head = handle.read(MBR_LBA)

    if head[510] != 0x55 or head[511] != 0xAA:
        raise ValueError(f"{image} has no MBR")

    for index in range(4):
        entry = MBR_ENTRIES + index * 16
        start = int.from_bytes(head[entry + MBR_START : entry + MBR_START + 4], "little")
        length = int.from_bytes(head[entry + 12 : entry + 16], "little")

        if start != 0 and length != 0:
            return start * MBR_LBA

    raise ValueError(f"{image}: the MBR names no partition")


def make_foreign(image: str) -> None:
    """Turn the volume of `image` into one of btrfs, in place.

    The magic of btrfs is written where its superblock carries it, and the
    header of MicroFS - the identifier of this system - is taken away, so the
    bytes are a volume of another system and not this one.
    """
    start = partition_start(image)
    size = os.path.getsize(image)

    with open(image, "r+b") as handle:
        handle.seek(start + BTRFS_MAGIC_OFFSET)
        handle.write(BTRFS_MAGIC)

        handle.seek(size - MFS_BLOCK)
        if handle.read(len(MFS_MAGIC)) != MFS_MAGIC:
            raise ValueError(f"{image}: the volume has no MicroFS header to replace")
        handle.seek(size - MFS_BLOCK)
        handle.write(b"\x00" * 8)


def expect(checks: list, title: str, condition: bool, detail: str = "") -> None:
    checks.append((title, bool(condition), detail))


def boot_volume(machine: Machine, boot: str, checks: list) -> None:
    """The shell of the machine on the volume of this system."""
    commands = ["ls /bin", "cat /etc/setting", "exec /bin/hello", "df /"]
    answers = [machine.run(command, timeout=60.0) for command in commands]

    expect(checks, "the boot finds the NVMe controller",
           "nvme0: CAP 0x" in boot and "entries of a queue at most" in boot)
    expect(checks, "the controller and its namespace are identified",
           re.search(r"nvme0: .+ namespace\(s\), NVMe \d+\.\d+\.\d+", boot) is not None, boot)
    expect(checks, "the namespace speaks sectors of 512 bytes",
           "nvme0: 16 MB, 512-byte blocks" in boot)
    expect(checks, "the one I/O queue pair is created",
           "nvme0: 16 MB, 512-byte blocks, 64 entries in the pair of the I/O queues" in boot)
    expect(checks, "the disk is the first partition of the table",
           f"x86-64: the root volume is {ROOT_DEVICE}" in boot)
    expect(checks, "the volume of this system is recognised",
           f"nvme0: {ROOT_DEVICE}: a volume of this system (mfs)" in boot)
    expect(checks, "the disk is read-only and the reason is printed",
           "nvme0: the disk is read-only: " + REFUSAL_REASON in boot)
    expect(checks, "the first boot of the controller writes nothing",
           "nvme: 1 disk(s) on the controller, read-only, nothing written" in boot)
    expect(checks, "the root is mounted from the NVMe disk",
           f"root: the root is mfs on {ROOT_DEVICE}" in boot)
    expect(checks, "the boot does not fall back to the volume of the memory",
           "root: the root is mfs on ram0" not in boot)

    expect(checks, "ls /bin reads the tree of the disk", "hello" in answers[0], answers[0])
    expect(checks, "cat /etc/setting reads a file of the disk", SETTING_LINE in answers[1],
           answers[1])
    expect(checks, "exec /bin/hello runs a file of the NVMe disk", HELLO_LINE in answers[2],
           answers[2])
    expect(checks, "df / counts the volume of the disk",
           re.search(r"\d+ KB", answers[3]) is not None, answers[3])

    # The write of the rule: there is no WRITE command in the driver, and the
    # call of the shell comes back as the error of the medium.
    refused = machine.run(f"write {WRITE_PATH} {WRITE_TEXT}", timeout=30.0)
    expect(checks, "a write to the volume of the disk is refused",
           f"write: {WRITE_PATH}" in refused and "error" in refused.lower(), refused)
    expect(checks, "the driver says why the writes are refused",
           REFUSAL in boot and REFUSAL_REASON in boot)

    listed = machine.run("ls /", timeout=30.0)
    expect(checks, "the file of the refused write does not exist", WRITE_TEXT not in listed,
           listed)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the NVMe disk of x86-64")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image with /bin of the disk")
    parser.add_argument("--build", default=BUILD, help="the build of the port (loader and kernel)")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(args.build, "loader32.elf")
    kernel = os.path.join(args.build, "microos64.bin")

    for path in (loader, kernel, args.image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    # A copy of each image: a test never touches the image of the build, and
    # the SHA-256 of the copy is the proof of what the boot did to it.
    run_image = os.path.join(args.build, "nvme-smoke.img")
    foreign_image = os.path.join(args.build, "nvme-foreign.img")

    checks: list = []

    try:
        shutil.copyfile(args.image, run_image)
        before = digest(run_image)

        with Machine(args.build, run_image, disk="nvme") as machine:
            try:
                boot = machine.open(timeout=args.timeout)
                boot_volume(machine, boot, checks)
            except (MachineError, MachineTimeout) as error:
                expect(checks, "the machine boots on the NVMe disk", False, str(error))

        after = digest(run_image)
        if args.verbose:
            print(machine.transcript.decode("utf-8", errors="replace"))
        expect(checks, "the SHA-256 of the image is the same after the boot", before == after,
               f"before {before}, after {after}")
        print(f"the image of the first boot: sha-256 {before}")

        # The second boot: the volume of another system, on the same disk.
        shutil.copyfile(args.image, foreign_image)
        make_foreign(foreign_image)
        foreign_before = digest(foreign_image)

        with Machine(args.build, foreign_image, disk="nvme") as machine:
            try:
                machine.open(timeout=15.0)
            except (MachineError, MachineTimeout):
                # The boot stops by design: the machine is alive and idle,
                # and the shell never comes. The transcript is the answer.
                pass

            console = machine.transcript.decode("utf-8", errors="replace")

        if args.verbose:
            print(console)

        expect(checks, "the foreign volume is named for what it is",
               f"nvme0: {ROOT_DEVICE}: the volume of another system (btrfs)" in console, console)
        expect(checks, "the mount of a foreign volume is refused",
               f"root: {ROOT_DEVICE} carries the volume of another system (btrfs)" in console)
        expect(checks, "the machine stops without a root instead of running a shell",
               "x86-64: the boot stops, there is no root filesystem to run from" in console)
        expect(checks, "no shell over the volume of another system",
               PROMPT not in console.split("there is no root filesystem to run from")[-1])

        foreign_after = digest(foreign_image)
        expect(checks, "the SHA-256 of the foreign image is the same after the boot",
               foreign_before == foreign_after,
               f"before {foreign_before}, after {foreign_after}")
        print(f"the image of the foreign boot: sha-256 {foreign_before}")
    finally:
        for path in (run_image, foreign_image):
            if os.path.exists(path):
                os.remove(path)

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the smoke test of the NVMe disk of x86-64")
    print("=" * 60)
    for title, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {title}")
        if detail and not ok:
            print(f"          {detail}")
    print("=" * 60)

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        print("RESULT: FAIL")
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
