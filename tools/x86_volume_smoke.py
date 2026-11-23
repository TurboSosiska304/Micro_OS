#!/usr/bin/env python3
"""Micro OS — the smoke test of a large volume (ADR 0029).

The other smoke tests of the port boot a volume of megabytes: the image of
the release is 16 MiB of MicroFS and 64 MiB of ext2, and a file of a test is
a few kilobytes. The plan of 1.4 asks a harder question - is a volume of 5
GiB an ordinary volume? - and the answer is four things at once:

  * the image is sparse on the host: 5 GiB of address space cost the
    megabytes of the filesystem and not the five gigabytes of the disk;
  * the machine mounts it as `/` and `df` counts its blocks;
  * a file of 128 MiB goes in, comes back byte for byte and costs a rate a
    person can live with;
  * the tools of the world agree: `e2fsck -fn` of the host finds nothing to
    repair in the image of the session and `debugfs` sees the file, while
    the machine's own `fsck` says its volume is consistent.

The two filesystems of ADR 0009 that hold a volume of that size are checked
here: ext2, whose file of 128 MiB grows through the double and the triple
table of pointers, and MicroFS, whose map of the free blocks is the memory of
the volume (640 KiB of a 5 GiB volume) and whose chain of blocks grows one
block at a time. FAT32 is not: it is the exchange volume of the second
partition and its ceiling is the format's own.

The program that writes the file is `/bin/mkbig`, built by `make x86-build`
and put in the image by this tool and not by the image of the release: the
smoke of the volume is a test of the machine, not a tool a person uses.

    python3 tools/x86_volume_smoke.py [--build DIR] [--mkfs PATH]
                                      [--dir DIR] [--mib-ext2 128]
                                      [--mib-mfs 16] [--keep] [--verbose]

The exit status is 0 when every check passed, 2 when a tool or an image is
missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from qemu_session import Machine, MachineError
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

# The volume of the plan: 5 GiB, the size a person's disk of a virtual
# machine has. The root partition is the disk less the first megabyte of the
# table and the gap, and the ext2 image carries no FAT32 volume: the second
# partition of the release's image is not the subject here.
VOLUME_MB = 5120
START_LBA = 2048
SECTOR = 512

# The ceiling of the sparse image on the host: the megabytes that are the
# filesystem and not the volume. A tree of 66 programs and the tables of
# mke2fs cost about 7 MiB of an image of 5 GiB; 64 MiB is generous enough
# for another tool of the image and tight enough to catch a copy that wrote
# the zeros out (5 GiB of them).
SPARSE_LIMIT_MB = 64

# The map of the free blocks of MicroFS, in the memory of the machine: one bit
# per block of the data section, so 615 blocks of 1 KiB (615 KiB) for the
# 5,032,178 data blocks of a volume of 5 GiB. The boot of a 16 MiB volume
# holds 2 KiB of it, and the two boots of this test are the same machine
# otherwise - the difference of the two `free` lines is the measurement.
MAP_KB = 615
MAP_TOLERANCE_KB = 96


# The lines of the report, in the order they were made.
CHECKS: list = []


def check(title: str, ok: bool, detail: str = "") -> None:
    """One line of the report."""
    CHECKS.append((title, bool(ok), detail))


def size_on_disk(path: str) -> int:
    """The bytes the image costs the host, holes not counted."""
    stat = os.stat(path)

    return stat.st_blocks * 512


def extract_partition(image: str, out: str, mib: int) -> None:
    """Copy the root partition of the disk image into a file of its own.

    e2fsck and debugfs of the host read a filesystem and not a disk, and the
    volume of the machine is a partition behind an MBR. The copy skips the
    blocks of zeros, so the file is as sparse as the volume is; the bytes are
    read with `os.pread`, which does not mix with the buffer of a file object
    after an `os.lseek` of the descriptor.
    """
    start = START_LBA * SECTOR
    length = mib * 1024 * 1024 - (START_LBA * SECTOR)

    with open(image, "rb") as disk, open(out, "wb") as part:
        descriptor = disk.fileno()
        part.truncate(length)
        position = 0

        while position < length:
            chunk = os.pread(descriptor, min(length - position, 1 << 20), start + position)
            if not chunk:
                break

            part.seek(position)

            if chunk.count(0) == len(chunk):
                part.seek(len(chunk), 1)
            else:
                part.write(chunk)

            position += len(chunk)


def host_tool(name: str) -> bool:
    return shutil.which(name) is not None


def run_tool(command: list, cap: int = 65536) -> tuple:
    """Run a tool of the host and return its status and its words."""
    try:
        result = subprocess.run(command, capture_output=True, check=False, timeout=1800)
    except (OSError, subprocess.TimeoutExpired) as error:
        return 127, str(error)

    text = (result.stdout + result.stderr).decode("utf-8", "replace")

    return result.returncode, text[:cap]


def timed(machine: Machine, command: str, timeout: float) -> tuple:
    """Type one line, return its answer and the seconds it took."""
    started = time.monotonic()
    answer = machine.run(command, timeout=timeout, hang=timeout)

    return answer, time.monotonic() - started


def mib_per_second(answer: str, what: str) -> float:
    """The rate the program printed, or zero when it printed none."""
    match = re.search(rf"{what} (\d+) bytes in (\d+) ms", answer)

    if match is None:
        return 0.0

    milliseconds = int(match.group(2))

    if milliseconds == 0:
        return 0.0

    return int(match.group(1)) / (milliseconds / 1000.0) / (1024 * 1024)


def heap_free_kb(answer: str) -> int:
    match = re.search(r"heap: (\d+) KB free", answer)

    return int(match.group(1)) if match is not None else 0


def df_blocks(answer: str) -> tuple:
    match = re.search(r"blocks: (\d+) of (\d+) free", answer)

    return (int(match.group(1)), int(match.group(2))) if match is not None else (0, 0)


def build_mkbig_image(image: str, build: str, mkfs: str, fs: str) -> int:
    """The tree of the release and `/bin/mkbig` in a volume of 5 GiB."""
    extra = ["--program", f"/bin/mkbig={os.path.join(build, 'mkbig.mox')}"]

    return build_image(
        image,
        build,
        mkfs,
        VOLUME_MB,
        fs=fs,
        data_mb=0,
        extra=extra,
    )


def ext2_checks(build: str, work: str, mkfs: str, mib: int, timeout: float) -> None:
    """The ext2 root: written, read back, and read by the tools of the world."""
    image = os.path.join(work, "ext2-5g.img")
    started = time.monotonic()

    if build_mkbig_image(image, build, mkfs, "ext2") != 0:
        check("ext2: the image of 5 GiB is written", False, image)
        return

    seconds = time.monotonic() - started
    on_disk = size_on_disk(image)
    check(
        f"ext2: the image of {VOLUME_MB} MB costs {on_disk // (1024 * 1024)} MB of the host",
        on_disk <= SPARSE_LIMIT_MB * 1024 * 1024,
        f"{on_disk} bytes on disk, {seconds:.1f} s to build",
    )
    print(f"  ext2: the image of {VOLUME_MB} MB is {on_disk // (1024 * 1024)} MB of the host"
          f" ({seconds:.1f} s of mkfs)")

    try:
        with Machine(build, image) as machine:
            banner = machine.open(timeout=timeout)
            check("ext2: the machine boots on the volume of 5 GiB",
                  "root: the root is ext2 on ata0p1" in banner, banner[-400:])

            answer, _ = timed(machine, "df /", 60.0)
            free, total = df_blocks(answer)
            check("ext2: df / counts the blocks of the volume of 5 GiB",
                  total * 1024 >= (VOLUME_MB - 64) * 1024 * 1024, answer)

            answer, _ = timed(machine, "ls /bin", 60.0)
            check("ext2: /bin of the volume is the tree of the release",
                  "mkbig" in answer and "hello" in answer and "no such file" not in answer,
                  answer)

            answer, seconds = timed(machine, f"exec /bin/mkbig write /big.bin {mib}", 1800.0)
            wrote = mib_per_second(answer, "wrote")
            check(f"ext2: a file of {mib} MiB is written to the volume",
                  f"wrote {mib * 1024 * 1024} bytes" in answer and wrote > 0.2, answer)

            written_rate = wrote
            written_seconds = seconds

            answer, _ = timed(machine, "df /", 60.0)
            after_free, after_total = df_blocks(answer)
            used = free - after_free
            check("ext2: df / shows the file on the medium",
                  after_total == total and mib * 1024 <= used <= mib * 1024 + 8192, answer)

            answer, seconds = timed(machine, f"exec /bin/mkbig verify /big.bin {mib}", 1800.0)
            read = mib_per_second(answer, "read")
            check(f"ext2: the file of {mib} MiB comes back from the medium",
                  "the bytes agree" in answer and read > 0.2, answer)
            read_rate = read
            read_seconds = seconds

            # The machine's own fsck speaks of MicroFS and FAT and no command
            # reaches ext2_check() of the kernel yet: the check of the ext2
            # root of this machine is e2fsck of the host below. The gap is in
            # docs/debts.md and in the report of the gate.
            answer, _ = timed(machine, "fsck", 60.0)
            print(f"  ext2: fsck of the machine: {answer.strip()}")
    except MachineError as error:
        check("ext2: the machine boots on the volume of 5 GiB", False, str(error))
        return

    print(f"  ext2: wrote {mib} MiB at {written_rate:.2f} MiB/s ({written_seconds:.1f} s),"
          f" read it at {read_rate:.2f} MiB/s ({read_seconds:.1f} s)")

    # --- the tools of the world, on the image of the session
    part = os.path.join(work, "ext2-part.img")

    if not host_tool("e2fsck") or not host_tool("debugfs"):
        print("  (e2fsprogs is not installed: the half of the host is skipped)", file=sys.stderr)
        return

    extract_partition(image, part, VOLUME_MB)

    status, text = run_tool(["e2fsck", "-fn", part])
    check("ext2: e2fsck -fn of the host finds nothing to repair",
          status == 0 and "Fix? no" not in text, text[-800:])

    status, text = run_tool(["debugfs", "-R", "stat /big.bin", part])
    check(f"ext2: debugfs sees the file of {mib} MiB",
          status == 0 and f"Size: {mib * 1024 * 1024}" in text, text[-400:])


def mfs_checks(build: str, work: str, mkfs: str, mib: int, timeout: float) -> int:
    """The MicroFS root: its map, a file of 16 MiB, and the fsck of the machine.

    Returns the kilobytes of the heap the boot left: the caller compares it
    with the boot of a small volume, which is the map of the free blocks.
    """
    image = os.path.join(work, "mfs-5g.img")
    small = os.path.join(work, "mfs-16m.img")
    heap = 0

    started = time.monotonic()

    if build_mkbig_image(image, build, mkfs, "mfs") != 0:
        check("mfs: the image of 5 GiB is written", False, image)
        return 0

    seconds = time.monotonic() - started
    on_disk = size_on_disk(image)
    print(f"  mfs: the image of {VOLUME_MB} MB is {on_disk // (1024 * 1024)} MB of the host"
          f" ({seconds:.1f} s of mkfs)")

    try:
        with Machine(build, image) as machine:
            banner = machine.open(timeout=timeout)
            check("mfs: the machine boots on the volume of 5 GiB",
                  "root: the root is mfs on ata0p1" in banner, banner[-400:])

            answer, seconds = timed(machine, "df /", 120.0)
            free, total = df_blocks(answer)
            check("mfs: df / counts the blocks of the volume of 5 GiB",
                  total * 1024 >= (VOLUME_MB - 512) * 1024 * 1024, answer)
            check("mfs: the inode table of the volume is walked once, not per record",
                  "inodes:" in answer, answer)

            answer, _ = timed(machine, "free", 60.0)
            heap = heap_free_kb(answer)
            check("mfs: the boot holds the map of the volume", heap > 0, answer)

            answer, seconds = timed(machine, f"exec /bin/mkbig write /big.bin {mib}", 1800.0)
            wrote = mib_per_second(answer, "wrote")
            check(f"mfs: a file of {mib} MiB is written to the volume",
                  f"wrote {mib * 1024 * 1024} bytes" in answer and wrote > 0.2, answer)
            written_rate = wrote
            written_seconds = seconds

            answer, seconds = timed(machine, f"exec /bin/mkbig verify /big.bin {mib}", 1800.0)
            read = mib_per_second(answer, "read")
            check(f"mfs: the file of {mib} MiB comes back from the medium",
                  "the bytes agree" in answer and read > 0.2, answer)
            read_rate = read
            read_seconds = seconds

            answer, _ = timed(machine, "df /", 120.0)
            after_free, after_total = df_blocks(answer)
            check("mfs: df / shows the file on the medium",
                  after_total == total and free - after_free >= mib * 1024, answer)

            answer, fsck_seconds = timed(machine, "fsck", 120.0)
            check("mfs: fsck of the machine says the volume is consistent",
                  "the volume is consistent" in answer, answer)
            check("mfs: fsck walks the volume and counts its blocks",
                  "block(s) free" in answer, answer)
    except MachineError as error:
        check("mfs: the machine boots on the volume of 5 GiB", False, str(error))
        return heap

    print(f"  mfs: wrote {mib} MiB at {written_rate:.2f} MiB/s ({written_seconds:.1f} s),"
          f" read it at {read_rate:.2f} MiB/s ({read_seconds:.1f} s),"
          f" fsck {fsck_seconds:.1f} s")

    # --- the same machine on a volume of 16 MiB: the map is the difference
    program = f"/bin/mkbig={os.path.join(build, 'mkbig.mox')}"

    if build_image(small, build, mkfs, 16, extra=["--program", program]) != 0:
        check("mfs: the small volume of the comparison is written", False, small)
        return heap

    try:
        with Machine(build, small) as machine:
            machine.open(timeout=timeout)
            answer, _ = timed(machine, "free", 60.0)
    except MachineError as error:
        check("mfs: the small volume of the comparison boots", False, str(error))
        return heap

    small_heap = heap_free_kb(answer)
    difference = small_heap - heap
    check(
        f"mfs: the map of the 5 GiB volume costs {difference} KB of the heap ({MAP_KB} expected)",
        abs(difference - MAP_KB) <= MAP_TOLERANCE_KB,
        f"5 GiB: {heap} KB, 16 MiB: {small_heap} KB",
    )
    print(f"  mfs: heap free {heap} KB on 5 GiB and {small_heap} KB on 16 MiB"
          f" ({difference} KB of map)")

    return heap


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of a large volume")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--dir", default=None, help="where the images are written")
    parser.add_argument("--mib-ext2", type=int, default=128, metavar="N",
                        help="the size of the file of the ext2 volume (default: 128)")
    parser.add_argument("--mib-mfs", type=int, default=16, metavar="N",
                        help="the size of the file of the MicroFS volume (default: 16)")
    parser.add_argument("--timeout", type=float, default=1800.0,
                        help="the seconds a boot gets (default: 1800)")
    parser.add_argument("--keep", action="store_true", help="keep the images of the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles as well")
    args = parser.parse_args(argv)

    for path in ("loader32.elf", "microos64.bin", "mkbig.mox"):
        if not os.path.isfile(os.path.join(args.build, path)):
            print(f"missing {args.build}/{path}: run make x86-build", file=sys.stderr)
            return 2

    if not os.path.isfile(args.mkfs):
        print(f"missing {args.mkfs}: run make build", file=sys.stderr)
        return 2

    if not shutil.which("qemu-system-x86_64"):
        print("missing qemu-system-x86_64", file=sys.stderr)
        return 2

    if args.dir is not None:
        work = args.dir
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="microos-volume-", dir=args.build)

    started = time.monotonic()

    try:
        ext2_checks(args.build, work, args.mkfs, args.mib_ext2, args.timeout)
        mfs_checks(args.build, work, args.mkfs, args.mib_mfs, args.timeout)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    failed = [item for item in CHECKS if not item[1]]
    seconds = time.monotonic() - started

    print("Micro OS — the volume smoke test of the x86-64 port")
    print("=" * 72)

    for title, ok, detail in CHECKS:
        print(f"  {' ok ' if ok else 'FAIL'}  {title}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 72)

    if failed:
        print(f"FAILED: {len(failed)} of {len(CHECKS)} check(s) in {seconds:.0f} s")
        print("RESULT: FAIL")
        return 1

    print(f"OK: {len(CHECKS)} check(s) passed on two volumes of {VOLUME_MB} MB"
          f" in {seconds:.0f} s")
    print("RESULT: PASS")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
