#!/usr/bin/env python3
"""Micro OS — the smoke test of the SATA disk of the x86-64 port.

The other smoke tests of this machine boot a disk on the legacy IDE channel
(`make x86-smoke`) or on a card of virtio (`make x86-smoke X86_DISK_BUS=virtio`).
This one boots the same volume on the **AHCI** controller of a chipset (ADR
0041): a machine of these years has no IDE channel at all, and the disk of a
laptop is a port of the SATA controller.

Four boots, and the rule of the lane is the difference between them:

  1. **a volume of this system that `/etc/volumes` names** - the image `make
     x86-disk` builds, on the first port of the controller. The boot finds the
     disk, reads the identify data and the partition table, mounts the MicroFS
     volume of `ahci0p1` read-write (the rule allows it and the root mount
     asked), lists `/bin`, reads `/etc/setting`, runs `/bin/hello` and writes
     `/note.txt`. `/proc/mounts` says `rw`.

  2. **the same image after a reset** - the machine starts again on the file a
     byte-for-byte copy of the first boot left, mounts the volume it finds, and
     `cat /note.txt` reads the line the first boot wrote. `fsck` says the
     volume is consistent.

  3. **a copy without the name** - the same volume, built with `--volumes none`
     so that `/etc/volumes` does not name `ahci0p1`. The root is mounted
     read-only, the write of the shell is refused with the sentence of the
     rule, and the SHA-256 of the whole image is the same before and after the
     boot: not one byte of the medium changed.

  4. **a volume of another system** - a copy of the image with the magic of
     btrfs over the volume and no header of MicroFS left. The mount is refused
     with the name of the foreign system, the boot stops instead of running a
     shell over somebody else's bytes, and the SHA-256 of the image is again
     the same before and after.

The exit status is 0 when every boot is what it has to be, 2 when a tool or an
image is missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

from qemu_session import Machine, MachineError, MachineTimeout, disk_name

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
IMAGE_TOOL = os.path.join(ROOT, "tools", "x86_image.py")

PROMPT = "microos> "

# The device the port prints for the disk of the run, and the first partition
# of it, which is the root of the boot.
DISK = disk_name("ahci")
ROOT_DEVICE = DISK + "p1"

# The line of the program of the image, from ring 3 out of a file of the
# volume. The same line `x86-smoke` reads from the IDE disk.
HELLO_LINE = "PROGRAM: hello from /bin"

# The first line of /etc/setting, written by tools/x86_image.py.
SETTING_LINE = "hostname=x86_64"

# The file the first boot writes and the second one reads: the line exists
# nowhere but on the medium of the run.
NOTE_PATH = "/note.txt"
NOTE_TEXT = "the disk remembers the SATA port"

# The sentences of the mount and of the refusal of the rule.
RW_LINE = "is mounted read-write"
RO_LINE = "is mounted read-only"
REFUSAL_REASON = "the volume is not named in /etc/volumes"

# The magic of btrfs at the start of its superblock (the third copy, at
# 0x10040 of the volume), which is what the image of the fourth boot carries
# instead of the header of MicroFS.
BTRFS_MAGIC_OFFSET = 0x10040
BTRFS_MAGIC = b"_BHRfS_M"

# The header block of MicroFS is the last block of the volume, and its first
# four bytes are the word 0x4D465331 of components/mfs (little endian on the
# medium: "1SFM").
MFS_BLOCK = 1024
MFS_MAGIC = b"1SFM"

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
    header of MicroFS - the identifier of this system - is taken away. What
    is left is a volume no type of the image mounts and the rule refuses.
    """
    start = partition_start(image)
    size = os.path.getsize(image)

    with open(image, "r+b") as handle:
        handle.seek(start + BTRFS_MAGIC_OFFSET)
        handle.write(BTRFS_MAGIC)

        # The last block of the volume: the end of the image, because the
        # partition of the tool runs to it.
        handle.seek(size - MFS_BLOCK)
        if handle.read(len(MFS_MAGIC)) != MFS_MAGIC:
            raise ValueError(f"{image}: the volume has no MicroFS header to replace")
        handle.seek(size - MFS_BLOCK)
        handle.write(b"\x00" * 8)


def build_unnamed(image: str, build: str, mkfs: str) -> None:
    """Write the same tree into a volume whose /etc/volumes names nothing.

    The negative half of the gate is a medium of the same format that no
    mount may write: the image tool with `--volumes none` leaves the file
    out, and the rule of the mount has the sentence the check reads.
    """
    command = [
        sys.executable,
        IMAGE_TOOL,
        "--image",
        image,
        "--build",
        build,
        "--mkfs",
        mkfs,
        "--volumes",
        "none",
    ]

    result = subprocess.run(command, capture_output=True, check=False)

    if result.returncode != 0:
        message = (result.stderr or result.stdout or b"").decode("utf-8", "replace")
        raise RuntimeError(f"the image without a name: {message.strip()}")


