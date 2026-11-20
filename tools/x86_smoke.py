#!/usr/bin/env python3
"""Micro OS — the smoke test of the x86-64 port.

The kernel of the host build is a process, and its smoke test runs it. This
one boots the kernel on a real (emulated) processor: QEMU loads the loader
of the port, the loader turns the long mode on and copies the kernel, and
the kernel prints its banner on the serial port. There is no operating
system under it and no libc.

Then the shell has to answer: the commands are typed into the machine once
the console is up, and their answers are read back from the same transcript.
The prompt paces the typing on its own — the next command goes out when the
shell prints the prompt of the previous one — so a line is never split by
the small FIFO of the serial port of the emulated chip.

Between the boot and the shell the port of this machine brings up a filesystem
and runs a program out of it: a volume in its memory is mounted at /, the image
of /bin/hello is written to it and given the mode of a program, and the boot
asks for it with execve. The line the program writes to COM1 is part of the
transcript; it is checked below, because it is the one thing a kernel that only
prints its banner cannot fake.

    tools/x86_smoke.py [--timeout SECONDS] [--verbose]

The exit status is 0 when the kernel came up, the program ran out of a file and
the shell answered, and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import subprocess
import sys
import time

# The commands that are typed into the shell. The answers are read by their
# place in the list, the way the smoke test of the simulator does it. The last
# one looks at the tree the boot left behind: a program that ran out of a file
# is a program that is still in /bin afterwards.
COMMANDS = [
    "help",
    "uname",
    "uptime",
    "ls /bin",
    "chmod 0644 /bin/hello",
    "exec /bin/hello",
    "chmod 0755 /bin/hello",
    "chown 0 /bin/hello",
    "exec /bin/hello",
]

# The prompt of the shell. It is written in one call, so the log of another
# task never splits it, and the answers can be separated by it.
PROMPT = "microos> "

# What the kernel has to say before the shell exists. The banner is the proof
# that the whole chain works: the loader, the switch to long mode, the
# console of COM1, the contract of the port and the boot of the kernel.
EXPECTED = [
    "board: qemu_x86_64",
    "port: x86_64, contract 1.3",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")


def run_machine(loader: str, kernel: str, timeout: float) -> str:
    """Boot the machine, drive the shell and return the transcript.

    The output is read while the machine runs, not after it stops: the
    commands have to arrive after the shell is ready, and the answer of a
    command is the proof the next command may go out.
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
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
                break  # the machine went away on its own

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            # One command per prompt: the shell prints the prompt before it
            # reads, so the line is written only when the shell is waiting.
            while sent < len(COMMANDS) and text.count(PROMPT) > sent:
                process.stdin.write((COMMANDS[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            # The prompt after the last command means its answer is done.
            if text.count(PROMPT) > len(COMMANDS):
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        # Whatever the machine wrote before it stopped is part of the answer.
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


def check(output: str) -> list:
    """The expectations about the boot and the shell of the machine."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the boot
    for line in EXPECTED:
        expect(line, line in output)

    # --- the program of the milestone
    # The line in the middle is not written by the kernel: it comes out of the
    # image that was written to and read back from /bin/hello, one byte at a
    # time, so it is the proof that the program ran and not that the kernel
    # said it would.
    expect("the filesystem takes the image", "x86-64: /bin/hello is written" in output)
    expect(
        "the boot asks for the program through the call",
        "x86-64: running /bin/hello through the call" in output,
    )
    expect("the program writes to the console", "PROGRAM: hello from /bin" in output)
    expect("the kernel is back from the program", "x86-64: the program returned" in output)

    # --- the shell
    expect("the shell starts", "MicroOS shell ready" in output)
    expect(
        "the shell runs in a task",
        "no tasks yet" not in output,
        "the task substrate of the port is not there",
    )

    # The answers, separated by the prompt. The echo of the typed line is
    # part of the block; the checks below look for the answer itself.
    blocks = output.split(PROMPT)[1:]

    def answer(command: str, occurrence: int = 0) -> str:
        """The answer of one command of the list, empty when it did not run.

        A command may be typed twice to see the two sides of a rule - the
        second exec of the file comes after the mode is given back - so the
        occurrence of the line decides which block is the answer.
        """
        indexes = [index for index, line in enumerate(COMMANDS) if line == command]
        if occurrence >= len(indexes):
            return ""
        index = indexes[occurrence]
        return blocks[index] if index < len(blocks) else ""

    expect(
        "every command is answered",
        len(blocks) > len(COMMANDS),
        f"{len(blocks)} block(s) for {len(COMMANDS)} command(s)",
    )

    expect("help lists the commands", "list the available commands" in answer("help"))
    # The version is whatever the build says: the shape is checked, so a
    # release does not break the smoke test.
    expect(
        "uname prints the name and the version",
        re.search(r"MicroOS \d+\.\d+\.\d+", answer("uname")) is not None,
    )
    expect(
        "uptime prints the time",
        re.search(r"up \d+ s", answer("uptime")) is not None,
    )
    # The volume is not part of the kernel image: the file was written to it at
    # the boot, so a listing is the proof that the bytes are on a filesystem and
    # not a line the kernel printed about itself.
    expect("the program is a file in /bin", "hello" in answer("ls /bin"))
    # The same file, asked for with a command of the shell: the library, the
    # syscall, the mode of the record and the jump are the ones of a user and
    # not the ones of the boot.
    expect(
        "the shell runs the program through execve",
        "the program has finished" in answer("exec /bin/hello", 1),
    )
    expect(
        "the program of the shell writes to the console",
        "PROGRAM: hello from /bin" in answer("exec /bin/hello", 1),
    )
    # The mode is a fact of the record and not a word of the kernel: take the
    # bit away with the shell and the same call refuses, give it back and the
    # program runs again. The library writes its errno through %fs here, so a
    # machine without a thread pointer would fault before it answered.
    expect(
        "a file without the x bit is not a program",
        "has no execute bit" in answer("exec /bin/hello", 0),
    )
    expect(
        "the owner is a number of the shell",
        "chown:" not in answer("chown 0 /bin/hello"),
    )

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=20.0, help="the seconds the machine gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    try:
        output = run_machine(loader, kernel, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    if args.verbose:
        print(output)

    checks = check(output)
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the smoke test of the x86-64 port")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)
    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        print("--- the console of the machine ---")
        print(output)
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
