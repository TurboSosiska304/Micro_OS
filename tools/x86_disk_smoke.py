#!/usr/bin/env python3
"""Micro OS — the smoke test of the disk of the x86-64 port.

The other smoke test of this machine (`x86_smoke.py`) boots a kernel whose
filesystem lives in the memory of the port: it is gone when the machine is
switched off. This one gives QEMU a raw image on the primary IDE channel and
checks the thing that no line of a boot can prove: a file written by one boot
is read by the next one, out of the same 8 MB of the image.

    python3 tools/x86_disk_smoke.py [--image PATH] [--timeout SECONDS]

The image is made before the run (the MBR of one partition, the partition
itself blank, the way a formatter of a flash leaves it) and removed after it,
unless --keep says otherwise. `--mkimage PATH` only writes the image, which is
what `make x86-run-disk` calls when there is no disk yet.

Two boots of the same machine, the same image:

  1. the boot finds "ata0", reads the MBR, registers "ata0p1" and formats the
     blank partition, because a volume that has never been written is what a
     filesystem makes; the shell writes /persist.txt to it;
  2. the machine starts again on the same file, mounts the volume it finds
     and the shell reads the file back. The line of /persist.txt below was
     written in the first boot and exists nowhere else.

The exit status is 0 when both boots are what they have to be and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import subprocess
import sys
import time

# The size of the image, and where its only partition starts: the 1 MiB that
# every tool of every system leaves before the first partition.
IMAGE_MB = 8
SECTOR = 512
PARTITION_START = 2048

PROMPT = "microos> "

# The line of the file: written in the first boot, read in the second.
MESSAGE = "the disk remembers"

FIRST_BOOT = [
    "df",
    f"write /persist.txt {MESSAGE}",
    "ls /",
    "cat /persist.txt",
]

SECOND_BOOT = [
    "ls /",
    "cat /persist.txt",
    "fsck",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "disk-smoke.img")


def make_image(path: str, size_mb: int = IMAGE_MB) -> None:
    """Write the image: a blank medium with the MBR of one partition.

    The blocks of the partition are 0xFF, the state of an erased flash, so
    that the filesystem of the first boot recognises a volume that was never
    written and formats it. The MBR itself is what a tool writes: zeros outside
    the entries, the type 0x83 of a Linux partition and the signature.
    """
    sectors = size_mb * 1024 * 1024 // SECTOR
    blank = b"\xff" * (1024 * 1024)

    with open(path, "wb") as image:
        for _ in range(size_mb):
            image.write(blank)

        table = bytearray(SECTOR)
        table[446 + 4] = 0x83
        table[446 + 8 : 446 + 12] = PARTITION_START.to_bytes(4, "little")
        table[446 + 12 : 446 + 16] = (sectors - PARTITION_START).to_bytes(4, "little")
        table[510] = 0x55
        table[511] = 0xAA

        image.seek(0)
        image.write(table)


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
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writethrough",
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

    # --- the first boot: a blank partition, formatted, and a file in it
    expect(
        "the blank partition is formatted at the first boot",
        "the volume of 'ata0p1' is not formatted yet" in first,
    )

    first_answers = answers(first, FIRST_BOOT)
    expect("every command of the first boot is answered", all(first_answers), "a command got no answer")
    expect(
        "the shell writes the file",
        "write:" not in first_answers[1],
        first_answers[1],
    )
    expect("the file is in the tree", "persist.txt" in first_answers[2], first_answers[2])
    expect(
        "the file is read back at the first boot",
        MESSAGE in first_answers[3],
        first_answers[3],
    )

    # --- the second boot: the same image, the same machine
    expect(
        "the second boot mounts the partition it finds",
        "x86-64: the root volume is ata0p1" in second,
    )
    expect(
        "the volume of the second boot is not formatted again",
        "is not formatted yet" not in second,
        "the filesystem of the first boot did not survive",
    )
    expect(
        "the program of /bin is written again",
        "x86-64: /bin/hello is written" in second,
    )

    second_answers = answers(second, SECOND_BOOT)
    expect("every command of the second boot is answered", all(second_answers), "")
    expect("the file survived the reset", "persist.txt" in second_answers[0], second_answers[0])
    expect(
        "the content survived the reset",
        MESSAGE in second_answers[1],
        f"cat answered {second_answers[1]!r}",
    )
    expect(
        "the volume is still consistent",
        "the volume is consistent" in second_answers[2],
        second_answers[2],
    )

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the disk smoke test of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image of the disk")
    parser.add_argument("--mkimage", metavar="PATH", help="write the image and stop")
    parser.add_argument("--size-mb", type=int, default=IMAGE_MB, help="the size of a new image")
    parser.add_argument("--timeout", type=float, default=25.0, help="the seconds a boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the image after the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles of both boots")
    args = parser.parse_args(argv)

    if args.mkimage:
        os.makedirs(os.path.dirname(os.path.abspath(args.mkimage)), exist_ok=True)
        make_image(args.mkimage, args.size_mb)
        print(f"disk: {args.mkimage} ({args.size_mb} MB, one partition at LBA {PARTITION_START})")
        return 0

    for path in ("loader32.elf", "microos64.bin"):
        if not os.path.isfile(os.path.join(BUILD, path)):
            print(f"missing build-x86/{path}: run make x86-build", file=sys.stderr)
            return 2

    try:
        if os.path.exists(args.image):
            os.remove(args.image)
        make_image(args.image, args.size_mb)

        first = run_machine(args.image, FIRST_BOOT, args.timeout)
        second = run_machine(args.image, SECOND_BOOT, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        if not args.keep and os.path.exists(args.image):
            os.remove(args.image)

    if args.verbose:
        print("--- the first boot ---")
        print(first)
        print("--- the second boot ---")
        print(second)

    checks = check(first, second)
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
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
