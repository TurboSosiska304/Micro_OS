#!/usr/bin/env python3
"""Micro OS — the screen of the x86-64 port: the framebuffer, the text mode and
the keyboard.

The smoke test of the port (tools/x86_smoke.py) drives the machine through
COM1. This one looks at the screen: the framebuffer of the VGA of QEMU
(ADR 0023, 1280x800, 16 bits per pixel) and the text mode of 80x25 that is the
fallback of a machine whose Bochs VBE interface does not answer. The keyboard
is the other half of the window: it is the only way a person who runs the
machine in a window can type at all.

There is no display here, and none is needed: QEMU with -display none still
has its video device, and its monitor reads the picture (screendump) and
presses keys (sendkey). The test boots the machine twice and checks the two
screens of ADR 0023:

  1. the framebuffer (-vga std, the default): the boot says the screen is the
     framebuffer of 1280x800 by 128x57 cells, the screenshot is a PPM of
     1280x800, and the cells of the first line are the glyphs of the 5x7 font
     at twice their size - the letter 'M' of "MicroOS" is compared with the
     bitmap of the font, pixel by pixel, so the test reads the text and not
     only the ink. The cursor is the one cell that is entirely ink. A command
     typed through the keyboard reaches the shell (IRQ1, the scancode map and
     the editing keys), its answer is on the serial console and the picture
     grows below the prompt, and `config set console.theme light` repaints the
     cells in the colours of /etc/console: the paper of the picture becomes
     the light grey of the VGA and the glyphs become black;
  2. the text mode (-vga cirrus, a card without the Bochs VBE interface): the
     boot says it kept the text mode, the screen is read out of the memory at
     0xB8000 with pmemsave as it always was, the picture of the device is
     720x400, the cursor of the CRTC is where the prompt ends, and the same
     typed command and theme reach the text cells and their attribute.

    tools/x86_vga_smoke.py [--timeout SECONDS] [--display none|gtk] [--verbose]

The exit status is 0 when both screens held and 1 otherwise; 2 when the image
or QEMU is missing. The test needs no X and no window, so a machine of a build
farm can run it; it is kept out of `make ci` because the monitor of QEMU is a
tool of its own and the serial smoke test already guards the boot.

With `--display gtk` the same cards are checked against the real window of
QEMU instead of its memory: the screendump is then the picture of the window
of the machine, drawn by the display backend, and a person can watch the keys
arrive as they are typed.
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
# the fallback test reads.
VGA_BASE = 0xB8000
VGA_COLUMNS = 80
VGA_ROWS = 25
VGA_BYTES = VGA_COLUMNS * VGA_ROWS * 2

# The picture of ADR 0023 and the cell of the 5x7 font at twice its size.
VBE_WIDTH = 1280
VBE_HEIGHT = 800
CELL_WIDTH = 10
CELL_HEIGHT = 14

# The two colours of the console, as the display makes them of RGB565 0xAD55
# (the light grey of the VGA) and 0x0000. The theme of /etc picks which is the
# ink and which is the paper; the inversion swaps the two.
GREY = (173, 170, 173)
BLACK = (0, 0, 0)

# The glyphs the test reads out of the picture, from components/font/src/
# font.c: five columns, the lowest bit of a column is the top row. A change of
# the font is a change of the picture, and the test says so - that is the point
# of a screenshot in the smoke.
GLYPHS = {
    "M": (0x7F, 0x02, 0x0C, 0x02, 0x7F),
    "I": (0x00, 0x41, 0x7F, 0x41, 0x00),
    "C": (0x3E, 0x41, 0x41, 0x41, 0x22),
    "R": (0x7F, 0x09, 0x19, 0x29, 0x46),
    "O": (0x3E, 0x41, 0x41, 0x41, 0x3E),
    "S": (0x46, 0x49, 0x49, 0x49, 0x31),
}

# The answer of the command the keyboard types: it is the checker of the
# filesystem, and the volume it checks is the one the boot mounted.
ANSWER = "the volume is consistent"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")


class Picture:
    """A PPM of the screen as a grid of pixels, and the cells of the font.

    The screenshot of QEMU is a binary PPM (P6): a header of three values and
    then three bytes a pixel, from the top left. A cell of the console is ten
    pixels wide and fourteen tall, and the glyph inside it is the 5x7 font at
    twice its size - so a cell is read as the bitmap of a letter and compared
    with the table of the font.
    """

    def __init__(self, width: int, height: int, pixels: bytes) -> None:
        self.width = width
        self.height = height
        self.pixels = pixels

    def pixel(self, x: int, y: int) -> tuple:
        index = (y * self.width + x) * 3
        return tuple(self.pixels[index : index + 3])

    def ink_pixels(self) -> int:
        """The bytes of the picture that are not black: an ink that a cell has."""
        return sum(1 for byte in self.pixels if byte)

    def lowest_ink_row(self) -> int:
        """The last line of the picture that has a pixel that is not black."""
        stride = self.width * 3

        for y in range(self.height - 1, -1, -1):
            if any(self.pixels[y * stride : (y + 1) * stride]):
                return y
        return -1

    def cell_is(self, row: int, column: int, glyph: tuple, ink: tuple, paper: tuple) -> bool:
        """The cell is the letter: every pixel of the 2x2 block of a bit.

        A row of the cell is compared as bytes and not pixel by pixel: the
        glyph at twice its size is the ten pixels of the five columns, and the
        row of the picture above the cell has to be exactly that.
        """
        x0 = column * CELL_WIDTH
        y0 = row * CELL_HEIGHT

        if x0 + CELL_WIDTH > self.width or y0 + CELL_HEIGHT > self.height:
            return False

        for cy in range(7):
            expected = b""
            for bits in glyph:
                expected += bytes(ink if bits & (1 << cy) else paper) * 2

            for sy in range(2):
                start = ((y0 + cy * 2 + sy) * self.width + x0) * 3
                if self.pixels[start : start + CELL_WIDTH * 3] != expected:
                    return False

        return True

    def cell_is_ink(self, row: int, column: int, ink: tuple) -> bool:
        """The cell is one block of the colour: what the cursor is."""
        x0 = column * CELL_WIDTH
        y0 = row * CELL_HEIGHT
        expected = bytes(ink) * CELL_WIDTH

        for y in range(y0, y0 + CELL_HEIGHT):
            start = (y * self.width + x0) * 3
            if self.pixels[start : start + CELL_WIDTH * 3] != expected:
                return False

        return True

    def cursor_cells(self, ink: tuple) -> int:
        """How many cells are one block of the ink: the cursor of the screen.

        The console draws the cursor as the reverse of a cell - the ink fills
        the ten by fourteen pixels and the letter, when there is one, is in
        the paper - so a cell that is all ink is the cursor and nothing else
        the console draws is.
        """
        count = 0

        for row in range(self.height // CELL_HEIGHT):
            for column in range(self.width // CELL_WIDTH):
                if self.cell_is_ink(row, column, ink):
                    count += 1

        return count


class Machine:
    """The emulated machine, its serial port and its monitor.

    Both devices are unix sockets: the serial port is read as a stream and the
    monitor is a socket that takes one command at a time, so nothing has to be
    parsed out of the console of QEMU itself.
    """

    def __init__(self, loader: str, kernel: str, workdir: str, vga: str, display: str = "none") -> None:
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
            # The card of the machine is the subject of the test and not its
            # instrument: by default no display of QEMU is opened, the video
            # memory and its picture are still there and the monitor reads
            # them with pmemsave and screendump. `--display gtk` opens the real
            # window, and the same cards below are then the picture of it.
            "-display",
            display,
            # `-vga std` is the VGA of QEMU with the Bochs VBE interface
            # (1234:1111, ADR 0023); `-vga cirrus` is a card with a text mode
            # and no such interface, which is the fallback of the port.
            "-vga",
            vga,
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

    def type_line(self, line: str) -> None:
        """A line of the shell through the serial port, which is a console too."""
        self.serial.sendall((line + "\n").encode("ascii"))

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

    def screen_text(self, directory: str) -> str:
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

    def attributes(self, directory: str) -> list:
        """The attribute byte of every cell of the text screen."""
        path = os.path.join(directory, "vga.bin")

        try:
            with open(path, "rb") as handle:
                blob = handle.read()
        except FileNotFoundError:
            return []

        return list(blob[1::2])

    def picture(self, directory: str) -> Picture | None:
        """The screen as a picture: what the device draws, not what it holds.

        screendump asks the video device to draw its screen, which is the
        picture a window would show: the text memory of the fallback and the
        framebuffer of the new screen are two ways to the same call.
        """
        path = os.path.join(directory, "screen.ppm")
        if os.path.exists(path):
            os.unlink(path)

        self.monitor_command(f'screendump "{path}"')

        deadline = time.monotonic() + 5.0
        size = -1
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
            return None

        # The header of a binary PPM: P6, the width and the height, and the
        # largest value of a channel. The pixels follow the fourth line.
        lines = data.split(b"\n", 3)
        try:
            width, height = (int(value) for value in lines[1].split())
        except (IndexError, ValueError):
            return None

        return Picture(width, height, lines[3] if len(lines) > 3 else b"")

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


def type_fsck(machine: Machine) -> None:
    """Type `fsck` with a wrong letter erased on the way.

    The wrong letter and the Backspace are part of the test: the shell echoes
    every key it takes, and the echo of a backspace is "\\b \\b", which the
    screen has to erase like a terminal does. A keyboard without the editing
    keys would type "fscxk" and the shell would answer "unknown command",
    which the answer below would not find.
    """
    machine.keys(["f", "s", "c", "x", "backspace", "k", "ret"])


def run_framebuffer(loader: str, kernel: str, timeout: float, verbose: bool, display: str) -> list:
    """The screen of ADR 0023: the framebuffer of the Bochs VBE interface."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"framebuffer: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-fb-") as workdir:
        machine = Machine(loader, kernel, workdir, "std", display)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the serial console is up as before", booted, "the shell never printed its prompt")

            serial = machine.read_serial()
            expect(
                "the port chooses the framebuffer",
                "x86-64: the screen is the framebuffer of 1280x800, 16 bpp, 128x57 cells" in serial,
                "the boot did not name the framebuffer of ADR 0023",
            )

            picture = machine.picture(workdir)
            expect("the device has a picture", picture is not None, "screendump wrote no PPM")

            if picture is not None:
                expect(
                    "the picture is the mode of ADR 0023",
                    (picture.width, picture.height) == (VBE_WIDTH, VBE_HEIGHT),
                    f"the screenshot is {picture.width}x{picture.height}",
                )
                # The first line of the screen is the banner of the kernel,
                # "MicroOS ...". The console draws it in the 5x7 font at twice
                # its size and the font has no lowercase, so the cells are the
                # letters below - and a cell is compared with the bitmap of the
                # font, not with a count of ink.
                expect(
                    "the cells of the first line are the glyphs of the font",
                    all(
                        picture.cell_is(0, index, GLYPHS[letter], GREY, BLACK)
                        for index, letter in enumerate("MICROOS")
                    ),
                    "a cell of the first line is not the glyph of the font",
                )
                expect(
                    "the theme of the boot is grey on black",
                    picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) == BLACK,
                    f"the paper is {picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2)}",
                )
                cursors = picture.cursor_cells(GREY)
                expect(
                    "the console draws the cursor",
                    cursors == 1,
                    f"{cursors} cell(s) of the picture are all ink",
                )
                expect(
                    "the device draws the text",
                    picture.ink_pixels() > 10000,
                    f"only {picture.ink_pixels()} pixel(s) of the picture are not black",
                )

            # --- the keyboard, and the picture it changes
            ink_before = picture.lowest_ink_row() if picture is not None else -1
            type_fsck(machine)

            answered = machine.wait_serial(ANSWER, timeout)
            expect("the keyboard reaches the shell", answered, f"the serial console never said {ANSWER!r}")

            picture = machine.picture(workdir)
            expect(
                "the answer reaches the picture",
                picture is not None and picture.lowest_ink_row() > ink_before,
                "the screen did not grow below the prompt after the command",
            )

            # --- the colours of /etc/console (ADR 0021) on the new screen
            machine.type_line("config set console.theme light")
            themed = machine.wait_serial("console: theme light, invert 0", timeout)
            expect(
                "config set repaints the framebuffer",
                themed,
                "the console did not apply the theme of /etc",
            )

            picture = machine.picture(workdir)
            if picture is not None:
                expect(
                    "the light theme is black on light grey",
                    picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) == GREY
                    and picture.pixel(0, VBE_HEIGHT - 2) == GREY,
                    f"the paper is {picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2)}",
                )
                expect(
                    "the text of the boot is read in the new colours",
                    picture is not None
                    and picture.cell_is(0, 0, GLYPHS["M"], BLACK, GREY),
                    "the cell of the banner is not the glyph in the colours of the theme",
                )
                if verbose:
                    print("--- the framebuffer after the theme ---")
                    print(f"{picture.width}x{picture.height}, {picture.ink_pixels()} pixel(s) of ink")
        finally:
            machine.close()

    return checks


