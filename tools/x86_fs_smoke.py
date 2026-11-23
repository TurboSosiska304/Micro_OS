#!/usr/bin/env python3
"""Micro OS — the smoke test of the filesystem of the x86-64 port.

The other smokes prove that a file survives a reset and that the two roots
answer the same. This one proves the three things the filesystem gained and
that no unit test can prove on the machine:

  1. **rename and rmdir through a program.** `/bin/mv` and `/bin/rmdir` are
     the two calls of POSIX; a name moves, a directory that still holds a name
     refuses to go, and an empty one goes.
  2. **`fsck ext2` on a broken image.** The free counter of the superblock is
     made wrong in the image before the boot; the machine says what is wrong,
     `-y` repairs it, and e2fsck of the host accepts the medium afterwards.
  3. **the journal of SQLite is a file.** A transaction is interrupted by a
     reset, the journal is still on the volume at the next boot, and the rows
     of the committed state are read after SQLite rolls it back.

The three runs use their own copies of the images of `make x86-disk`; a
failure never leaves a half-written volume for the next run.

    python3 tools/x86_fs_smoke.py [--image PATH] [--ext2-image PATH]
                                  [--timeout SECONDS] [--keep] [--verbose]
"""

from __future__ import annotations

import argparse
import os
import select
import shutil
import struct
import subprocess
import sys
import time

PROMPT = "microos> "
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
BASE_IMAGE = os.path.join(BUILD, "programs.img")
BASE_EXT2_IMAGE = os.path.join(BUILD, "programs-ext2.img")

# The root partition of the ext2 image: the table in LBA 0 and the volume
# from LBA 2048 (tools/x86_image.py, EXT2_START_LBA).
EXT2_START_LBA = 2048
EXT2_SUPERBLOCK_OFFSET = 1024
EXT2_FREE_BLOCKS_OFFSET = 12

RENAME_COMMANDS = [
    "mkdir /fsdir",
    "write /fsdir/old.txt hello",
    "exec /bin/mv /fsdir/old.txt /fsdir/new.txt",
    "cat /fsdir/new.txt",
    "exec /bin/rmdir /fsdir",
    "exec /bin/rm /fsdir/new.txt",
    "exec /bin/rmdir /fsdir",
    "ls /",
]

EXT2_COMMANDS = [
    "fsck ext2 /",
    "fsck -y ext2 /",
    "fsck ext2 /",
]

SQL_SETUP_COMMANDS = [
    "exec /bin/sqlite3 /fs.db < /etc/sql-setup.sql",
    "ls /",
]

SQL_TXN_COMMANDS = [
    "exec /bin/sqlite3 /fs.db < /etc/sql-txn.sql",
]

SQL_AFTER_COMMANDS = [
    "ls /",
    "exec /bin/sqlite3 /fs.db < /etc/sql-after.sql",
    "ls /",
]


def machine_command(image: str) -> list:
    return [
        "qemu-system-x86_64",
        "-kernel",
        os.path.join(BUILD, "loader32.elf"),
        "-initrd",
        os.path.join(BUILD, "microos64.bin"),
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writeback",
    ]


