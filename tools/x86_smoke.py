#!/usr/bin/env python3
"""Micro OS — the smoke test of the x86-64 port.

The kernel of the host build is a process, and its smoke test runs it. This
one boots the kernel on a real (emulated) processor: QEMU loads the loader
of the port, the loader turns the long mode on and copies the kernel, and
the kernel prints its banner on the serial port. There is no operating
system under it and no libc.

    tools/x86_smoke.py [--timeout SECONDS]

The exit status is 0 when the kernel came up and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

# What the kernel has to say before it stops. The banner is the proof that
# the whole chain works: the loader, the switch to long mode, the console of
# COM1, the contract of the port and the boot of the kernel.
EXPECTED = [
    "board: qemu_x86_64",
    "port: x86_64, contract 1.2",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=20.0, help="the seconds the machine gets")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

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

    try:
        run = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=args.timeout,
            check=False,
        )
        output = run.stdout + run.stderr
    except subprocess.TimeoutExpired as expired:
        # The kernel halts and the machine waits: a timeout is the good end.
        def as_text(value) -> str:
            if value is None:
                return ""
            return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value

        output = as_text(expired.stdout) + as_text(expired.stderr)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    failed = 0
    for line in EXPECTED:
        ok = line in output
        print(f"  {'ok  ' if ok else 'FAIL'}  {line}")
        if not ok:
            failed += 1

    print()
    if failed:
        print(f"RESULT: FAIL ({failed} line(s) missing)")
        print("--- the console of the machine ---")
        print(output)
        return 1

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