def run_text(loader: str, kernel: str, timeout: float, verbose: bool, display: str) -> list:
    """The fallback of ADR 0023: the text mode of the VGA, 80x25.

    The card is a Cirrus, which has a text mode and no Bochs VBE interface, so
    the port keeps the screen the firmware left and the whole console of 1.1
    is the one a machine without a picture has.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"text mode: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-text-") as workdir:
        machine = Machine(loader, kernel, workdir, "cirrus", display)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the serial console is up as before", booted, "the shell never printed its prompt")
            expect(
                "the port keeps the text mode",
                "x86-64: the screen is the text mode of the VGA, 80x25" in machine.read_serial(),
                "the boot did not say it fell back to the text mode",
            )

            boot = machine.screen_text(workdir)
            if verbose:
                print("--- the text screen after the boot ---")
                print(boot)

            expect("the kernel writes to the video memory", "MicroOS" in boot)
            expect("the prompt is on the screen", PROMPT.strip() in boot)
            # A screen nobody cleared would carry the line of the firmware at
            # the top; the kernel clears it before the banner.
            expect("the screen is the kernel's own", "SeaBIOS" not in boot and "iPXE" not in boot)

            picture = machine.picture(workdir)
            expect(
                "the device has the text picture of 80x25",
                picture is not None and (picture.width, picture.height) == (720, 400),
                f"the screenshot is {picture.width}x{picture.height}"
                if picture is not None
                else "screendump wrote no PPM",
            )
            expect(
                "the device draws the text",
                picture is not None and picture.ink_pixels() > 1000,
                f"only {picture.ink_pixels()} pixel(s) are not black"
                if picture is not None
                else "no picture",
            )

            # The cursor of the hardware is where the prompt ends: the shell
            # writes "microos> " and waits, so the register of the CRTC and the
            # screen of the kernel have to agree on the same cell.
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
            type_fsck(machine)

            answered = machine.wait_serial(ANSWER, timeout)
            expect("the keyboard reaches the shell", answered, f"the serial console never said {ANSWER!r}")

            window = machine.screen_text(workdir)
            if verbose:
                print("--- the text screen after fsck ---")
                print(window)

            expect("the keys are echoed on the screen", "fsck" in window)
            expect("the answer is on the screen", ANSWER in window)
            expect("the shell waits for the next line", PROMPT.strip() in window)

            # --- the colours of /etc/console (ADR 0021) on the text screen
            machine.type_line("config set console.theme light")
            themed = machine.wait_serial("the attribute of the screen 112", timeout)
            expect(
                "the theme reaches the attribute of the VGA",
                themed,
                "the console did not put black on light grey on the text screen",
            )

            # The attribute of every cell is the pair, and the characters stay
            # where they are: the line that was on the glass is read again.
            machine.screen_text(workdir)
            attributes = machine.attributes(workdir)
            expect(
                "every cell carries the new attribute",
                attributes and all(value == 0x70 for value in attributes),
                f"{len(set(attributes))} attribute(s) on the screen",
            )
            expect(
                "the text is still there after the repaint",
                "fsck" in machine.screen_text(workdir),
            )
        finally:
            machine.close()

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the screen and keyboard test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=20.0, help="the seconds each boot gets")
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
        checks = run_framebuffer(loader, kernel, args.timeout, args.verbose, args.display)
        checks += run_text(loader, kernel, args.timeout, args.verbose, args.display)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    except RuntimeError as error:
        print(f"{error}", file=sys.stderr)
        return 2

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the screen test of the x86-64 port (framebuffer, VGA text, PS/2 keyboard)")
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