def expect(checks: list, title: str, condition: bool, detail: str = "") -> None:
    checks.append((title, bool(condition), detail))


def boot_lines(boot: str) -> list:
    return [line for line in boot.splitlines() if line.strip() != ""]


def boot_writable(machine: Machine, boot: str, checks: list) -> None:
    """The shell of the machine on the volume the rule allows to write."""
    commands = ["ls /bin", "cat /etc/setting", "exec /bin/hello", "df /", "cat /proc/mounts"]
    answers = [machine.run(command, timeout=60.0) for command in commands]

    expect(checks, "the boot names the controller of the chipset",
           re.search(r"ahci0: \d+ port\(s\) of the controller", boot) is not None)
    expect(checks, "the controller reads the identify data of the disk",
           re.search(r"ahci0: \d+ MB on the port \d+", boot) is not None)
    expect(checks, "the disk is the first partition of the table",
           f"x86-64: the root volume is {ROOT_DEVICE}" in boot)
    expect(checks, "the volume of this system is recognised",
           "ahci0: " + ROOT_DEVICE + ": a volume of this system (mfs)" in boot)
    expect(checks, "the controller registers the disk read-only",
           "ahci: 1 disk(s) on the controller, read-only, nothing written" in boot)
    expect(checks, "the root is mounted from the SATA disk",
           f"root: the root is mfs on {ROOT_DEVICE}" in boot)
    expect(checks, "the boot does not fall back to the volume of the memory",
           "root: the root is mfs on ram0" not in boot)
    expect(checks, "the volume is named in /etc/volumes and the writes are open",
           f"root: {ROOT_DEVICE} {RW_LINE}" in boot and
           f"ahci0: the writes of {ROOT_DEVICE} are open" in boot, boot)

    expect(checks, "ls /bin reads the tree of the disk", "hello" in answers[0], answers[0])
    expect(checks, "cat /etc/setting reads a file of the disk", SETTING_LINE in answers[1],
           answers[1])
    expect(checks, "exec /bin/hello runs a file of the SATA disk", HELLO_LINE in answers[2],
           answers[2])
    expect(checks, "df / counts the volume of the disk",
           re.search(r"\d+ KB", answers[3]) is not None, answers[3])
    expect(checks, "/proc/mounts says the root is read-write",
           re.search(rf"{re.escape(ROOT_DEVICE)}\s+/\s+mfs\s+rw", answers[4]) is not None,
           answers[4])

    written = machine.run(f"write {NOTE_PATH} {NOTE_TEXT}", timeout=30.0)
    expect(checks, "the shell writes a file of the volume",
           "bytes written to " + NOTE_PATH in written and "error" not in written.lower(), written)

    read_back = machine.run(f"cat {NOTE_PATH}", timeout=30.0)
    expect(checks, "the file is read back at the first boot", NOTE_TEXT in read_back, read_back)


def boot_after_reset(machine: Machine, boot: str, checks: list) -> None:
    """The second boot of the positive image: the file outlives the machine."""
    expect(checks, "the second boot reaches the shell", PROMPT in boot or boot != "", boot[:200])
    expect(checks, "the second boot mounts the volume it finds",
           f"root: the root is mfs on {ROOT_DEVICE}" in boot)

    read_back = machine.run(f"cat {NOTE_PATH}", timeout=30.0)
    expect(checks, "the file of the first boot survives the reset", NOTE_TEXT in read_back,
           read_back)

    fsck = machine.run("fsck", timeout=120.0)
    expect(checks, "the volume is still consistent after the writes",
           "the volume is consistent" in fsck, fsck)

    mounts = machine.run("cat /proc/mounts", timeout=30.0)
    expect(checks, "the root of the second boot is read-write again",
           re.search(rf"{re.escape(ROOT_DEVICE)}\s+/\s+mfs\s+rw", mounts) is not None, mounts)


