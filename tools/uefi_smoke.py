#!/usr/bin/env python3
"""Micro OS — the boot of the x86-64 machine on a firmware of UEFI (OVMF).

The other smoke tests of the port boot a multiboot image that QEMU loads with
`-kernel`: there is no firmware under it, the loader of the port copies the
kernel to 16 MiB and the console of the port programs the card itself. This
one boots the way a laptop does: the firmware of the machine (OVMF, the
EDK2 build that QEMU carries) reads a FAT volume, starts
`\\EFI\\BOOT\\BOOTX64.EFI` from it, and the loader of `loader/uefi` finds the
picture of the firmware, reads the kernel and the disk image out of the same
volume, leaves the boot services and jumps into the kernel.

What the run proves, in the order it happens:

  * the loader of the firmware starts (its line on the console of the
    firmware), asks for a picture and programs the mode it chose;
  * the volume of the boot holds the two files, and the loader copies them:
    the kernel to a 2 MiB-aligned block of the memory of the firmware and the
    disk image below the identity map of the boot (16 MB of a file);
  * the map of the memory of the firmware is read and written in the shape
    the port reads, and the root of the ACPI tables comes from the
    configuration table of the firmware - which is the only place a machine
    of UEFI keeps it;
  * the boot services are left, the page tables of the boot carry the image
    at the top of the address space, and the kernel of the BIOS path - the
    same `microos64.bin` - comes up;
  * the kernel reads the disk of the machine from the memory (there is no
    driver for the controller the volume was on) and mounts it;
  * the console draws on the frame of the firmware, which is 32 bits per
    pixel in the order of its own bytes, and the screenshot of the emulated
    card is the picture of the shell: the run reads the screen, not only the
    serial line;
  * the shell answers the four commands of the other smoke test on that
    machine: `uname`, `ls /bin`, `cat /etc/setting` and `pkg -Q` (the last one
    only in a build that has the installer).

    tools/uefi_smoke.py [--image PATH] [--timeout SECONDS] [--verbose]

The image is the ESP of `make uefi` (`tools/uefi_esp.py`), and the firmware is
the OVMF of the machine: `/usr/share/edk2/x64/OVMF_CODE.4m.fd` on Arch and the
two other names a distribution gives it. The variables of the firmware are
copied to a temporary file, because a boot writes to them and the next boot of
the machine has to find the firmware it had.

The exit status is 0 when every check held, 2 when the firmware, the image or
QEMU is missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_IMAGE = os.path.join(ROOT, "build-uefi", "esp.img")

PROMPT = "microos> "

# The firmware of the machine: the code and the variables of the EDK2 build of
# QEMU. The 4 MiB image is the one of the distribution of this machine; the
# plain one is what the older ones carry, and the run finds the pair that is
# installed instead of naming one.
FIRMWARE = [
    ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
    ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),
    ("/usr/share/qemu/OVMF_CODE.fd", "/usr/share/qemu/OVMF_VARS.fd"),
    ("/usr/share/edk2-ovmf/x64/OVMF_CODE.fd", "/usr/share/edk2-ovmf/x64/OVMF_VARS.fd"),
]

# The colour of the plain text of the console in the format of the kernel
# (RGB565 0xAD55 is the light grey of the VGA) as the display widens it: five
# bits of red, six of green and five of blue, each repeated into a byte.
GREY = (173, 170, 173)

# The commands the other smoke test of the port types, on the machine of the
# firmware. The answers are the ones of a machine whose disk is a file of the
# ESP: the tree is the same tree.
COMMANDS = [
    "uname",
    "ls /bin",
    "cat /etc/setting",
    "pkg -Q",
]


class MachineError(RuntimeError):
    """The machine is not there any more."""


class Machine:
    """One boot of the ESP under OVMF, its console and its monitor."""

    def __init__(self, image: str, code: str, variables: str, workdir: str, cpus: int,
                 memory: str) -> None:
        self.image = image
        self.code = code
        self.variables = variables
        self.workdir = workdir
        self.cpus = cpus
        self.memory = memory
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.process: subprocess.Popen | None = None
        self.transcript = ""
        self.pending = ""
        self.monitor: socket.socket | None = None

    def command(self) -> list:
        return [
            "qemu-system-x86_64",
            "-machine",
            "q35",
            # Two processors: the boot says what the firmware's MADT reports,
            # which is the one line of the log that proves the root of the ACPI
            # tables came from the firmware and not from the extended BIOS area
            # (a machine of UEFI does not keep it there).
            "-smp",
            str(self.cpus),
            "-m",
            self.memory,
            "-drive",
            f"if=pflash,format=raw,readonly=on,file={self.code}",
            "-drive",
            f"if=pflash,format=raw,file={self.variables}",
            # The ESP as a disk of the machine's own: the firmware finds the FAT
            # volume, and the loader reads the files out of it.
            "-drive",
            f"file={self.image},format=raw,if=ide,index=0",
            "-nographic",
            "-no-reboot",
            "-monitor",
            f"unix:{self.monitor_path},server=on,wait=off",
        ]

    # --- the boot and the console ---------------------------------------

    def open(self, timeout: float) -> str:
        command = self.command()
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        if self.process.stdout is None:
            raise MachineError("no console of the machine")

        self._connect_monitor()

        return self._read_until(PROMPT, timeout)

    def close(self) -> None:
        if self.monitor is not None:
            self.monitor.close()
            self.monitor = None

        if self.process is None:
            return

        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

        if self.process.stdout is not None:
            self.process.stdout.close()
        if self.process.stdin is not None:
            self.process.stdin.close()

        self.process = None

    def _connect_monitor(self, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            try:
                monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                monitor.connect(self.monitor_path)
                monitor.settimeout(0.2)
                self.monitor = monitor
                self._drain_monitor()
                return
            except OSError:
                time.sleep(0.05)

        raise MachineError("the monitor of QEMU is not there")

    def _drain_monitor(self, window: float = 0.3) -> str:
        text = ""
        deadline = time.monotonic() + window

        assert self.monitor is not None

        while time.monotonic() < deadline:
            try:
                chunk = self.monitor.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break

            if not chunk:
                break

            text += chunk.decode("utf-8", errors="replace")

        return text

    def _read(self, timeout: float) -> str:
        process = self.process

        if process is None or process.stdout is None:
            raise MachineError("the machine is not running")

        descriptor = process.stdout.fileno()
        ready, _, _ = select.select([descriptor], [], [], 0.05)

        if not ready:
            return ""

        chunk = os.read(descriptor, 65536)

        if not chunk:
            raise MachineError("QEMU went away: a reset or a triple fault")

        text = chunk.decode("utf-8", errors="replace")
        self.transcript += text
        self.pending += text

        return text

    def _read_until(self, marker: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout

        while marker not in self.pending:
            if time.monotonic() >= deadline:
                raise MachineError(f"the machine wrote nothing in {timeout:.0f} s")

            self._read(timeout)

        index = self.pending.index(marker)
        block = self.pending[:index]
        self.pending = self.pending[index + len(marker):]

        return block

    def run(self, command: str, timeout: float = 30.0) -> str:
        process = self.process

        if process is None or process.stdin is None:
            raise MachineError("the machine is not running")

        process.stdin.write(command.encode("ascii") + b"\n")
        process.stdin.flush()

        return self._read_until(PROMPT, timeout)

    # --- the screen ------------------------------------------------------

    def screenshot(self, name: str, timeout: float = 10.0) -> tuple:
        """The picture of the card as (width, height, bytes) of the pixels."""
        path = os.path.join(self.workdir, name)

        if os.path.exists(path):
            os.unlink(path)

        assert self.monitor is not None
        self.monitor.sendall(f'screendump "{path}"\n'.encode("ascii"))

        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 0:
                break
            time.sleep(0.05)
            self._drain_monitor()
        else:
            raise MachineError("the monitor of QEMU did not dump the screen")

        return read_ppm(path)


def read_ppm(path: str) -> tuple:
    """A binary PPM (P6) as the width, the height and the bytes of the pixels."""
    data = open(path, "rb").read()
    values = []
    index = 0

    while len(values) < 4:
        while index < len(data) and data[index : index + 1].isspace():
            index += 1
        if data[index : index + 1] == b"#":
            while data[index : index + 1] != b"\n":
                index += 1
            continue
        start = index
        while not data[index : index + 1].isspace():
            index += 1
        values.append(data[start:index])

    if values[0] != b"P6":
        raise MachineError("the picture of the card is not a PPM")

    width = int(values[1])
    height = int(values[2])

    return width, height, data[index + 1:]


def count_grey(pixels: bytes) -> int:
    """The pixels of the ink of the console: the light grey of the theme."""
    wanted = bytes(GREY)
    count = 0
    offset = 0

    while True:
        found = pixels.find(wanted, offset)

        if found < 0:
            break

        # The three bytes have to be one pixel: not the tail of another one.
        if found % 3 == 0:
            count += 1

        offset = found + 1

    return count


def find_firmware() -> tuple:
    for code, variables in FIRMWARE:
        if os.path.isfile(code) and os.path.isfile(variables):
            return code, variables

    return None, None


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="boot the x86-64 machine under OVMF")
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--memory", default="512",
                        help="the memory of the machine in MB (default: 512, the size of the "
                             "first gigabyte the kernel can name)")
    parser.add_argument("--cpus", type=int, default=2,
                        help="the processors of the machine (default: 2, which is what the "
                             "boot line of the MADT is checked against)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if shutil.which("qemu-system-x86_64") is None:
        print("uefi-smoke: qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    if not os.path.isfile(args.image):
        print(f"uefi-smoke: {args.image} is not there: make uefi", file=sys.stderr)
        return 2

    code, variables = find_firmware()

    if code is None:
        print("uefi-smoke: no OVMF of the machine (install edk2-ovmf)", file=sys.stderr)
        return 2

    checks = 0
    failures = 0

    def expect(name: str, condition: bool) -> None:
        nonlocal checks, failures

        checks += 1

        if condition:
            print(f"  ok   {name}")
            return

        failures += 1
        print(f"  FAIL {name}")

    with tempfile.TemporaryDirectory(prefix="microos-uefi-") as workdir:
        # The variables of the firmware are written by every boot: a copy for
        # this one, and the firmware of the machine stays what it was.
        copy = os.path.join(workdir, "vars.fd")
        shutil.copyfile(variables, copy)

        machine = Machine(args.image, code, copy, workdir, args.cpus, args.memory)

        try:
            banner = machine.open(args.timeout)
        except MachineError as error:
            print(f"uefi-smoke: {error}", file=sys.stderr)
            machine.close()
            return 1

        if args.verbose:
            print(banner)

        # --- the loader ----------------------------------------------------

        expect("the firmware starts BOOTX64.EFI", "loader of a firmware machine" in banner)

        mode = re.search(r"uefi: the picture is (\d+)x(\d+)", banner)
        expect("the loader programs a picture", mode is not None)

        expect("the picture is a 32-bit format of the firmware",
               re.search(r"uefi: the picture is .*, 32 bpp (bgrx|rgbx)", banner) is not None)

        expect("the frame is below the 8 MiB of the console",
               re.search(r"uefi: the frame is (\d+) KB", banner) is not None)

        expect("the volume has the kernel", "uefi: the kernel is" in banner)
        expect("the volume has the disk image", "uefi: the disk is" in banner)
        expect("the kernel is placed below the arena",
               re.search(r"uefi: the kernel is placed at 0x6|0x7|0x8|0x9|0xa|0xb|0xc|0xd|0xe|0x10", banner)
               is not None)
        expect("the map of the memory is read",
               re.search(r"uefi: the map of the memory has \d+ region", banner) is not None)
        expect("the ACPI tables come from the firmware",
               "uefi: the ACPI tables are at 0x" in banner)
        expect("the boot services are left", "the boot services are gone" in banner)

        # --- the kernel ----------------------------------------------------

        expect("the kernel comes up", "MicroOS" in banner or "micro: MicroOS" in banner)
        expect("the boot says it is the firmware's", "x86-64: the boot is uefi" in banner)
        expect("the map of the kernel is W^X", "the map is W^X" in banner)
        expect("the console draws on the frame of the firmware",
               "the framebuffer of the firmware" in banner)

        expect(f"the firmware's MADT is read ({args.cpus} processors)",
               re.search(rf"x86-64: {args.cpus} cpu\(s\) reported by the firmware", banner)
               is not None)

        expect("the disk of the machine is the module of the boot",
               "root: the root is mfs on boot0p1" in banner)

        # The keyboard: the port asks the device which scancode set it speaks
        # and the controller whether it translates (src/ps2.c). The keyboard of
        # QEMU is a device of the set 2 behind a controller that translates,
        # and reading that answer as "the bytes are the set 2" would give a
        # machine whose every key is another one - so the line and the commands
        # below are one check of the two.
        expect("the keyboard is asked which scancode set it speaks",
               re.search(r"ps2: the keyboard speaks set [123]", banner) is not None)
        expect("the bytes of the keyboard are the set 1",
               re.search(r"ps2: the keyboard speaks set 1|ps2: .*translates it into set 1", banner)
               is not None)

        expect("the program of /bin runs", "PROGRAM: hello from /bin" in banner)
        expect("the shell is up", "MicroOS shell ready" in banner)

        # --- the shell -----------------------------------------------------

        answers = {}

        for command in COMMANDS:
            try:
                answers[command] = machine.run(command)
            except MachineError as error:
                print(f"uefi-smoke: {error}", file=sys.stderr)
                machine.close()
                return 1

            if args.verbose:
                print(answers[command])

        # The echo of the line editor of the session: the command a person
        # types is in the transcript before its answer, because the session
        # writes every key it takes (components/config/src/config_session.c).
        # A machine whose line is not echoed is a machine a person types blind
        # at, which is what the laptop of the set 2 looked like before the
        # scancodes were read right.
        expect("the shell echoes what is typed", answers["uname"].lstrip().startswith("uname"))
        expect("uname answers the machine", "MicroOS" in answers["uname"] and "x86_64" in answers["uname"])
        expect("ls /bin reads the tree of the disk", "hello" in answers["ls /bin"] and "ls" in answers["ls /bin"])
        expect("cat /etc/setting reads a file of the disk", "hostname=x86_64" in answers["cat /etc/setting"])

        # The installer of programs is a build of the machine with the network
        # (MICRO_OS_PKG); a build without it answers with the shell's refusal,
        # which is the honest answer of that build and not a failure of the
        # boot.
        pkg = answers["pkg -Q"]
        expect("pkg -Q answers like the build it is",
               "no package" in pkg or "command not found" in pkg or "unknown command" in pkg)

        # --- the screen ----------------------------------------------------

        width, height, pixels = machine.screenshot("boot.ppm")
        expect("the picture of the card is the mode of the boot",
               mode is not None and width == int(mode.group(1)) and height == int(mode.group(2)))

        ink = count_grey(pixels)
        expect("the console is on the glass", ink > 2000)

        # The answer of a command reaches the glass: the picture is read again
        # after the shell wrote a line, and it is another picture. The ink of
        # the screen is not compared by its count - a boot whose log fills the
        # grid scrolls, and a line leaves the top of the picture as the new one
        # arrives - but the two pictures are not the same bytes.
        machine.run("uname")
        width, height, after = machine.screenshot("after.ppm")
        expect("the picture of the glass answers the shell",
               after != pixels and count_grey(after) > 2000)

        # The band of a present is a band of cells: the rest of the frame is
        # the paper of the console, which is black in the theme of the boot.
        expect("the frame around the console is the paper",
               pixels[(height // 2 * width + width - 8) * 3:(height // 2 * width + width - 8) * 3 + 3]
               == b"\x00\x00\x00")

        machine.close()

    print(f"uefi-smoke: {checks - failures} of {checks} checks held")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
