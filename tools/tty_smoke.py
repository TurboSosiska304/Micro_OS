#!/usr/bin/env python3
"""Micro OS — `make tty-smoke`: a full-screen editor, typed at the machine.

The second half of the gate of ADR 0051. `make ports-smoke` proves that a
program of somebody else builds with this system's SDK without a line of its
source being edited and that its answers are the answers of the release; a
transcript cannot type at an editor, because an editor reads one key at a time
and never a line. This test drives the keyboard of the machine itself.

The run is the promise of the lane in five commands:

1. `ttyprobe` says the console is a terminal and that it is canonical;
2. `kilo /tmp/k.txt` opens a file that is not there and draws its screen;
3. the keys of a person go to it - a line, Ctrl+S to save, Ctrl+Q to leave -
   and the shell is waiting again;
4. `cat /tmp/k.txt` prints the line that was typed;
5. `ttyprobe` says the console is canonical again, which is the mode that came
   back with the program that changed it (`k_tty_task_exit`).

    tools/tty_smoke.py [--build build-ports] [--x86 build-x86]
                       [--timeout SECONDS] [--keep] [--verbose]

The volume is the one of `make ports-smoke` (`tools/ports_smoke.py` builds it),
so the editor of the run is the image of the gate and not a second copy. The
one half of the gate this file does not repeat is the sha256 of the sources:
`make ports-smoke` refuses to boot a tree whose sources are not the release.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)

sys.path.insert(0, TOOLS)
from x86_smoke import PROMPT, machine_argv  # noqa: E402
from ports_smoke import (  # noqa: E402
    OWN_PROGRAMS,
    SCREEN_PROGRAMS,
    build_image,
    upstream_checks,
)

# The commands of the run: the two of the terminal, the editor, the file and
# the terminal again. A command here and a command there in `drive()`: the
# editor is driven by keys and not by a line, so it has no place in a list of
# lines.
BEFORE = "exec /bin/ports/ttyprobe"
EDIT = "exec /bin/ports/kilo /tmp/k.txt"
READ = "exec /bin/cat /tmp/k.txt"
AFTER = "exec /bin/ports/ttyprobe"

# The line that is typed into the editor and the file it must leave behind.
TYPED = "hello kilo"

# The bytes of a terminal: Ctrl+S saves, Ctrl+Q leaves (the help line of the
# editor says so: "Ctrl-S = save | Ctrl-Q = quit").
CTRL_S = b"\x13"
CTRL_Q = b"\x11"


class Console:
    """The serial console of the machine: the bytes out, the keys in."""

    def __init__(self, loader: str, kernel: str, image: str, disk: str, timeout: float):
        self.process = subprocess.Popen(
            machine_argv(loader, kernel, image, disk),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        assert self.process.stdout is not None
        assert self.process.stdin is not None
        self.data = b""
        self.deadline = time.monotonic() + timeout

    def text(self) -> str:
        return self.data.decode("utf-8", errors="replace")

    def pump(self, seconds: float) -> None:
        """Read whatever the machine says for `seconds`."""
        import select

        end = time.monotonic() + seconds
        fd = self.process.stdout.fileno()

        while time.monotonic() < end:
            ready, _, _ = select.select([fd], [], [], min(0.1, end - time.monotonic()))
            if not ready:
                continue
            chunk = os.read(fd, 4096)
            if not chunk:
                return
            self.data += chunk

    def wait_more(self, needle: str, count: int, timeout: float) -> bool:
        """Wait until `needle` has appeared `count` times."""
        end = time.monotonic() + timeout

        while time.monotonic() < end:
            self.pump(0.1)
            if self.text().count(needle) >= count:
                return True

        return self.text().count(needle) >= count

    def send(self, data: bytes) -> None:
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def line(self, line: str) -> None:
        self.send((line + "\n").encode("ascii"))

    def keys(self, data: bytes, delay: float = 0.05) -> None:
        """Type a key at a time: the ring of the keyboard of the machine is
        not a file, and a burst is a burst that nobody promised to keep."""
        for byte in data:
            self.send(bytes([byte]))
            self.pump(delay)

    def close(self) -> None:
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

        while True:
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                break
            self.data += chunk

        self.process.stdout.close()
        self.process.stdin.close()


def block(text: str, command: str, occurrence: int = 0) -> str:
    """What the machine said between the echo of `command` and the next prompt.

    The same command is typed twice - the terminal before the editor and after
    it - so the occurrence is named and not guessed.
    """
    index = -1
    for _ in range(occurrence + 1):
        index = text.find(command, index + 1)
        if index < 0:
            return ""
    end = text.find(PROMPT, index)

    return text[index:] if end < 0 else text[index:end]


def line_is(text: str, word: str) -> bool:
    return re.search(r"(?m)^" + re.escape(word) + r"\r?$", text) is not None


def drive(console: Console) -> str:
    """The five commands of the run, and the keys of an editor between two."""
    prompts = 0

    if not console.wait_more(PROMPT, prompts + 1, 90):
        return console.text()
    prompts += 1

    console.line(BEFORE)
    if not console.wait_more(PROMPT, prompts + 1, 20):
        return console.text()
    prompts += 1

    console.line(EDIT)
    # The editor draws before it reads: the name of the file is in its status
    # bar, and the line of the help is the second half of the same row.
    console.wait_more("Ctrl-S", 1, 15)
    console.pump(0.3)

    console.keys(TYPED.encode("ascii"))
    console.keys(CTRL_S)
    console.pump(0.3)
    console.keys(CTRL_Q)

    # The end of a program: the port logs the status and the shell asks again.
    console.wait_more("the program exited with 0", 1, 20)
    if not console.wait_more(PROMPT, prompts + 1, 20):
        return console.text()
    prompts += 1

    console.line(READ)
    if not console.wait_more(PROMPT, prompts + 1, 20):
        return console.text()
    prompts += 1

    console.line(AFTER)
    console.wait_more(PROMPT, prompts + 1, 20)

    return console.text()


def check(output: str) -> list:
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    before = block(output, BEFORE)
    editor = block(output, EDIT)
    read = block(output, READ)
    after = block(output, AFTER, 1)

    # The boot is the machine of the release.
    expect("the volume of the machine is the disk", "the root volume is" in output)

    # --- the terminal, before the editor -----------------------------------
    expect("the console of the machine is a terminal", BEFORE in output, output[-400:])
    expect("a terminal is canonical before the editor",
           line_is(before, "tty: mode canonical=1 echo=1 isig=1 vmin=1 vtime=0"), before)
    expect("the screen has a size", re.search(r"(?m)^tty: size [0-9]+x[0-9]+\r?$", before)
           is not None, before)

    # --- the editor, driven by keys ----------------------------------------
    expect("the editor of somebody else starts", "Ctrl-S" in editor, editor)
    expect("the editor left with the status 0", "the program exited with 0" in editor, editor)
    expect("the editor wrote what was typed", TYPED in editor, editor)
    expect("the serial log of a screen is text and not escapes",
           "\x1b[" not in editor, editor)

    # --- the file it left ---------------------------------------------------
    expect("the file of the editor is read back", line_is(read, TYPED), read)
    expect("the file has one line", read.count(TYPED) == 1, read)

    # --- the terminal, after the editor -------------------------------------
    expect("the mode of the console came back with the program",
           line_is(after, "tty: mode canonical=1 echo=1 isig=1 vmin=1 vtime=0"), after)
    expect("the machine is still the machine of the release", PROMPT in output)

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="a full-screen editor at the machine")
    parser.add_argument("--build", default=os.path.join(ROOT, "build-ports"))
    parser.add_argument("--x86", default=os.path.join(ROOT, "build-x86"))
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--disk", default="ide", choices=("ide", "virtio"))
    parser.add_argument("--keep", action="store_true", help="keep the volume of the run")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    loader = os.path.join(args.x86, "loader32.elf")
    kernel = os.path.join(args.x86, "microos64.bin")
    programs = os.path.join(args.build, "programs")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-disk", file=sys.stderr)
            return 2

    # The sources of the gate are the release: the same check as ports-smoke,
    # because a run of an edited kilo would say nothing about this system.
    broken = [item for item in upstream_checks() if not item[1]]
    if broken:
        print("Micro OS — a full-screen editor at the machine (make tty-smoke)")
        print("=" * 72)
        for name, _, detail in broken:
            print(f"  FAIL  {name}")
            if detail:
                print(f"          {detail}")
        print("=" * 72)
        print("FAILED: the sources of tests/ports are not the release of the upstream")
        return 1

    image = os.path.join(args.build, "ports.img")
    run_image = os.path.join(args.build, "tty-run.img")

    if os.path.exists(run_image):
        os.remove(run_image)

    rc = build_image(image, args.x86, programs, args.verbose)
    if rc != 0:
        print("tty-smoke: the volume of the run was not written", file=sys.stderr)
        return rc

    shutil.copyfile(image, run_image)

    started = time.monotonic()
    console = Console(loader, kernel, run_image, args.disk, args.timeout)

    try:
        output = drive(console)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        console.close()
        if not args.keep and os.path.exists(run_image):
            os.remove(run_image)

    if args.verbose:
        print(output)

    checks = check(output)
    failed = [item for item in checks if not item[1]]

    print("Micro OS — a full-screen editor at the machine (make tty-smoke)")
    print("=" * 72)
    for name, ok, detail in checks:
        if ok and not args.verbose:
            continue
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 72)
    print(f"tty: {len(SCREEN_PROGRAMS)} program(s) of a screen, {len(OWN_PROGRAMS)} of this "
          f"directory, {len(checks)} check(s), {time.monotonic() - started:.0f} s of the machine")

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