def run_machine(image: str, commands: list, timeout: float, stop_marker: str | None = None):
    """Boot once on the image, drive the shell and return (transcript, cut).

    `cut` is true when the machine was taken down at `stop_marker`: the run
    after it is the reset the journal of SQLite has to survive.
    """
    process = subprocess.Popen(
        machine_command(image),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    fd = process.stdout.fileno()
    transcript = b""
    sent = 0
    cut = False
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

            if stop_marker is not None and stop_marker in text:
                # The transaction is on the volume and has not committed: the
                # process is killed, which is the reset a power cut gives the
                # journal. The rest of the script never runs.
                process.kill()
                cut = True
                break

            while sent < len(commands) and text.count(PROMPT) > sent:
                process.stdin.write((commands[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            if text.count(PROMPT) > len(commands):
                break
    finally:
        if process.poll() is None:
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

    # The console writes the pair a terminal expects; the checks read lines,
    # so the carriage return is folded away here and not in each of them.
    return transcript.decode("utf-8", errors="replace").replace("\r\n", "\n"), cut


def answers(transcript: str) -> list:
    """The answer of every command: the text after each prompt.

    The shell prints the prompt before it reads, so the part after the n-th
    prompt is the answer to the n-th command, with the echo of the line the
    console wrote in front of it.
    """
    return transcript.split(PROMPT)[1:]


def copy_image(source: str, target: str) -> None:
    if not os.path.isfile(source):
        raise FileNotFoundError(f"missing {source}: run make x86-disk")
    shutil.copyfile(source, target)


def corrupt_ext2_counter(image: str, value: int = 1) -> None:
    """Write a wrong s_free_blocks_count into the superblock of the volume.

    The mount reads the field and keeps it; nothing before `fsck ext2` puts it
    right, so the machine sees a volume whose counter lies.
    """
    offset = EXT2_START_LBA * 512 + EXT2_SUPERBLOCK_OFFSET + EXT2_FREE_BLOCKS_OFFSET

    with open(image, "r+b") as disk:
        disk.seek(offset)
        disk.write(struct.pack("<I", value))


def partition_from_image(image: str, target: str) -> None:
    """Copy the first Linux partition of the image into its own file."""
    with open(image, "rb") as disk:
        table = disk.read(512)
        entry = None

        for index in range(4):
            candidate = table[446 + index * 16 : 446 + (index + 1) * 16]
            if candidate[4] == 0x83:
                entry = candidate
                break

        if entry is None:
            raise ValueError(f"{image} has no Linux partition")

        lba = int.from_bytes(entry[8:12], "little")
        sectors = int.from_bytes(entry[12:16], "little")
        disk.seek(lba * 512)

        with open(target, "wb") as part:
            left = sectors * 512
            while left > 0:
                chunk = disk.read(min(1024 * 1024, left))
                if not chunk:
                    raise ValueError(f"{image}: the partition ends early")
                part.write(chunk)
                left -= len(chunk)


def check_e2fsck(path: str):
    result = subprocess.run(["e2fsck", "-fn", path], capture_output=True, text=True, check=False)
    output = (result.stdout or "") + (result.stderr or "")
    return result.returncode == 0 and "Fix? no" not in output, output


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the filesystem smoke of the x86-64 port")
    parser.add_argument("--image", default=BASE_IMAGE, help="the MicroFS image of the build")
    parser.add_argument("--ext2-image", default=BASE_EXT2_IMAGE, help="the ext2 image of the build")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds one boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the images of the run")
    parser.add_argument("--verbose", action="store_true", help="print every transcript")
    args = parser.parse_args(argv)

    mfs_image = os.path.join(BUILD, "fs-smoke-mfs.img")
    ext2_image = os.path.join(BUILD, "fs-smoke-ext2.img")
    part_image = os.path.join(BUILD, "fs-smoke-ext2-part.img")

    for path in (os.path.join(BUILD, "loader32.elf"), os.path.join(BUILD, "microos64.bin")):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    checks = []
    transcripts = {}

    try:
        # --- 1. rename and rmdir through the programs of /bin
        copy_image(args.image, mfs_image)
        rename, _ = run_machine(mfs_image, RENAME_COMMANDS, args.timeout)
        transcripts["rename"] = rename
        rename_answers = answers(rename)
        checks += [
            ("mv moves the name", len(rename_answers) > 3 and "hello" in rename_answers[3]),
            ("rmdir refuses a directory that holds a name",
             len(rename_answers) > 4 and "error 39" in rename_answers[4]),
            ("rmdir removes the empty directory",
             len(rename_answers) > 6 and "error" not in rename_answers[6]),
            ("the directory is gone from the root",
             len(rename_answers) > 7 and "fsdir" not in rename_answers[7]),
        ]

        # --- 2. fsck ext2 on a broken image
        copy_image(args.ext2_image, ext2_image)
        corrupt_ext2_counter(ext2_image)
        fsck_out, _ = run_machine(ext2_image, EXT2_COMMANDS, args.timeout)
        transcripts["fsck"] = fsck_out
        checks += [
            ("fsck ext2 names the wrong counter", "problem(s), the first:" in fsck_out),
            ("fsck ext2 -y repairs the counter", "repair(s) applied" in fsck_out),
            ("fsck ext2 proves the volume after the repair",
             "0 problem(s) after the repair" in fsck_out and "the volume is consistent" in fsck_out),
        ]

        partition_from_image(ext2_image, part_image)
        clean, e2out = check_e2fsck(part_image)
        checks.append(("e2fsck accepts the repaired ext2 volume", clean))
        if not clean:
            transcripts["e2fsck"] = e2out

        # --- 3. a journal on the file, rolled back after a reset
        copy_image(args.image, mfs_image)
        setup, _ = run_machine(mfs_image, SQL_SETUP_COMMANDS, args.timeout)
        transcripts["sql-setup"] = setup
        setup_answers = answers(setup)
        checks.append(("the database of the first boot is made",
                       len(setup_answers) > 0 and "ok" in setup_answers[0]))

        cut, was_cut = run_machine(mfs_image, SQL_TXN_COMMANDS, args.timeout,
                                   stop_marker="MIDTRANSACTION")
        transcripts["sql-txn"] = cut
        checks.append(("the transaction reached its journal before the reset", was_cut))

        after, _ = run_machine(mfs_image, SQL_AFTER_COMMANDS, args.timeout)
        transcripts["sql-after"] = after
        after_answers = answers(after)
        sql = after_answers[1] if len(after_answers) > 1 else ""
        checks += [
            ("the journal survived the reset",
             len(after_answers) > 0 and "fs.db-journal" in after_answers[0]),
            ("the reset rolled the transaction back", "1.5" in sql and "\n2\n" in "\n" + sql),
            ("the row of the interrupted transaction is gone", "\n0\n" in "\n" + sql),
            ("SQLite finds the database sound after the rollback", "ok" in sql),
        ]
    except FileNotFoundError as error:
        print(f"fs: {error}", file=sys.stderr)
        return 2
    finally:
        if not args.keep:
            for path in (mfs_image, ext2_image, part_image):
                if os.path.exists(path):
                    os.remove(path)

    if args.verbose:
        for name, text in transcripts.items():
            print(f"--- {name} ---")
            print(text)

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the filesystem smoke of the x86-64 port")
    print("=" * 60)
    for name, ok in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
    print("=" * 60)

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        for name in ("rename", "fsck", "sql-txn", "sql-after", "e2fsck"):
            if name in transcripts:
                print(f"--- {name} ---")
                print(transcripts[name])
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
