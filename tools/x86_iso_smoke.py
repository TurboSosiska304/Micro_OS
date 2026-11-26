#!/usr/bin/env python3
"""Micro OS — the smoke test of the bootable ISO of the x86-64 port.

`make x86-smoke` boots the loader and the kernel the way QEMU takes them
(`-kernel` and `-initrd`): the machine starts with the image of the port in
its memory and no medium of any kind. This test boots the other machine, the
one a person has: a BIOS reads the El Torito catalogue of a CD, starts the
boot loader of the image, and the loader finds the kernel and the root image
as two modules of the same medium.

    python3 tools/x86_iso_smoke.py [--iso PATH] [--timeout SECONDS] [--keep]

The ISO is built by `make x86-iso` (tools/x86_iso.py) and is not modified:
the machine is started with `-cdrom` and nothing else. The volume of the boot
is read-only - it is the image of the CD - so the test asks for it and does
not write to it: a boot that fails there is the failure this medium has, and
the line about the write that is refused is part of the transcript.

The exit status is 0 when the machine came up from the CD, mounted the root
image and ran the programs of /bin, and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86-iso")
DEFAULT_ISO = os.path.join(BUILD, "microos.iso")

PROMPT = "microos> "

# The boot, then the questions of a person at the shell of a machine that has
# no disk: the tree of the CD, the numbers of the volume, and a program of
# /bin run the way the boot runs it.
COMMANDS = [
    "ls /bin",
    "df",
    "cat /etc/setting",
    "exec /bin/hello",
    "ls /",
]


def answers(output: str, commands: list) -> list:
    """The answer of every command, split by the prompt of the shell."""
    blocks = output.split(PROMPT)[1:]
    return [blocks[index] if index < len(blocks) else "" for index in range(len(commands))]


def run_machine(iso: str, commands: list, timeout: float) -> str:
    """Boot the ISO in QEMU, drive the shell and return the transcript."""
    command = [
        "qemu-system-x86_64",
        "-cdrom",
        iso,
        "-m",
        "256",
        # The machine has no disk and no floppy: boot the CD first instead of
        # letting SeaBIOS try the two and say that they are not there.
        "-boot",
        "d",
        "-nographic",
        "-no-reboot",
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


def check(output: str) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the way in: a BIOS, the catalogue of the CD and the loader
    expect(
        "the loader of the ISO started and verified the kernel",
        "Micro OS: image signature ok" in output,
        "the boot did not reach the loader of the port",
    )
    expect(
        "the kernel of the port came up",
        re.search(r"MicroOS \d+\.\d+\.\d+", output) is not None,
    )
    expect(
        "neither module of the boot lies over the kernel image",
        "a module of the boot lies over the kernel image" not in output,
        "the loader refused the layout of the CD",
    )

    # --- the medium: a read-only volume, mounted from the second module
    expect(
        "the boot found the root image of the CD",
        "x86-64: the boot carries a read-only volume" in output,
        "the second module of the boot is not the root image",
    )
    expect(
        "the root of the machine is the volume of the CD",
        "x86-64: the root volume is boot0p1" in output,
    )
    expect(
        "the read-only boot says its logs are in RAM",
        "the root is read-only, /etc/bootlog.log and /etc/run.log are in RAM" in output,
        "the boot did not say where the logs are kept",
    )
    expect(
        "the volume of the CD is not formatted again",
        "is not formatted yet" not in output,
        "the boot formatted the volume of the boot medium",
    )
    expect(
        "the boot ran a program out of the CD",
        "PROGRAM: hello from /bin" in output and "the program exited with 0" in output,
        "the program of /bin did not run at the boot",
    )

    # --- the shell of the machine that came from a CD
    expect(
        "the machine reached the shell",
        PROMPT in output,
        f"the machine wrote {len(output)} byte(s) and stopped",
    )

    replies = answers(output, COMMANDS)
    expect("every command is answered", all(replies), "a command got no answer")
    expect(
        "the programs came with the root image",
        "hello" in replies[0] and "cat" in replies[0],
        replies[0],
    )
    expect(
        "df reads the volume of the CD",
        "blocks:" in replies[1] and "inodes:" in replies[1],
        replies[1],
    )
    expect("the setting came with the root image", "hello" in replies[2], replies[2])
    expect(
        "a program of the CD runs from the shell",
        "PROGRAM: hello from /bin" in replies[3] and "the program exited with 0" in replies[3],
        replies[3],
    )
    # `ls` of the CD is the program of sbase: a directory is its name and no
    # slash at the end, so the check reads a line and not a suffix.
    expect(
        "the tree of the machine is the tree of every port",
        all(
            re.search(rf"(?m)^{name}\r?$", replies[4]) is not None
            for name in ("bin", "etc", "proc", "dev")
        ),
        replies[4],
    )

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the bootable ISO")
    parser.add_argument("--iso", default=DEFAULT_ISO)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--keep", action="store_true", help="keep the transcript")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.iso):
        print(f"missing {args.iso}: run make x86-iso", file=sys.stderr)
        return 2

    output = run_machine(args.iso, COMMANDS, args.timeout)

    if args.keep:
        path = args.iso + ".transcript"
        with open(path, "w", encoding="utf-8") as file:
            file.write(output)
        print(f"the transcript is in {path}")

    checks = check(output)
    failed = 0

    for name, ok, detail in checks:
        print(f"{'ok  ' if ok else 'FAIL'}  {name}")

        if not ok:
            failed += 1

            if detail:
                print(f"      {detail}")

    if failed:
        print(f"\n{failed} check(s) failed; the machine wrote {len(output)} byte(s)")
        return 1

    print(f"\nthe ISO boots: {len(checks)} check(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
