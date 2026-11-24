#!/usr/bin/env python3
"""Micro OS — the keyboard of the USB on the x86-64 port (ADR 0040).

The machine of a laptop of these years has no PS/2 port its firmware will
share and no COM port, so the keyboard of the person in front of the screen is
a device of the USB and the only way to it is the host controller of the PCI
bus. This test boots exactly that machine: the i8042 of QEMU is turned off
(`-machine pc,i8042=off`), so the *only* keyboard of the machine is the one of
the emulated xHCI controller (`-device qemu-xhci -device usb-kbd`), and the
keys of the test arrive through the monitor of QEMU (`sendkey`), which the
input layer of QEMU hands to the USB keyboard of the machine because the PS/2
keyboard does not exist. Nothing of the transcript below can therefore be a
keystroke of a PS/2.

The checks are the two halves of the contract:

  1. the characters: a line is typed into the shell, the shell echoes it and
     answers it, so the controller, the enumeration, the HID report, the
     character map and the console all worked; a Shift and a Backspace are
     part of the line because a modifier and an editing key are the parts a
     plain letter does not prove;
  2. the events: `/bin/keys` takes the keyboard away from the console
     (`/dev/input0`) and prints the code and the edge of every event it reads,
     so the test reads the key *as a key* - the code of `a` and its press and
     release - and not only the letter the shell echoed.

The machine is the one of the smoke test of the port (the same image of the
disk, the same kernel), so a machine without a controller still boots: the
driver says nothing and the PS/2 keeps its keys.

    tools/x86_usb_smoke.py [--image PATH] [--timeout SECONDS] [--verbose]

The exit status is 0 when the boot, the characters and the events are there
and 1 otherwise; 2 when the image or QEMU is missing.
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

PROMPT = "microos> "

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")

# The keys of one line of the shell, in the names of the monitor of QEMU. The
# names of the letters are the letters, the space is `spc` and Enter is `ret`;
# `shift-u` is the capital that proves the modifier of the report.
LINE_ECHO = ["e", "c", "h", "o", "spc", "shift-u", "s", "b", "ret"]

# The same with a letter that the Backspace takes back: the line the shell
# runs is `echo ab`, and the `x` is in the echo of the typing and not in the
# answer.
LINE_BACKSPACE = ["e", "c", "h", "o", "spc", "a", "b", "x", "backspace", "ret"]

# The line that runs the reader of the events, and the two keys it reads. The
# program is a file of /bin, so the shell has to read it from the disk of the
# machine - the door of the system call is the same one the smoke test of the
# port walks.
LINE_KEYS = [
    "e", "x", "e", "c", "spc", "slash", "b", "i", "n", "slash",
    "k", "e", "y", "s", "ret",
]

# The code of `a` and of Enter in the contract of kernel/input.h, and the two
# values of an edge.
KEY_A = 1
KEY_ENTER = 37
VALUE_RELEASE = 0
VALUE_PRESS = 1


class Machine:
    """The emulated machine, its serial port and the monitor of QEMU."""

    def __init__(self, loader: str, kernel: str, image: str, workdir: str) -> None:
        self.serial_path = os.path.join(workdir, "serial.sock")
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.transcript = b""

        command = [
            "qemu-system-x86_64",
            "-kernel",
            loader,
            "-initrd",
            kernel,
            "-m",
            "256",
            "-no-reboot",
            # The point of the test: a machine whose firmware has no PS/2
            # controller at all. The keyboard of the person is the device of
            # the xHCI below and nothing else.
            "-machine",
            "pc,i8042=off",
            "-device",
            "qemu-xhci,id=xhci",
            "-device",
            "usb-kbd,bus=xhci.0",
            "-drive",
            f"file={image},format=raw,if=ide,index=0,cache=writeback",
            "-display",
            "none",
            "-serial",
            f"unix:{self.serial_path},server=on,wait=off",
            "-monitor",
            f"unix:{self.monitor_path},server=on,wait=off",
        ]

        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.serial = self._connect(self.serial_path)
        self.monitor = self._connect(self.monitor_path)
        self._drain_monitor()

    @staticmethod
    def _connect(path: str, timeout: float = 30.0) -> socket.socket:
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            if os.path.exists(path):
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    sock.connect(path)
                except OSError:
                    sock.close()
                else:
                    sock.settimeout(0.2)
                    return sock
            time.sleep(0.05)

        raise RuntimeError(f"no socket at {path}: QEMU did not start")

    @staticmethod
    def _read(sock: socket.socket, window: float) -> bytes:
        chunk = b""
        sock.settimeout(max(window, 0.01))
        try:
            part = sock.recv(4096)
            while part:
                chunk += part
                if len(chunk) > 65536:
                    break
                part = sock.recv(4096)
        except (socket.timeout, BlockingIOError):
            pass
        return chunk

    def _drain_monitor(self, window: float = 1.0) -> str:
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def read_serial(self) -> str:
        self.transcript += self._read(self.serial, 0.01)
        return self.transcript.decode("utf-8", errors="replace")

    def wait_serial(self, needle: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.read_serial():
                return True
            time.sleep(0.05)
        return False

    def monitor_command(self, command: str, window: float = 2.0) -> str:
        self.monitor.sendall((command + "\n").encode("ascii"))
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def press(self, names: list, delay: float = 0.06) -> None:
        """Press the keys of a line, one at a time, with the pace of a person.

        The controller reports eight bytes of state and the driver reads the
        difference of two reports: two keys of the same report would be one
        change and not two, which is what a person produces and what the
        monitor does with one `sendkey` a time.
        """
        for name in names:
            self.monitor_command(f"sendkey {name}")
            time.sleep(delay)

    def prompts(self) -> int:
        return self.read_serial().count(PROMPT)

    def answer(self, lines: list, timeout: float) -> str:
        """Type a line and return what the shell printed between the prompts.

        The count of the prompts is taken before the line goes out: the prompt
        of the line is the mark of its end, and the text between the two is
        the answer of the command (the echo of the typing included).
        """
        count = self.prompts()
        start = self.read_serial().rfind(PROMPT)
        self.press(lines)
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            text = self.read_serial()
            if text.count(PROMPT) > count:
                end = text.find(PROMPT, start + len(PROMPT))
                return text[start + len(PROMPT) : end if end >= 0 else len(text)]
            time.sleep(0.05)

        return ""

    def close(self) -> None:
        for sock in (self.serial, self.monitor):
            try:
                sock.close()
            except OSError:
                pass

        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

        if self.process.stdout is not None:
            self.process.stdout.close()


def run(loader: str, kernel: str, image: str, timeout: float) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    workdir = tempfile.mkdtemp(prefix="microos-usb-")
    try:
        machine = Machine(loader, kernel, image, workdir)
    except (OSError, RuntimeError) as error:
        expect("the machine of the USB starts", False, str(error))
        return checks

    try:
        booted = machine.wait_serial(PROMPT, timeout)
        text = machine.read_serial()

        # --- the controller and the device
        expect(
            "the machine boots on a keyboard of the USB",
            booted,
            text[-600:],
        )
        expect(
            "the boot finds the controller of the USB 3.0",
            "xhci: controller at" in text,
            "the port did not find the xHCI function of the PCI bus",
        )
        match = re.search(r"xhci: controller at [0-9a-f]+, version \d+\.\d+, (\d+) port", text)
        expect(
            "the controller reports its ports",
            match is not None and int(match.group(1)) >= 1,
            "no line of the controller in the boot",
        )
        expect(
            "the boot names the keyboard of the boot protocol",
            re.search(r"usb: keyboard [0-9a-f]{4}:[0-9a-f]{4} on port \d+, slot \d+", text)
            is not None,
            "the enumeration of the HID device did not finish",
        )
        expect(
            "the keyboard is a HID device of eight-byte reports",
            re.search(r"usb: keyboard .*, \d+ bytes", text) is not None and "8 bytes" in text,
            "the report of the keyboard is not the one of the boot protocol",
        )

        # --- the characters, through the console
        echo = machine.answer(LINE_ECHO, timeout)
        lines = [line.strip() for line in echo.replace("\r", "").split("\n")]
        expect(
            "a line typed on the USB keyboard reaches the shell",
            "echo Usb" in lines,
            echo[-300:],
        )
        expect(
            "the shell runs the line of the USB keyboard",
            "Usb" in lines,
            echo[-300:],
        )
        expect(
            "the Shift of the USB report is the capital of the console",
            "Usb" in lines and "usb" not in [line for line in lines if line != "echo Usb"],
            echo[-300:],
        )

        edited = machine.answer(LINE_BACKSPACE, timeout)
        edited_lines = [line.strip() for line in edited.replace("\r", "").split("\n")]
        expect(
            "the Backspace of the USB report edits the line",
            any(line.startswith("echo abx") for line in edited_lines),
            edited[-300:],
        )
        expect(
            "the letter taken back is not in the answer",
            "ab" in edited_lines and "abx" not in edited_lines,
            edited[-300:],
        )

        # --- the events, through /dev/input0
        #
        # The line is typed and not answered: the program takes the keyboard
        # away from the console and waits for the events of the person, so a
        # reader that waited for the prompt of the shell would wait until the
        # program gave up. The keys go out once the line is on the console and
        # the program has had the moment to open the device.
        machine.press(LINE_KEYS)
        opened = machine.wait_serial("exec /bin/keys", timeout)
        time.sleep(0.4)
        machine.press(["a"], delay=0.12)
        machine.press(["ret"], delay=0.12)
        machine.wait_serial("keys: done", timeout)
        events = machine.read_serial()

        expect(
            "the reader of the events is a program of the machine",
            opened and "exec: /bin/keys:" not in events,
            events[-400:],
        )

        expect(
            "the event of the key `a` comes out of /dev/input0",
            re.search(rf"keys: \d+ {KEY_A} {VALUE_PRESS}\r?$", events, re.MULTILINE) is not None,
            events[-400:],
        )
        expect(
            "the release of the key `a` is an event of its own",
            re.search(rf"keys: \d+ {KEY_A} {VALUE_RELEASE}\r?$", events, re.MULTILINE) is not None,
            events[-400:],
        )
        expect(
            "the Enter of the USB keyboard is the key of the machine",
            re.search(rf"keys: \d+ {KEY_ENTER} {VALUE_PRESS}\r?$", events, re.MULTILINE)
            is not None,
            events[-400:],
        )
        expect(
            "the reader gives the keyboard back to the shell",
            "keys: done with" in events,
            events[-400:],
        )

        # --- the machine lives after the capture of the keyboard
        alive = machine.answer(["u", "n", "a", "m", "e", "ret"], timeout)
        expect(
            "the shell answers after the keyboard was given back",
            re.search(r"MicroOS \d+\.\d+\.\d+", alive) is not None,
            alive[-300:],
        )
    finally:
        machine.close()

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the keyboard of the USB of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the disk image of the machine")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds for one wait")
    parser.add_argument("--verbose", action="store_true", help="print the boot of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel, args.image):
        if not os.path.isfile(path):
            print(f"usb-smoke: {path} is missing (make x86-disk)", file=sys.stderr)
            return 2

    try:
        checks = run(loader, kernel, args.image, args.timeout)
    except FileNotFoundError as error:
        print(f"usb-smoke: {error}", file=sys.stderr)
        return 2

    failed = [item for item in checks if not item[1]]
    if args.verbose or failed:
        for name, ok, detail in checks:
            print(f"{'ok  ' if ok else 'FAIL'} {name}")
            if not ok and detail:
                print(f"     {detail}")

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