def boot_read_only(machine: Machine, boot: str, checks: list) -> None:
    """The volume of this system that /etc/volumes does not name."""
    expect(checks, "the volume is still one of this system",
           f"ahci0: {ROOT_DEVICE}: a volume of this system (mfs)" in boot, boot)
    expect(checks, "the mount asks for the writes and the rule refuses",
           f"root: {ROOT_DEVICE} {RO_LINE}: {REFUSAL_REASON}" in boot, boot)

    mounts = machine.run("cat /proc/mounts", timeout=30.0)
    expect(checks, "/proc/mounts says the root is read-only",
           re.search(rf"{re.escape(ROOT_DEVICE)}\s+/\s+mfs\s+ro", mounts) is not None, mounts)

    refused = machine.run(f"write {NOTE_PATH} {NOTE_TEXT}", timeout=30.0)
    expect(checks, "a write to a volume the rule does not name is refused",
           f"write: {NOTE_PATH}" in refused and "error" in refused.lower(), refused)

    listed = machine.run("ls /", timeout=30.0)
    expect(checks, "the file of the refused write does not exist", "note.txt" not in listed,
           listed)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the AHCI disk of x86-64")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image with /bin of the disk")
    parser.add_argument("--build", default=BUILD, help="the build of the port (loader and kernel)")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool that writes a volume")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(args.build, "loader32.elf")
    kernel = os.path.join(args.build, "microos64.bin")

    for path in (loader, kernel, args.image, args.mkfs):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    # A copy of each image: a test never touches the image of the build, and
    # the SHA-256 of the copy is the proof of what the boot did to it.
    run_image = os.path.join(args.build, "ahci-smoke.img")
    unnamed_image = os.path.join(args.build, "ahci-unnamed.img")
    foreign_image = os.path.join(args.build, "ahci-foreign.img")

    checks: list = []

    try:
        shutil.copyfile(args.image, run_image)
        before = digest(run_image)

        with Machine(args.build, run_image, disk="ahci") as machine:
            try:
                boot = machine.open(timeout=args.timeout)
                boot_writable(machine, boot, checks)
            except (MachineError, MachineTimeout) as error:
                expect(checks, "the machine boots on the SATA disk", False, str(error))

        after_first = digest(run_image)
        if args.verbose:
            print(machine.transcript.decode("utf-8", errors="replace"))
        expect(checks, "the writes of the first boot reached the medium", after_first != before,
               f"before {before}, after {after_first}")

        # The reset: a new machine on the same file.
        with Machine(args.build, run_image, disk="ahci") as machine:
            try:
                boot = machine.open(timeout=args.timeout)
                boot_after_reset(machine, boot, checks)
            except (MachineError, MachineTimeout) as error:
                expect(checks, "the machine boots again on the same disk", False, str(error))

        after_second = digest(run_image)
        if args.verbose:
            print(machine.transcript.decode("utf-8", errors="replace"))
        # The bytes of the image after the second boot are not the bytes of the
        # first: the machine keeps its own log now (`/etc/bootlog.log`,
        # `run.log`), and a boot that writes a log is a boot that changes the
        # volume. The promise of the check is the note of the first boot, so the
        # bytes of the note are what is read.
        with open(run_image, "rb") as image:
            after_second_bytes = image.read()
        expect(checks, "the file of the first boot is in the bytes of the image",
               NOTE_TEXT.encode() in after_second_bytes,
               f"the note of the first boot is not in the image after the second")

        # The third boot: the same tree, and /etc/volumes that names nothing.
        build_unnamed(unnamed_image, args.build, args.mkfs)
        unnamed_before = digest(unnamed_image)

        with Machine(args.build, unnamed_image, disk="ahci") as machine:
            try:
                boot = machine.open(timeout=args.timeout)
                boot_read_only(machine, boot, checks)
            except (MachineError, MachineTimeout) as error:
                expect(checks, "the machine boots on the unnamed volume", False, str(error))

        unnamed_after = digest(unnamed_image)
        if args.verbose:
            print(machine.transcript.decode("utf-8", errors="replace"))
        expect(checks, "the SHA-256 of the unnamed image is the same after the boot",
               unnamed_before == unnamed_after,
               f"before {unnamed_before}, after {unnamed_after}")
        print(f"the unnamed image: sha-256 {unnamed_before}")

        # The fourth boot: the volume of another system, on the same disk.
        shutil.copyfile(args.image, foreign_image)
        make_foreign(foreign_image)
        foreign_before = digest(foreign_image)

        with Machine(args.build, foreign_image, disk="ahci") as machine:
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
               f"ahci0: {ROOT_DEVICE}: the volume of another system (btrfs)" in console, console)
        expect(checks, "the mount of a foreign volume is refused",
               f"root: {ROOT_DEVICE} carries the volume of another system (btrfs)" in console)
        expect(checks, "the machine stops without a root instead of running a shell",
               "x86-64: the boot stops, there is no root filesystem to run from" in console)
        expect(checks, "no shell over the volume of another system", PROMPT not in console.split(
            "there is no root filesystem to run from")[-1])

        foreign_after = digest(foreign_image)
        expect(checks, "the SHA-256 of the foreign image is the same after the boot",
               foreign_before == foreign_after,
               f"before {foreign_before}, after {foreign_after}")
        print(f"the image of the foreign boot: sha-256 {foreign_before}")
    finally:
        for path in (run_image, unnamed_image, foreign_image):
            if os.path.exists(path):
                os.remove(path)

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the smoke test of the SATA disk of x86-64")
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
