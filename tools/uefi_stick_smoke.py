#!/usr/bin/env python3
"""Micro OS — the gate of the stick: a file survives the power of a laptop.

The medium of a UEFI machine is a GPT with two partitions: an ESP the firmware
reads (the loader, the kernel and the image of a boot that has no driver) and a
MicroFS volume the kernel writes through the USB port (ADR 0056). This gate
boots the stick under OVMF as a USB storage device, writes a file, takes the
power away - the QEMU process is terminated, not asked to shut down - and
boots the same bytes again: the file is there.

    python3 tools/uefi_stick_smoke.py [--image PATH] [--build DIR]
                                      [--mkfs PATH] [--ovmf-code PATH]
                                      [--ovmf-vars PATH] [--timeout SECONDS]
                                      [--keep] [--verbose]

The checks are the promise of the stick, line by line:

  * the firmware starts the loader from the USB stick, the kernel finds the
    volume of the second partition (`usb0p2`) and mounts it `rw`;
  * `/proc/mounts` says `usb0p2 / mfs rw`, and `write /persist.txt` answers
    with the bytes it wrote;
  * the power goes; the SHA-256 of the medium changed, and a second boot reads
    the file back and `fsck` finds the volume consistent;
  * a stick whose second partition is a volume of another system is not taken
    for a root: the machine boots the read-only image of the ESP, the write is
    refused, and the SHA-256 of the stick is the same before and after.

The exit status is 0 when every check passed, 2 when a tool or a file is
missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
DEFAULT_STICK = os.path.join(ROOT, "build-uefi", "stick.img")
STICK_TOOL = os.path.join(ROOT, "tools", "uefi_stick.py")

PROMPT = "microos> "
PERSIST_PATH = "/persist.txt"
PERSIST_TEXT = "the-stick-remembers"

OVMF_DIRS = ["/usr/share/edk2/x64", "/usr/share/OVMF", "/usr/share/edk2/ovmf"]
OVMF_CODE = ["OVMF_CODE.4m.fd", "OVMF_CODE.fd", "OVMF_CODE_4M.fd"]
OVMF_VARS = ["OVMF_VARS.4m.fd", "OVMF_VARS.fd", "OVMF_VARS_4M.fd"]

CHECKS: list = []


def check(title: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((title, bool(ok), detail))


def short(text: str, lines: int = 6) -> str:
    if not text:
        return "(no words)"
    return "\n          ".join(text.strip().splitlines()[-lines:])


def digest(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_ovmf(names: list, explicit: str | None, env: str) -> str | None:
    override = explicit or os.environ.get(env)

    if override:
        return override if os.path.isfile(override) else None

    for directory in OVMF_DIRS:
        for name in names:
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                return path

    return None


class Machine:
    """One QEMU of the stick, booted from the USB port by OVMF."""

    def __init__(self, stick: str, code: str, variables: str, memory: str = "512") -> None:
        self.stick = stick
        self.code = code
        self.variables = variables
        self.memory = memory
        self.process: subprocess.Popen | None = None
        self.transcript = b""
        self.pending = b""
        self.work = tempfile.mkdtemp(prefix="stick-smoke-", dir="/tmp/opencode")

    def open(self, timeout: float) -> str:
        variables = os.path.join(self.work, "vars.fd")
        shutil.copyfile(self.variables, variables)

        command = [
            "qemu-system-x86_64",
            "-drive", f"if=pflash,format=raw,readonly=on,file={self.code}",
            "-drive", f"if=pflash,format=raw,file={variables}",
            "-device", "qemu-xhci,id=xhci",
            "-drive", f"if=none,id=usbdisk,file={self.stick},format=raw",
            "-device", "usb-storage,drive=usbdisk,bus=xhci.0",
            "-m", self.memory,
            "-nographic",
            "-no-reboot",
            "-boot", "c",
        ]

        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        return self._wait_prompt(timeout)

    def _read(self, timeout: float, until_prompt: bool) -> str:
        assert self.process is not None and self.process.stdout is not None
        descriptor = self.process.stdout.fileno()
        deadline = time.monotonic() + timeout
        start = len(self.transcript)

        while True:
            if until_prompt and PROMPT.encode() in self.pending:
                index = self.pending.find(PROMPT.encode())
                block = self.pending[:index]
                self.pending = self.pending[index + len(PROMPT):]
                return block.decode("utf-8", errors="replace")

            if time.monotonic() >= deadline:
                raise TimeoutError("the machine answered nothing")

            ready, _, _ = select.select([descriptor], [], [], 0.1)
            if not ready:
                continue

            chunk = os.read(descriptor, 4096)
            if not chunk:
                raise RuntimeError("QEMU went away")

            self.transcript += chunk
            self.pending += chunk

            if not until_prompt:
                return self.transcript[start:].decode("utf-8", errors="replace")

    def _wait_prompt(self, timeout: float) -> str:
        return self._read(timeout, True)

    def run(self, command: str, timeout: float = 60.0) -> str:
        assert self.process is not None and self.process.stdin is not None
        self.process.stdin.write((command + "\n").encode("ascii"))
        self.process.stdin.flush()
        return self._read(timeout, True)

    def boot_text(self) -> str:
        return self.transcript.decode("utf-8", errors="replace")

    def kill(self) -> None:
        """The power goes: the process is taken away, not asked to stop."""
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def close(self) -> None:
        self.kill()
        if self.process is not None:
            if self.process.stdout is not None:
                self.process.stdout.close()
            if self.process.stdin is not None:
                self.process.stdin.close()
        shutil.rmtree(self.work, ignore_errors=True)


def boot_positive(stick: str, code: str, variables: str, timeout: float, verbose: bool) -> tuple:
    """The first boot: the stick is the root, the write answers."""
    machine = Machine(stick, code, variables)
    try:
        machine.open(timeout)
        booted = machine.boot_text()

        check("the machine boots from the stick on the firmware",
              "root: the root is mfs on usb0p2" in booted, short(booted))

        mounts = machine.run("cat /proc/mounts", timeout=60.0)
        wrote = machine.run(f"write {PERSIST_PATH} {PERSIST_TEXT}", timeout=60.0)
        ports = machine.run("ls /bin/ports", timeout=60.0)
    finally:
        machine.kill()
        transcript = machine.boot_text()
        machine.close()

    check("the kernel finds the volume of the second partition",
          "usb0: usb0p2: a volume of this system (mfs)" in booted, short(booted))
    check("the mount opens the writes of the stick",
          "usb0: the writes of usb0p2 are open: the volume is named in /etc/volumes" in booted,
          short(booted))
    check("the root of the stick is mounted read-write",
          "root: usb0p2 is mounted read-write, the volume is named in /etc/volumes" in booted,
          short(booted))
    check("/proc/mounts says the root of the stick is rw",
          re.search(r"usb0p2\s+/\s+mfs\s+rw", mounts) is not None, mounts)
    check("the shell writes a file of the stick",
          "bytes written to" in wrote or "written to" in wrote, short(wrote))
    check("the machine was alive while it wrote",
          "microos> " in transcript, short(transcript))

    # The programs of the SDK are on the stick and not only in the disk image
    # of `make x86-disk`: the volume of the stick is written by its own tool,
    # and a release whose notes promise `kilo` is a stick that answers it.
    check("the volume of the stick carries the programs of the SDK",
          "kilo" in ports and "cksum" in ports, short(ports))

    return booted, wrote, transcript


def boot_persisted(stick: str, code: str, variables: str, timeout: float) -> str:
    """The second boot: the file of the first one is there."""
    machine = Machine(stick, code, variables)
    try:
        machine.open(timeout)
        booted = machine.boot_text()
        read = machine.run(f"cat {PERSIST_PATH}", timeout=60.0)
        fsck = machine.run("fsck", timeout=120.0)
        mounts = machine.run("cat /proc/mounts", timeout=60.0)
    finally:
        transcript = machine.boot_text()
        machine.close()

    check("the second boot finds the volume of the stick",
          "root: the root is mfs on usb0p2" in booted, short(booted))
    check("the file written before the power is there",
          PERSIST_TEXT in read, short(read))
    check("the volume of the stick is consistent after the power",
          "the volume is consistent" in fsck, short(fsck))
    check("the second boot mounts the stick read-write again",
          re.search(r"usb0p2\s+/\s+mfs\s+rw", mounts) is not None, mounts)
    check("the machine of the second boot answered every line",
          "microos> " in transcript, short(transcript))

    return booted


def boot_foreign(stick: str, code: str, variables: str, timeout: float) -> str:
    """The negative: the second partition is not a volume of this system."""
    machine = Machine(stick, code, variables)
    try:
        machine.open(timeout)
        booted = machine.boot_text()
        mounts = machine.run("cat /proc/mounts", timeout=60.0)
        wrote = machine.run(f"write /nope.txt {PERSIST_TEXT}", timeout=60.0)
    finally:
        transcript = machine.boot_text()
        machine.close()

    check("a foreign second partition is named for what it is",
          re.search(r"usb0: usb0p2: .*another system", booted) is not None, short(booted))
    check("the machine boots the image of the ESP, not the foreign stick",
          "root: the root is mfs on boot0p1" in booted, short(booted))
    check("the image of the ESP is read-only",
          "root: boot0p1" in booted and "read-only" in booted, short(booted))
    check("/proc/mounts says the root of the image is ro",
          re.search(r"boot0p1\s+/\s+mfs\s+ro", mounts) is not None, mounts)
    check("the write to a foreign stick is refused",
          "input/output error" in wrote or "read-only" in wrote, short(wrote))
    check("the machine of the negative boot answered every line",
          "microos> " in transcript, short(transcript))

    return booted


def build_foreign(work: str, build: str, mkfs: str, verbose: bool) -> str | None:
    """A stick of the same shape whose second partition is a volume of nobody."""
    image = os.path.join(work, "foreign.img")
    command = [sys.executable, STICK_TOOL, "--image", image, "--build", build, "--mkfs", mkfs,
               "--second", "fat32"]
    if verbose:
        print("+ " + " ".join(command), file=sys.stderr)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        return None
    return image


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the gate of the stick of a person")
    parser.add_argument("--image", default=DEFAULT_STICK, help="the stick of the run")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--ovmf-code", default=None, help="the code of the firmware")
    parser.add_argument("--ovmf-vars", default=None, help="the variables of the firmware")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the stick of the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles as well")
    args = parser.parse_args(argv)

    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    code = find_ovmf(OVMF_CODE, args.ovmf_code, "OVMF_CODE")
    variables = find_ovmf(OVMF_VARS, args.ovmf_vars, "OVMF_VARS")

    if code is None or variables is None:
        print("no OVMF firmware (install edk2-ovmf, or name --ovmf-code/--ovmf-vars)",
              file=sys.stderr)
        return 2

    if not os.path.isfile(args.image):
        print(f"missing {args.image}: run make uefi-stick", file=sys.stderr)
        return 2

    work = tempfile.mkdtemp(prefix="stick-gate-", dir="/tmp/opencode")
    stick = os.path.join(work, "stick.img")
    shutil.copyfile(args.image, stick)

    try:
        before = digest(stick)
        boot_positive(stick, code, variables, args.timeout, args.verbose)
        after_write = digest(stick)

        check("the write of the first boot reached the medium", after_write != before,
              f"before {before}, after {after_write}")

        boot_persisted(stick, code, variables, args.timeout)

        foreign = build_foreign(work, args.build, args.mkfs, args.verbose)

        if foreign is None:
            check("the foreign stick of the gate is written", False, "uefi_stick.py failed")
        else:
            before_foreign = digest(foreign)
            boot_foreign(foreign, code, variables, args.timeout)
            after_foreign = digest(foreign)
            check("the foreign stick is not written", before_foreign == after_foreign,
                  f"before {before_foreign}, after {after_foreign}")
    except (OSError, RuntimeError, TimeoutError) as error:
        check("the machine of the stick answers", False, str(error))
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    failed = [item for item in CHECKS if not item[1]]

    print("Micro OS — the stick of a person keeps a file")
    print("=" * 72)

    for title, ok, detail in CHECKS:
        print(f"  {' ok ' if ok else 'FAIL'}  {title}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 72)

    if failed:
        print(f"FAILED: {len(failed)} of {len(CHECKS)} check(s)")
        print("RESULT: FAIL")
        return 1

    print(f"OK: {len(CHECKS)} check(s) passed")
    print("RESULT: PASS")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
