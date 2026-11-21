#!/usr/bin/env python3
"""Micro OS — the window of the x86-64 port: the VGA text and the keyboard.

The smoke test of the port (tools/x86_smoke.py) drives the machine through
COM1. This one looks at the other console: the 80x25 text mode of the VGA at
0xB8000, where the kernel draws the same bytes it sends to the serial port,
and the PS/2 keyboard, which is the only way a person who runs the machine
in a window can type at all.

There is no display here, and none is needed: QEMU with -display none still
has the video memory, and its monitor can read it (pmemsave) and press keys
(sendkey). The test proves the two halves of the window independently of the
graphics of the host:

  1. after the boot the cells at 0xB8000 carry the banner and the prompt
     "microos> ", so the screen was cleared, written and scrolled by the
     kernel and not by the firmware; the video device draws that memory as a
     720x400 picture (the text mode of the 80x25 cells), and the cursor of
     the CRTC is read back out of the registers 14 and 15 and compared with
     the cell where the prompt ends;
  2. a command typed through sendkey - "fsck", with a wrong letter erased by
     Backspace on the way - reaches the shell through IRQ1, runs, and its
     answer "the volume is consistent" is in the video memory, which proves
     the interrupt, the scancode map and the editing keys at once.

    tools/x86_vga_smoke.py [--timeout SECONDS] [--display none|gtk] [--verbose]

The exit status is 0 when both held and 1 otherwise; 2 when the image or QEMU
is missing. The test needs no X and no window, so a machine of a build farm
can run it; it is kept out of `make ci` because the monitor of QEMU is a tool
of its own and the serial smoke test already guards the boot.

With `--display gtk` the same cards are checked against the real window of
QEMU instead of its memory: the screendump is then the picture of the window
of the machine, drawn by the display backend, and a person can watch the
keys arrive as they are typed.
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

# The prompt of the shell: it is written in one call, so it is never split by
# the log of another task, and the boot is over when it is on the screen.
PROMPT = "microos> "

# The text screen of the mode 3 of the VGA: 80 columns, 25 rows, two bytes a
# cell (the character and the colour), and that is the whole of the memory
# the test reads.
VGA_BASE = 0xB8000
VGA_COLUMNS = 80
VGA_ROWS = 25
VGA_BYTES = VGA_COLUMNS * VGA_ROWS * 2

# The answer of the command the keyboard types: it is the checker of the
# filesystem, and the volume it checks is the one the boot mounted.
ANSWER = "the volume is consistent"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")


class Machine:
    """The emulated machine, its serial port and its monitor.

    Both devices are unix sockets: the serial port is read as a stream and
    the monitor is a socket that takes one command at a time, so nothing has
    to be parsed out of the console of QEMU itself.
    """

    def __init__(self, loader: str, kernel: str, workdir: str, display: str = "none") -> None:
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
            # The window is the subject of the test and not its instrument: by
            # default no display of QEMU is opened, the video memory is still
            # there and the monitor reads it. `--display gtk` opens the real
            # window, and the same cards below are then the picture of it.
            "-display",
            display,
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
    def _connect(path: str, timeout: float = 10.0) -> socket.socket:
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

    def _read(self, sock: socket.socket, window: float) -> bytes:
        """Whatever the socket has, waiting at most `window` seconds."""
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

    def _drain_monitor(self, window: float = 0.2) -> str:
        """The banner and the prompt of the monitor, which are not a command."""
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def read_serial(self) -> str:
        """The serial port so far: the boot log and the echo of the shell."""
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
        """One command of the human monitor, answered with its own prompt."""
        self.monitor.sendall((command + "\n").encode("ascii"))
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def screen(self, directory: str) -> str:
        """The text of the VGA, read out of the memory of the machine."""
        path = os.path.join(directory, "vga.bin")
        if os.path.exists(path):
            os.unlink(path)

        self.monitor_command(f'pmemsave 0x{VGA_BASE:x} {VGA_BYTES} "{path}"')

        # The dump is written before the monitor answers with its prompt, but
        # a file of a slow machine may still be in the page cache: the wait is
        # a courtesy and the size is the proof.
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if os.path.exists(path) and os.path.getsize(path) == VGA_BYTES:
                break
            time.sleep(0.02)

        try:
            with open(path, "rb") as handle:
                blob = handle.read()
        except FileNotFoundError:
            return ""

        lines = []
        for row in range(VGA_ROWS):
            cells = blob[row * VGA_COLUMNS * 2 : (row + 1) * VGA_COLUMNS * 2]
            text = "".join(
                chr(cells[index]) if 0x20 <= cells[index] < 0x7F else " "
                for index in range(0, len(cells), 2)
            )
            lines.append(text.rstrip())
        return "\n".join(lines)

    def keys(self, names: list) -> None:
        """Press the keys of the monitor: the device interrupts the kernel."""
        for name in names:
            self.monitor_command(f"sendkey {name}")
            time.sleep(0.05)

    def cursor(self) -> int:
        """The position of the cursor of the hardware, out of the CRTC.

        The controller keeps the cursor in the registers 14 and 15 (the low
        and the high byte of one number), addressed through the index port:
        the monitor can read an I/O port, so the position the kernel wrote is
        a number this test can compare with its own idea of the screen.
        """
        position = 0

        for shift, register in ((0, 0x0F), (8, 0x0E)):
            self.monitor_command(f"o /b 0x3d4 0x{register:02x}")
            answer = self.monitor_command("i /b 0x3d5")
            found = re.findall(r"portb\[0x03d5\] = 0x([0-9a-fA-F]+)", answer)
            if len(found) != 1:
                return -1
            position |= int(found[0], 16) << shift

        return position

    def drawn(self, directory: str) -> tuple:
        """The size and the ink of a screen dump.

        screendump asks the video device to draw its screen, which is the
        picture a window would show: the text memory above is what the kernel
        writes, and this is what the device makes of it. The text mode of the
        80x25 is 720x400 pixels on the standard VGA, and a screen with text
        has thousands of bytes that are not black.
        """
        path = os.path.join(directory, "screen.ppm")
        if os.path.exists(path):
            os.unlink(path)

        self.monitor_command(f'screendump "{path}"')

        deadline = time.monotonic() + 5.0
        size = 0
        while time.monotonic() < deadline:
            if os.path.exists(path):
                current = os.path.getsize(path)
                if current > 0 and current == size:
                    break
                size = current
            time.sleep(0.05)

        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except FileNotFoundError:
            return (0, 0, 0)

        # The header of a binary PPM: P6, the width and the height, and the
        # largest value of a channel. The pixels follow the fourth line.
        lines = data.split(b"\n", 3)
        try:
            width, height = (int(value) for value in lines[1].split())
        except (IndexError, ValueError):
            return (0, 0, 0)

        pixels = lines[3] if len(lines) > 3 else b""
        return (width, height, sum(1 for byte in pixels if byte))

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


def run(loader: str, kernel: str, timeout: float, verbose: bool, display: str) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-vga-") as workdir:
        machine = Machine(loader, kernel, workdir, display)

        try:
            # --- the screen after the boot
            booted = machine.wait_serial(PROMPT, timeout)
            expect(
                "the serial console is up as before",
                booted,
                "the shell never printed its prompt",
            )

            boot = machine.screen(workdir)
            if verbose:
                print("--- the screen after the boot ---")
                print(boot)

            expect("the kernel writes to the video memory", "MicroOS" in boot)
            expect("the prompt is on the screen", PROMPT.strip() in boot)
            # A screen nobody cleared would carry the line of the firmware at
            # the top; the kernel clears it before the banner.
            expect(
                "the screen is the kernel's own",
                "SeaBIOS" not in boot and "iPXE" not in boot,
            )

            # The device, not only the memory: the picture a window would
            # show. The 720x400 pixels are the 80x25 cells of the text mode at
            # nine by sixteen, and the ink is the grey of the characters.
            width, height, ink = machine.drawn(workdir)
            expect(
                "the video mode is the text mode of 80x25",
                (width, height) == (720, 400),
                f"the screenshot is {width}x{height}",
            )
            expect(
                "the device draws the text",
                ink > 1000,
                f"only {ink} byte(s) of the screenshot are not black",
            )

            # The cursor of the hardware is where the prompt ends: the shell
            # writes "microos> " and waits, so the register of the CRTC and
            # the screen of the kernel have to agree on the same cell.
            expected = -1
            for index, line in enumerate(boot.split("\n")):
                column = line.rfind(PROMPT.strip())
                if column >= 0:
                    # The prompt and the space after it are the cells the
                    # shell wrote last: the cursor sits after both.
                    expected = index * VGA_COLUMNS + column + len(PROMPT)

            position = machine.cursor()
            expect(
                "the cursor follows the text",
                position == expected and expected >= 0,
                f"the CRTC says {position}, the prompt ends at {expected}",
            )

            # --- the keyboard
            #
            # The wrong letter and the Backspace are part of the test: the
            # shell echoes every key it takes, and the echo of a backspace is
            # "\b \b", which the screen has to erase like a terminal does. A
            # keyboard without the editing keys would type "fscxk" and the
            # shell would answer "unknown command", which the answer below
            # would not find.
            machine.keys(["f", "s", "c", "x", "backspace", "k", "ret"])

            answered = machine.wait_serial(ANSWER, timeout)
            expect(
                "the keyboard reaches the shell",
                answered,
                f"the serial console never said {ANSWER!r}",
            )

            window = machine.screen(workdir)
            if verbose:
                print("--- the screen after fsck ---")
                print(window)

            expect("the keys are echoed on the screen", "fsck" in window)
            expect("the answer is on the screen", ANSWER in window)
            # The shell prints the prompt of the next line when the command is
            # done: the screen that carries the answer carries the prompt too.
            expect("the shell waits for the next line", PROMPT.strip() in window)
        finally:
            machine.close()

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the VGA and keyboard test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=20.0, help="the seconds the machine gets")
    parser.add_argument("--verbose", action="store_true", help="print the screen of the machine")
    parser.add_argument(
        "--display",
        default="none",
        help="the display of QEMU: none (the default, no X needed) or gtk for the real window",
    )
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    try:
        checks = run(loader, kernel, args.timeout, args.verbose, args.display)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    except RuntimeError as error:
        print(f"{error}", file=sys.stderr)
        return 2

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the window test of the x86-64 port (VGA text, PS/2 keyboard)")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)
    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
