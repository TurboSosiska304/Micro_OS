#!/usr/bin/env python3
"""Micro OS — the power smoke test of the x86-64 port (ADR 0050).

The other smoke tests of the machine reset it by killing the process. This
one asks the machine to go down and checks the two promises of the clean
end:

  1. a boot writes a file, `shutdown` prints its steps, and **QEMU ends the
     process by itself** - the S5 of ACPI, not a `kill` of the test;
  2. the same image starts again: the volume is clean (`fsck`), the file is
     there, and `reboot` restarts the machine **in the same QEMU process**
     (the reset of the chipset or of the ACPI register, not a new one);
  3. after the reset the volume is the same and the file is still there,
     and a second `shutdown` ends the process again.

The negative of the lane - a disk that does not answer the flush keeps the
machine alive - belongs to the host unit test (`test_power.c`), because it
needs a block device that fails on purpose and no QEMU at all.

    python3 tools/x86_power_smoke.py [--image PATH] [--disk BUS]
                                     [--timeout SECONDS] [--keep] [--verbose]

The exit status is 0 when every check passed and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import select
import shutil
import subprocess
import sys
import time

from qemu_session import DISKS, drive_args, machine_args, root_name

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
BASE_IMAGE = os.path.join(BUILD, "programs.img")
DEFAULT_IMAGE = os.path.join(BUILD, "power-smoke.img")

PROMPT = b"microos> "
MESSAGE = "the machine that went off"
FILE = "/p.txt"


class MachineGone(RuntimeError):
    """QEMU ended while the test was waiting for something else."""


class Machine:
    """One QEMU process and its console, without `-no-reboot`.

    A reset of the guest is a second boot of the same process here, which
    is the whole point of the reboot check: `-no-reboot` (the choice of
    every other smoke test) turns a reset into an exit and would prove
    nothing about the reset of the port.
    """

    def __init__(self, image: str, disk: str, timeout: float, verbose: bool) -> None:
        self.image = image
        self.disk = disk
        self.timeout = timeout
        self.verbose = verbose
        self.buffer = b""
        self.process: subprocess.Popen | None = None

    def command(self) -> list:
        return [
            "qemu-system-x86_64",
            "-kernel",
            os.path.join(BUILD, "loader32.elf"),
            "-initrd",
            os.path.join(BUILD, "microos64.bin"),
            "-m",
            "256",
            "-nographic",
            *machine_args(self.disk),
            *drive_args(self.image, self.disk),
        ]

    def open(self) -> str:
        self.process = subprocess.Popen(
            self.command(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.buffer = b""
        return self.wait_prompt(self.timeout)

    def _read(self, timeout: float) -> None:
        process = self.process
        if process is None or process.stdout is None:
            raise MachineGone("the machine is not running")

        ready, _, _ = select.select([process.stdout.fileno()], [], [], timeout)
        if not ready:
            return

        chunk = os.read(process.stdout.fileno(), 4096)
        if not chunk:
            raise MachineGone("QEMU ended")

        self.buffer += chunk
        if self.verbose:
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()

    def wait_prompt(self, timeout: float) -> str:
        """Read until the shell prints a prompt; return the block before it."""
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            index = self.buffer.find(PROMPT)
            if index >= 0:
                block = self.buffer[:index]
                self.buffer = self.buffer[index + len(PROMPT) :]
                return block.decode("utf-8", errors="replace")
            self._read(0.1)

        raise TimeoutError(f"the shell answered nothing in {timeout:.0f} s")

    def wait_exit(self, timeout: float) -> int:
        """Read until QEMU is gone and return its exit code."""
        process = self.process
        if process is None:
            raise MachineGone("the machine was never started")

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if process.poll() is not None:
                return process.returncode

            try:
                self._read(0.1)
            except MachineGone:
                try:
                    return process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            except (OSError, ValueError):
                break

        raise TimeoutError("QEMU did not end by itself")

    def run(self, command: str, timeout: float | None = None) -> str:
        process = self.process
        if process is None or process.stdin is None:
            raise MachineGone("the machine is not running")

        process.stdin.write((command + "\n").encode("ascii"))
        process.stdin.flush()
        return self.wait_prompt(timeout or self.timeout)

    def close(self) -> None:
        process = self.process
        if process is None:
            return

        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

        if process.stdout is not None:
            process.stdout.close()
        if process.stdin is not None:
            process.stdin.close()
        self.process = None


def make_image(path: str) -> None:
    if not os.path.isfile(BASE_IMAGE):
        raise FileNotFoundError(f"missing {BASE_IMAGE}: run make x86-disk")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    shutil.copyfile(BASE_IMAGE, path)


def run(args) -> list:
    checks: list = []

    def expect(name: str, ok: bool, detail: str = "") -> None:
        checks.append((name, bool(ok), detail))

    make_image(args.image)

    first = Machine(args.image, args.disk, args.timeout, args.verbose)
    try:
        banner = first.open()
        expect("the first boot reaches the shell", len(banner) > 0)

        block = first.run(f"write {FILE} {MESSAGE}")
        expect("the shell writes the file", "write:" not in block, block)

        block = first.run("fsck")
        expect("the volume is consistent before the shutdown", "the volume is consistent" in block,
               block)

        first.process.stdin.write(b"shutdown\n")
        first.process.stdin.flush()

        try:
            code = first.wait_exit(15)
            ended = True
        except TimeoutError:
            code = None
            ended = False

        transcript = first.buffer.decode("utf-8", errors="replace")
        expect("shutdown ends the QEMU process by itself", ended, transcript[-400:])
        expect("the shutdown reports the flush of the disks",
               "power: the volumes are synced, the disks are flushed" in transcript
               or "power: the volumes are synced" in transcript,
               transcript[-600:])
        expect("the shutdown takes the power down (ACPI S5)",
               "x86-64: the power goes off" in transcript and "acpi" in transcript.lower(),
               transcript[-600:])
        expect("QEMU leaves with a success", code == 0, f"the exit code was {code}")
    finally:
        first.close()

    second = Machine(args.image, args.disk, args.timeout, args.verbose)
    try:
        second.open()

        block = second.run("fsck")
        expect("the volume of the second boot is clean", "the volume is consistent" in block, block)

        block = second.run(f"cat {FILE}")
        expect("the file of the first boot survived the shutdown", MESSAGE in block, block)

        # The reboot: the guest resets and the same process boots again.
        block = second.run("reboot", timeout=max(args.timeout, 60.0))
        expect("the reboot resets the machine", "x86-64: the machine restarts" in block,
               block[:400])
        expect("the machine boots again in the same process",
               "MicroOS" in block or "the kernel is up" in block, block[:400])
        expect("the boot after the reset finds its root",
               f"x86-64: the root volume is {root_name(args.disk)}" in block, block[:800])

        block = second.run("fsck")
        expect("the volume after the reset is clean", "the volume is consistent" in block, block)

        block = second.run(f"cat {FILE}")
        expect("the file survived the reset", MESSAGE in block, block)

        second.process.stdin.write(b"shutdown\n")
        second.process.stdin.flush()
        try:
            code = second.wait_exit(15)
            ended = True
        except TimeoutError:
            code = None
            ended = False

        expect("the second shutdown ends the process", ended, second.buffer.decode(
            "utf-8", errors="replace")[-400:])
        expect("the second shutdown is a success too", code == 0, f"the exit code was {code}")
    finally:
        second.close()

    return checks


def report(checks: list) -> int:
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the power smoke test of the x86-64 port")
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


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the power smoke test of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image of the disk")
    parser.add_argument("--timeout", type=float, default=60.0, help="the seconds a boot gets")
    parser.add_argument("--disk", choices=DISKS, default="ide", help="the bus of the disk")
    parser.add_argument("--keep", action="store_true", help="keep the image after the run")
    parser.add_argument("--verbose", action="store_true", help="print the console as it comes")
    args = parser.parse_args(argv)

    try:
        checks = run(args)
    finally:
        if not args.keep and os.path.exists(args.image) and not args.verbose:
            os.remove(args.image)

    return report(checks)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
