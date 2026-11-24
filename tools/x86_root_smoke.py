#!/usr/bin/env python3
"""Micro OS — the smoke test of the root of the x86-64 machine.

The other smoke tests of this machine boot one disk: the MicroFS volume of
`make x86-disk` (`x86_smoke.py`) or a copy of it that survives a reset
(`x86_disk_smoke.py`). This one boots **two**: the same tree in a MicroFS
volume and in an ext2 volume (ADR 0027). The kernel reads the type of the
root out of the medium (`root: the root is ext2 on ata0p1`), so the sentence
of the boot and the answers of the shell are what says which filesystem the
machine is running on.

The commands are the same on both roots on purpose:

  * `ls /bin` - the tree the image tool wrote, read through the filesystem;
  * `cat /etc/setting` - a file of the host, and the first line of it;
  * `exec /bin/hello` - a program of the image, entered through execve;
  * `df /` - the volume as the filesystem describes it.

The ext2 image carries a second partition as well: a FAT32 volume the machine
mounts at `/data` (`mount ata0p2 /data fat32`), writes a file to and reads it
back. That is the mount point of ADR 0027 §4 and the exchange format of the
machine; the MicroFS image has one partition and the case is done on the ext2
one, where the table names the second volume.

    python3 tools/x86_root_smoke.py [--image PATH] [--ext2-image PATH]
                                    [--build DIR] [--timeout SECONDS]
                                    [--verbose]

The exit status is 0 when both roots boot and answer, 2 when a tool or an
image is missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import time

from qemu_session import DISKS, disk_name, drive_args, root_name

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The bus of the disk of this run, set by `--disk`, and the name of the second
# partition of it - the FAT32 volume the shell mounts at /data. The commands of
# the run name the device they mount, so the list below is built once the bus
# is known.
DISK = "ide"
DATA_DEVICE = "ata0p2"

# The command that mounts the FAT32 volume of the second partition. The name
# of the device is the one of the bus of the run.
MOUNT_DATA = "mount " + DATA_DEVICE + " /data fat32"
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(DEFAULT_BUILD, "programs.img")
DEFAULT_EXT2_IMAGE = os.path.join(DEFAULT_BUILD, "programs-ext2.img")

PROMPT = "microos> "

# The first line of /etc/setting, written by tools/x86_image.py: the same file
# in both images, because the tree of the two is one list.
SETTING_LINE = "hostname=x86_64"

# The program of the image, and the line it prints from ring 3.
HELLO_LINE = "PROGRAM: hello from /bin"

# The commands of the shared set, in the order they are sent. The answers are
# compared between the two roots where they must agree (the listing of /bin,
# the setting, the program, the name of the root) and checked alone where the
# volume says its own numbers (`df`).
ROOT_COMMANDS = [
    "ls /bin",
    "cat /etc/setting",
    "exec /bin/hello",
    "df /",
    "ls /",
]

# The memory a program maps on the ext2 root: the file /mm.txt is written
# through a shared mapping and `msync`, and it stays on the volume after the
# boot for `e2fsck` and `debugfs` of the host to read.
MMAP_COMMANDS = [
    "exec /bin/mmap",
    "head -n 1 /mm.txt",
]

# The second partition of the ext2 image: the FAT32 volume of /data.
DATA_COMMANDS = [
    MOUNT_DATA,
    "write /data/note the data volume",
    "cat /data/note",
    "df /data",
]

# The programs of /bin every image has to carry: the two ports of the tree
# (zlib and Lua), the ten commands of sbase, the two calculators, zstd, file,
# SQLite and awk. The list is short on purpose - `ls /bin` is compared whole
# between the roots, and this is the handful a person would check by eye.
BIN_NAMES = ("hello", "ls", "cat", "lua", "zcat", "bc", "dc", "sqlite3", "awk", "zstd")

# A line of a listing that is a name and not a message of the shell or of the
# kernel around it (`task: created 'program' (pid 4, prio 5)`).
NAME_LINE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")


def run_machine(image: str, commands: list, timeout: float, build: str) -> str:
    """Boot once on the image, drive the shell and return the transcript."""
    command = [
        "qemu-system-x86_64",
        "-kernel",
        os.path.join(build, "loader32.elf"),
        "-initrd",
        os.path.join(build, "microos64.bin"),
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
        # A raw image on the primary channel, with the cache of the host on
        # purpose: the writes of the guest are a few files of a test and the
        # flush of `sync` still reaches the medium (tools/x86_disk_smoke.py).
        *drive_args(image, DISK),
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    fd = process.stdout.fileno()
    transcript = b""
    sent = 0
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                break

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            # One command per prompt: the shell prints the prompt before it
            # reads, so the line goes out only when the shell waits for it.
            while sent < len(commands) and text.count(PROMPT) > sent:
                process.stdin.write((commands[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            if text.count(PROMPT) > len(commands):
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


def answers(output: str, commands: list) -> list:
    """The answer of every command, split by the prompt of the shell.

    The console of the port ends every line with CR LF (the serial terminal
    of a person), so the carriage returns go before any line of an answer is
    compared with another one.
    """
    blocks = output.split(PROMPT)[1:]
    return [
        blocks[index].replace("\r", "") if index < len(blocks) else ""
        for index in range(len(commands))
    ]


def root_checks(name: str, transcript: str, expect_type: str) -> tuple:
    """The checks of one root, and the answers they are made of."""
    checks = []

    def expect(title: str, condition: bool, detail: str = "") -> None:
        checks.append((title, bool(condition), detail))

    replies = answers(transcript, ROOT_COMMANDS)
    listing, setting, hello, free, tree = replies

    # --- the boot reads the type of the root out of the medium
    expect(
        f"{name}: the root is {expect_type} and the boot says so",
        f"root: the root is {expect_type} on {root_name(DISK)}" in transcript,
        "the line of the root does not name the filesystem of the volume",
    )
    expect(
        f"{name}: the programs come from the root",
        f"x86-64: the programs come from {root_name(DISK)}:/bin" in transcript,
    )
    expect(
        f"{name}: the boot runs the program of /bin through the call",
        "x86-64: running /bin/hello through the call" in transcript,
    )

    # --- ls /bin: the tree of the image, read through the filesystem
    for binary in BIN_NAMES:
        expect(
            f"{name}: /bin/{binary} is a file of the root",
            re.search(rf"(?m)^{re.escape(binary)}$", listing) is not None,
            listing,
        )
    expect(
        f"{name}: the listing of /bin is a directory of the root",
        "no such file" not in listing and listing.strip() != "",
        listing,
    )

    # --- cat /etc/setting: a file the image tool wrote
    expect(f"{name}: /etc/setting is read from the root", SETTING_LINE in setting, setting)

    # --- exec /bin/hello: a program of the volume, entered through the call
    expect(f"{name}: the program of /bin runs", HELLO_LINE in hello, hello)
    expect(f"{name}: the program runs in ring 3", "PROGRAM: cpl 3" in hello, hello)

    # --- df /: the volume as the filesystem describes it
    match = re.search(r"blocks: (\d+) of (\d+) free", free)
    expect(
        f"{name}: df / count the blocks of the volume",
        match is not None and int(match.group(2)) > 0 and int(match.group(1)) <= int(match.group(2)),
        free,
    )

    # --- ls /: the tree of the machine, not an empty root
    for entry in ("bin", "etc"):
        expect(
            f"{name}: /{entry} is a directory of the root",
            re.search(rf"(?m)^{entry}/?$", tree) is not None,
            tree,
        )

    return checks, replies


def data_checks(name: str, transcript: str) -> list:
    """The FAT32 volume of the second partition: mounted, written, read."""
    checks = []

    def expect(title: str, condition: bool, detail: str = "") -> None:
        checks.append((title, bool(condition), detail))

    replies = answers(transcript, ROOT_COMMANDS + DATA_COMMANDS)
    mounted, written, read_back, free = replies[len(ROOT_COMMANDS) :]

    expect(
        f"{name}: the second partition is a FAT32 volume",
        "fat: FAT32 mounted" in mounted
        and f"mount: {disk_name(DISK)}p2 is mounted at /data" in mounted,
        mounted,
    )
    expect(
        f"{name}: /data takes a file",
        "bytes written to /data/note" in written,
        written,
    )
    expect(
        f"{name}: the file of /data comes back",
        "the data volume" in read_back,
        read_back,
    )
    expect(
        f"{name}: df /data counts the volume of the second partition",
        re.search(r"blocks: \d+ of \d+ free", free) is not None,
        free,
    )
    # The block of FAT32 is the sector of 512 bytes, and the line names it: a
    # "0 KB each" would read as a volume of no bytes. FAT keeps no inodes, and
    # the line says so instead of a zero nobody read.
    expect(
        f"{name}: df /data names the unit of the blocks of the volume",
        "(512 bytes each)" in free and "0 KB each" not in free,
        free,
    )
    expect(
        f"{name}: df /data does not invent a table of inodes",
        "not kept by this volume" in free and "0 of 0" not in free,
        free,
    )

    return checks


def extract_partition(image: str, out: str) -> None:
    """Copy the first partition of the disk image into a file of its own.

    e2fsck and debugfs of the host read a filesystem and not a disk, and the
    volume of the machine is the first partition behind an MBR: the entry of
    the table names the first sector and the count of them.
    """
    with open(image, "rb") as disk:
        mbr = disk.read(512)

        if len(mbr) < 512 or mbr[510] != 0x55 or mbr[511] != 0xAA:
            raise OSError("the image has no partition table")

        start = int.from_bytes(mbr[446 + 8 : 446 + 12], "little") * 512
        count = int.from_bytes(mbr[446 + 12 : 446 + 16], "little") * 512

        if count == 0:
            count = os.path.getsize(image) - start

        disk.seek(start)

        with open(out, "wb") as part:
            left = count

            while left > 0:
                chunk = disk.read(min(left, 1 << 20))

                if not chunk:
                    break

                part.write(chunk)
                left -= len(chunk)


def host_tool(name: str) -> bool:
    """Is a tool of the host there? A machine without e2fsprogs skips the two
    checks of the volume and says so, instead of failing the run."""
    return shutil.which(name) is not None


def run_tool(command: list) -> tuple:
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def mapped_file_checks(image: str, transcript: str) -> list:
    """The file a shared mapping wrote, read by the tools of the world.

    The bytes of the mapping are in the file on the volume: `head` of the
    machine reads them, `e2fsck -fn` of the host finds nothing to repair, and
    `debugfs` of the host reads the same file after the boot. It is the gate
    of the plan of 1.6 - a shared mapping that `cat` and `e2fsck` see - on the
    root that is ext2 and not MicroFS.
    """
    checks = []

    def expect(title: str, condition: bool, detail: str = "") -> None:
        checks.append((title, bool(condition), detail))

    expect(
        "ext2: the shared mapping writes the file of the root",
        "mmap: the file says HELLO" in transcript and "HELLO" in transcript.split("head -n 1")[-1],
        transcript[-400:],
    )

    if not host_tool("e2fsck") or not host_tool("debugfs"):
        expect("ext2: the tools of the host are there", False, "e2fsck or debugfs is missing")
        return checks

    partition = image + ".part"

    try:
        extract_partition(image, partition)
    except OSError as error:
        expect("ext2: the volume of the root is a partition", False, str(error))
        return checks

    try:
        status, text = run_tool(["e2fsck", "-fn", partition])
        expect(
            "ext2: e2fsck finds nothing to repair in the volume of the mapping",
            status == 0,
            text[-500:],
        )

        status, text = run_tool(["debugfs", "-R", "cat /mm.txt", partition])
        expect(
            "ext2: debugfs reads the bytes of the shared mapping",
            text.startswith("HELLO"),
            text[:120],
        )
    finally:
        if os.path.exists(partition):
            os.remove(partition)

    return checks


def main(argv: list) -> int:
    global DISK, DATA_DEVICE
    parser = argparse.ArgumentParser(description="the smoke test of the root of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the MicroFS image of the disk")
    parser.add_argument("--ext2-image", default=DEFAULT_EXT2_IMAGE, help="the ext2 image of it")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--timeout", type=float, default=120.0, help="the seconds a boot gets")
    parser.add_argument("--verbose", action="store_true", help="print both consoles")
    parser.add_argument(
        "--disk",
        choices=DISKS,
        default="ide",
        help="the bus of the disk of the run (default: ide)",
    )
    args = parser.parse_args(argv)

    DISK = args.disk
    DATA_DEVICE = disk_name(DISK) + "p2"

    DATA_COMMANDS[0] = f"mount {DATA_DEVICE} /data fat32"

    for path in ("loader32.elf", "microos64.bin"):
        if not os.path.isfile(os.path.join(args.build, path)):
            print(f"missing {args.build}/{path}: run make x86-build", file=sys.stderr)
            return 2

    for path in (args.image, args.ext2_image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    # The boots work on copies: the ext2 root takes the settings files of
    # /etc and the FAT32 volume takes the note of the test, and the image of
    # `make x86-disk` has to stay the one the tool wrote for `make bundle`.
    root_copy = args.build + "/root-smoke-mfs.img"
    ext2_copy = args.build + "/root-smoke-ext2.img"

    host_checks = []

    try:
        shutil.copyfile(args.image, root_copy)
        shutil.copyfile(args.ext2_image, ext2_copy)

        first = run_machine(root_copy, ROOT_COMMANDS, args.timeout, args.build)
        checks, microfs_answers = root_checks("mfs", first, "mfs")

        # The same commands on the second root, the FAT32 volume of its
        # second partition after them, and the memory a program maps of it.
        second = run_machine(
            ext2_copy, ROOT_COMMANDS + DATA_COMMANDS + MMAP_COMMANDS, args.timeout, args.build
        )

        # The volume of the mapping is read by the tools of the host before
        # the copy goes: e2fsck and debugfs are the two readers of a volume
        # that this machine did not write with its own code.
        host_checks = mapped_file_checks(ext2_copy, second)
    except (FileNotFoundError, OSError) as error:
        print(f"root: {error}", file=sys.stderr)
        return 2
    finally:
        for path in (root_copy, ext2_copy):
            if os.path.exists(path):
                os.remove(path)

    ext2_checks, ext2_replies = root_checks("ext2", second, "ext2")
    checks += ext2_checks
    checks += data_checks("ext2", second)
    checks += host_checks

    # --- the two roots are the same machine
    # The listing of /bin is the same tree: the two volumes carry the same
    # files, so the names (and their order, which is the order of the tool)
    # are the same in both listings. The lines of the shell and of the kernel
    # around them are not names of a file.
    def names(listing: str) -> list:
        return [
            line.strip()
            for line in listing.splitlines()
            if NAME_LINE.match(line.strip()) is not None
        ]

    checks.append(
        (
            "the two roots carry the same /bin",
            names(microfs_answers[0]) == names(ext2_replies[0]),
            f"mfs: {names(microfs_answers[0])}\next2: {names(ext2_replies[0])}",
        )
    )
    checks.append(
        (
            "the two roots read the same /etc/setting",
            microfs_answers[1] == ext2_replies[1],
            f"mfs: {microfs_answers[1]}\next2: {ext2_replies[1]}",
        )
    )
    checks.append(
        (
            "the two roots run the same program",
            HELLO_LINE in microfs_answers[2] and HELLO_LINE in ext2_replies[2],
            f"mfs: {microfs_answers[2]}\next2: {ext2_replies[2]}",
        )
    )

    if args.verbose:
        print("--- MicroFS root, the boot and the shell")
        print(first)
        print("--- ext2 root, the boot and the shell")
        print(second)

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the root smoke test of the x86-64 port")
    print("=" * 60)
    for title, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {title}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s)")
        print("RESULT: FAIL")
        return 1

    print(f"OK: {len(checks)} check(s) passed on two roots")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
