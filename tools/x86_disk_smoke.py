#!/usr/bin/env python3
"""Micro OS — the smoke test of the disk of the x86-64 port.

The other smoke test of this machine (`x86_smoke.py`) boots on a copy of the
image `make x86-disk` builds: a volume of MicroFS with /bin and /etc in it.
This one checks the thing that no line of a boot can prove: a file written by
one boot is read by the next one, out of the same 8 MB of the image, and the
programs of /bin are still there and still run.

    python3 tools/x86_disk_smoke.py [--image PATH] [--timeout SECONDS]
                                    [--repeat N] [--keep] [--verbose]

The image is a copy of the one build-x86/programs.img (`make x86-disk`) and
removed after the run, unless --keep says otherwise. `--mkimage PATH` only
writes the copy and stops.

Two boots of the same machine, the same image:

  1. the boot finds "ata0", reads the MBR, registers "ata0p1", mounts the
     volume the tool built and runs /bin/hello from it; the shell lists /bin,
     runs the program itself and writes /persist.txt to the volume;
  2. the machine starts again on the same file, mounts the volume it finds,
     and the shell reads the file back and runs a program of /bin again. The
     line of /persist.txt below was written in the first boot and exists
     nowhere else; /bin is the tree the tool wrote and no boot installs it.

`--repeat N` does the whole pair N times on a copy made again each time: a
driver of a disk is only as good as its worst boot, and one pass of a test
that flaked once says nothing.

The exit status is 0 when every boot of every run is what it has to be and 1
otherwise.
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

# The image of the build, and the image of this run: the test works on a
# copy, so a failed boot never leaves a half-written disk for the next one.
BASE_IMAGE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "build-x86", "programs.img")

PROMPT = "microos> "

# The line of the file: written in the first boot, read in the second.
MESSAGE = "the disk remembers"

FIRST_BOOT = [
    "df",
    "ls /bin",
    "exec /bin/hello",
    f"write /persist.txt {MESSAGE}",
    "ls /",
    "cat /persist.txt",
    # The first port of somebody else's library: zcat inflates a .gz of the
    # image with zlib and writes the bytes to a file of MicroFS, so the second
    # boot reads the text that the first one inflated.
    "exec /bin/zcat -o /persist.gz.txt /etc/greeting.gz",
    "exec /bin/wc -c /persist.gz.txt",
    # The tools of sbase read the same volume on the second boot: a file of
    # the disk through the library of a program that grew with them.
    "exec /bin/sb-ls -la /etc",
    "exec /bin/sb-grep -r hostname /etc",
    # SQLite: the first boot creates a database on the volume, the second one
    # reads it out of the same disk after a reset.
    "exec /bin/sqlite3 /sqlite.db < /etc/sql-setup.sql",
]

SECOND_BOOT = [
    "ls /bin",
    "exec /bin/hello",
    "cat /etc/setting",
    "tree /",
    "ls /proc",
    "cat /proc/version",
    "ls /dev",
    "cat /persist.txt",
    "fsck",
    "cat /persist.gz.txt",
    "exec /bin/wc -c /persist.gz.txt",
    # The tools of sbase read the same volume on the second boot: a file of
    # the disk through the library of a program that grew with them.
    "exec /bin/sb-ls -la /etc",
    "exec /bin/sb-grep -r hostname /etc",
    # The database of the first boot: the rows are read and the file is
    # checked by the library of the second boot.
    "exec /bin/sqlite3 /sqlite.db < /etc/sql-read.sql",
    "exec /bin/sqlite3 /sqlite.db < /etc/sql-schema.sql",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "disk-smoke.img")


def make_image(path: str, base: str = BASE_IMAGE) -> None:
    """Seed the image of the run with the tree of the build.

    The bytes of the volume are the tool's (tools/mkfs_mfs.c): this file does
    not write a partition table or a MicroFS record, it copies what the build
    made. A machine that starts without that copy has no /bin, which is the
    state the port is in when QEMU was given no disk.
    """
    if not os.path.isfile(base):
        raise FileNotFoundError(f"missing {base}: run make x86-disk")

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    shutil.copyfile(base, path)


def run_machine(image: str, commands: list, timeout: float) -> str:
    """Boot once on the image, drive the shell and return the transcript."""
    command = [
        "qemu-system-x86_64",
        "-kernel",
        os.path.join(BUILD, "loader32.elf"),
        "-initrd",
        os.path.join(BUILD, "microos64.bin"),
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
        # A raw image on the primary channel: the driver of the port polls
        # the ports of the IDE controller (ADR 0008).
        #
        # The cache of the host is on purpose. `writethrough` turns every
        # 512-byte write of the guest into a synchronous write of the file,
        # and an image on a disk of the machine that runs QEMU then costs a
        # flush of the host filesystem per sector: a format of 8 MB took
        # tens of seconds and looked like a hang of the driver. The reboot
        # this test is about is a new process on the same file, and the page
        # cache of the host is enough for it; the flush of the guest (the
        # ATA command of `sync`) still reaches the medium.
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writeback",
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
    """The answer of every command, split by the prompt of the shell."""
    blocks = output.split(PROMPT)[1:]
    return [blocks[index] if index < len(blocks) else "" for index in range(len(commands))]


def check(first: str, second: str) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the disk of the machine is found and read
    expect("the drive on the channel is found", re.search(r"ata0: \d+ MB", first) is not None)
    expect(
        "the boot reads the MBR and takes the first partition",
        "x86-64: the root volume is ata0p1 (1 partition(s))" in first,
    )

    # A boot that stopped before its shell is the failure this test is
    # famous for, and it deserves a line of its own: the timeout of the run,
    # the driver stuck on the channel and the console of the machine all look
    # the same when only the answer of a command is checked.
    expect(
        "the first boot reaches the shell",
        PROMPT in first,
        f"the machine wrote {len(first)} byte(s) and stopped",
    )

    # --- the first boot: a volume of the build, its programs, and a file in it
    expect(
        "the volume that came with the image is not formatted again",
        "is not formatted yet" not in first,
        "the boot formatted the volume of the tool",
    )

    first_answers = answers(first, FIRST_BOOT)
    expect("every command of the first boot is answered", all(first_answers), "a command got no answer")
    expect(
        "the programs came with the image",
        "hello" in first_answers[1] and "cat" in first_answers[1] and "zcat" in first_answers[1],
        first_answers[1],
    )
    expect(
        "a program of the disk runs at the first boot",
        "PROGRAM: hello from /bin" in first_answers[2]
        and "the program exited with 0" in first_answers[2],
        first_answers[2],
    )
    expect(
        "the shell writes the file",
        "write:" not in first_answers[3],
        first_answers[3],
    )
    expect(
        "the tree of the machine is the tree of every port",
        all(name in first_answers[4] for name in ("bin/", "etc/", "proc/", "dev/"))
        and "persist.txt" in first_answers[4],
        first_answers[4],
    )
    expect(
        "the file is read back at the first boot",
        MESSAGE in first_answers[5],
        first_answers[5],
    )
    expect(
        "zcat inflates a stream of the image",
        "the program exited with 0" in first_answers[6] and "zcat:" not in first_answers[6],
        first_answers[6],
    )
    expect(
        "zcat writes every inflated byte",
        "134 /persist.gz.txt" in first_answers[7],
        first_answers[7],
    )

    # --- the second boot: the same image, the same machine
    expect(
        "the second boot reaches the shell",
        PROMPT in second,
        f"the machine wrote {len(second)} byte(s) and stopped",
    )
    expect(
        "the second boot mounts the partition it finds",
        "x86-64: the root volume is ata0p1" in second,
    )
    expect(
        "the volume of the second boot is not formatted again",
        "is not formatted yet" not in second,
        "the filesystem of the first boot did not survive",
    )
    # The files of /bin were written by the tool, not by a boot: a line that
    # installs a program is the thing this milestone removed.
    expect(
        "no boot installs a program",
        "x86-64: /bin/hello is written" not in first
        and "x86-64: /bin/hello is written" not in second,
    )

    second_answers = answers(second, SECOND_BOOT)
    expect("every command of the second boot is answered", all(second_answers), "")
    expect(
        "the programs are still on the disk",
        "hello" in second_answers[0] and "wc" in second_answers[0],
        second_answers[0],
    )
    expect(
        "a program of the disk runs after the reset",
        "PROGRAM: hello from /bin" in second_answers[1]
        and "the program exited with 0" in second_answers[1],
        second_answers[1],
    )
    expect("the setting came with the image", "hello" in second_answers[2], second_answers[2])
    expect(
        "tree / shows the tree of the machine after the reset",
        all(name in second_answers[3] for name in ("bin/", "etc/", "proc/", "dev/")),
        second_answers[3],
    )
    expect(
        "the procfs is mounted after the reset",
        all(name in second_answers[4] for name in ("version", "uptime", "meminfo")),
        second_answers[4],
    )
    expect(
        "the file of the kernel is read after the reset",
        re.search(r"MicroOS \d+\.\d+\.\d+", second_answers[5]) is not None,
        second_answers[5],
    )
    expect(
        "the devfs is mounted after the reset",
        "tty0" in second_answers[6] and "null" in second_answers[6],
        second_answers[6],
    )
    expect("the file survived the reset", "persist.txt" in second_answers[7], second_answers[7])
    expect(
        "the content survived the reset",
        MESSAGE in second_answers[7],
        f"cat answered {second_answers[7]!r}",
    )
    expect(
        "the volume is still consistent",
        "the volume is consistent" in second_answers[8],
        second_answers[8],
    )
    # The bytes zcat inflated in the first boot are a file of the volume in
    # the second one: the text is the fixture of zlib, not a line of this run.
    expect(
        "the inflated file survived the reset",
        "Micro OS: the first port of a program" in second_answers[9]
        and "zlib 1.3.1, MicroFS and ring 3" in second_answers[9],
        second_answers[9],
    )
    expect(
        "the inflated file has every byte after the reset",
        "134 /persist.gz.txt" in second_answers[10],
        second_answers[10],
    )
    listing = second_answers[11]
    expect(
        "a tool of sbase reads the volume after the reset",
        "root" in listing and "setting" in listing and "greeting.gz" in listing,
        listing,
    )
    recursive = second_answers[12]
    expect(
        "a recursive grep of sbase walks the volume after the reset",
        "/etc/setting:hostname=x86_64" in recursive,
        recursive,
    )

    # --- SQLite: a database written in the first boot and read in the second
    created = first_answers[10]
    expect(
        "the first boot writes a database of SQLite",
        "1|one|1.5" in created and "2|two|2.25" in created and "ok" in created,
        created,
    )
    read_back = second_answers[13]
    expect(
        "the database of the first boot is read after the reset",
        "2|3.75" in read_back,
        read_back,
    )
    expect(
        "the database passes its integrity check after the reset",
        re.search(r"(?m)^ok\r?$", read_back) is not None,
        read_back,
    )
    schema = second_answers[14]
    expect(
        "the schema of the database survived the reset",
        "CREATE TABLE items" in schema and "items" in schema,
        schema,
    )

    return checks


def run_once(args) -> tuple:
    """One image, two boots: the transcripts and how long each one took."""
    if os.path.exists(args.image):
        os.remove(args.image)

    make_image(args.image)

    started = time.monotonic()
    first = run_machine(args.image, FIRST_BOOT, args.timeout)
    middle = time.monotonic()
    second = run_machine(args.image, SECOND_BOOT, args.timeout)
    ended = time.monotonic()

    return first, second, middle - started, ended - middle


def report(checks: list, runs: int) -> None:
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the disk smoke test of the x86-64 port")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s)")
        print("RESULT: FAIL")
        return

    if runs > 1:
        print(f"OK: {runs} run(s) of {len(checks)} check(s) passed")
    else:
        print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk smoke test of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image of the disk")
    parser.add_argument("--mkimage", metavar="PATH", help="write the copy of the build image and stop")
    parser.add_argument("--timeout", type=float, default=60.0, help="the seconds a boot gets")
    parser.add_argument("--repeat", type=int, default=1, help="how many times to boot the pair")
    parser.add_argument("--keep", action="store_true", help="keep the image after the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles of the boots")
    args = parser.parse_args(argv)

    if args.mkimage:
        try:
            make_image(args.mkimage)
        except (FileNotFoundError, OSError) as error:
            print(f"disk: {error}", file=sys.stderr)
            return 2
        print(f"disk: {args.mkimage} (a copy of {BASE_IMAGE})")
        return 0

    for path in ("loader32.elf", "microos64.bin"):
        if not os.path.isfile(os.path.join(BUILD, path)):
            print(f"missing build-x86/{path}: run make x86-build", file=sys.stderr)
            return 2

    runs = 0
    failed_run = None
    last = None

    try:
        for index in range(max(1, args.repeat)):
            first, second, first_s, second_s = run_once(args)
            checks = check(first, second)
            runs += 1

            bad = [item for item in checks if not item[1]]
            print(
                f"  run {index + 1}: {len(checks) - len(bad)} of {len(checks)}"
                f" in {first_s:.1f} s + {second_s:.1f} s"
            )

            if bad:
                failed_run = (checks, first, second)
                break

            last = (checks, first, second)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        if not args.keep and os.path.exists(args.image):
            os.remove(args.image)

    shown = failed_run if failed_run is not None else last

    if shown is None:
        print("no run was made", file=sys.stderr)
        return 2

    checks, first, second = shown

    if args.verbose:
        print("--- the first boot ---")
        print(first)
        print("--- the second boot ---")
        print(second)

    report(checks, runs)

    return 1 if failed_run is not None else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
