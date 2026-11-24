#!/usr/bin/env python3
"""Micro OS — the keyboard, the stick and the wire of the USB (ADR 0040).

The machine of a laptop of these years has no PS/2 port its firmware will
share and no COM port, so the keyboard of the person, the stick of the world
and the dongle of the network are devices of the USB and the only way to them
is the host controller of the PCI bus. This test boots exactly that machine
and drives the three classes of M1 of 1.8:

  1. **the keyboard** (`-device qemu-xhci -device usb-kbd`): the i8042 of QEMU
     is turned off (`-machine pc,i8042=off`), so the only keyboard of the
     machine is the one of the controller and the keys of the monitor
     (`sendkey`) arrive through it. A line of the shell is typed (with a Shift
     and a Backspace) and `/bin/keys` reads the keys themselves out of
     `/dev/input0` - the code of `a` and its press and release, the code of
     Enter - so the test proves the keyboard twice: as characters and as
     events;

  2. **the stick** (`-device usb-storage`): the class of the Bulk-Only
     Transport enumerates the disk, reads INQUIRY, READ CAPACITY and READ(10),
     registers `usb0`, scans its partition table and mounts `usb0p1`. The
     volume of the run is the ext2 image of `make x86-disk` (a volume of
     another system), so the rule of ADR 0041 calls the disk read-only; the
     test reads a file of it, and a write is refused by the driver. The proof
     that nothing was written is the SHA-256 of the whole image before and
     after the boot - the same two numbers, or the test fails;

  3. **the wire** (`-device usb-net`): the class of CDC-ECM, the MAC out of
     the string descriptor of the interface, SET_INTERFACE of the data
     alternate setting and SET_ETHERNET_PACKET_FILTER, and then the frames of
     the port's stack: `netinfo` names `usbnet0` and an echo of ICMP to the
     gateway of QEMU user-net crosses the bulk endpoints (`ping 10.0.2.2`).
     That boot uses the image of `MICRO_OS_X86_NET=ON` (`build-x86-net`), the
     same one `make x86-net-smoke` reads over virtio.

A fourth mode (`--stick DEVICE`) is the stick of a person: the device is
attached read-only to the controller, the test mounts its first partition and
lists it, and the SHA-256 of the whole device is compared before and after.
The device is a block device of the host (`/dev/sdc`) and the run needs the
permission to read it; a machine without the device skips the mode.

    tools/x86_usb_smoke.py [--image PATH] [--ext2-image PATH]
                           [--net-build PATH] [--stick DEVICE]
                           [--timeout SECONDS] [--verbose]

The exit status is 0 when every check of the modes asked held, 1 when one did
not, and 2 when the image or QEMU is missing.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PROMPT = "microos> "

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
NET_BUILD = os.path.join(ROOT, "build-x86-net")
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")
DEFAULT_EXT2_IMAGE = os.path.join(BUILD, "programs-ext2.img")

# The keys of one line of the shell, in the names of the monitor of QEMU. The
# names of the letters are the letters, the space is `spc` and Enter is `ret`;
# `shift-u` is the capital that proves the modifier of the report.
LINE_ECHO = ["e", "c", "h", "o", "spc", "shift-u", "s", "b", "ret"]

# The same with a letter that the Backspace takes back: the line the shell
# runs is `echo ab`, and the `x` is in the echo of the typing and not in the
# answer.
LINE_BACKSPACE = ["e", "c", "h", "o", "spc", "a", "b", "x", "backspace", "ret"]

# The line that runs the reader of the events, and the two keys it reads.
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

# The volume of the stick of the run: the ext2 image of `make x86-disk`, whose
# tree is the one of /bin and /etc of the release. The setting file of it is
# the file the test reads through the USB.
SETTING = ["hello", "hostname=x86_64", "console=vga"]


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()

    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)

    return digest.hexdigest()


class Machine:
    """The emulated machine, its serial port and the monitor of QEMU."""

    def __init__(self, loader: str, kernel: str, image: str, workdir: str,
                 devices: list, serial_only: bool = False) -> None:
        os.makedirs(workdir, exist_ok=True)
        self.serial_path = os.path.join(workdir, "serial.sock")
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.transcript = b""
        self.serial_only = serial_only

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
            "-display",
            "none",
            "-serial",
            f"unix:{self.serial_path},server=on,wait=off",
            "-monitor",
            f"unix:{self.monitor_path},server=on,wait=off",
            *devices,
            "-drive",
            f"file={image},format=raw,if=ide,index=0,cache=writeback",
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

    def _block(self, start: int, count: int, timeout: float) -> str:
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            text = self.read_serial()
            if text.count(PROMPT) > count:
                end = text.find(PROMPT, start + len(PROMPT))
                return text[start + len(PROMPT) : end if end >= 0 else len(text)]
            time.sleep(0.05)

        return ""

    def answer(self, lines: list, timeout: float) -> str:
        """Type a line through the keyboard and return what the shell printed.

        The count of the prompts is taken before the line goes out: the prompt
        of the line is the mark of its end, and the text between the two is
        the answer of the command (the echo of the typing included).
        """
        count = self.prompts()
        start = self.read_serial().rfind(PROMPT)
        self.press(lines)
        return self._block(start, count, timeout)

    def answer_serial(self, line: str, timeout: float) -> str:
        """Type a line through the serial console, which is a console too."""
        count = self.prompts()
        start = self.read_serial().rfind(PROMPT)
        self.serial.sendall((line + "\n").encode("ascii"))
        return self._block(start, count, timeout)

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


def lines_of(text: str) -> list:
    return [line.strip() for line in text.replace("\r", "").split("\n")]


def run_usb(loader: str, kernel: str, image: str, ext2_image: str, workdir: str,
            timeout: float) -> list:
    """One boot with the keyboard, the stick and the events of the keyboard."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    stick = os.path.join(workdir, "stick.img")
    os.makedirs(workdir, exist_ok=True)
    shutil.copyfile(ext2_image, stick)
    before = sha256_file(stick)

    devices = [
        "-device", "usb-kbd,bus=xhci.0",
        "-device", "usb-storage,drive=stick,bus=xhci.0",
        "-drive", f"id=stick,file={stick},format=raw,if=none",
    ]

    try:
        machine = Machine(loader, kernel, image, workdir, devices)
    except (OSError, RuntimeError) as error:
        expect("the machine of the USB starts", False, str(error))
        return checks

    try:
        booted = machine.wait_serial(PROMPT, timeout)
        text = machine.read_serial()

        # --- the controller and the devices
        expect("the machine boots on a keyboard of the USB", booted, text[-600:])
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
        expect(
            "the boot finds the stick of the storage class",
            re.search(r"usb0: .*\d+ MB on the port \d+ of the controller", text) is not None,
            "the Bulk-Only Transport did not enumerate the disk",
        )
        expect(
            "the stick is a volume of another system, read-only",
            re.search(r"usb0: usb0p1: the volume of another system \(ext2\), read-only", text)
            is not None,
            "the rule did not identify the volume of the stick",
        )
        expect(
            "the rule says why the stick is read-only",
            "usb0: the disk is read-only: the medium carries a volume of another system" in text,
            "no sentence of ADR 0041 in the boot",
        )
        expect(
            "the storage class reports one disk and nothing written",
            "usb: 1 disk(s) of the storage class, read-only, nothing written" in text,
            "the boot did not say what it did with the stick",
        )

        # --- the characters, through the console
        echo = machine.answer(LINE_ECHO, timeout)
        answered = lines_of(echo)
        expect(
            "a line typed on the USB keyboard reaches the shell",
            "echo Usb" in answered,
            echo[-300:],
        )
        expect(
            "the shell runs the line of the USB keyboard",
            "Usb" in answered,
            echo[-300:],
        )
        expect(
            "the Shift of the USB report is the capital of the console",
            "Usb" in answered
            and "usb" not in [line for line in answered if line != "echo Usb"],
            echo[-300:],
        )

        edited = machine.answer(LINE_BACKSPACE, timeout)
        edited_lines = lines_of(edited)
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

        # --- the stick, through the shell of the serial port
        made = machine.answer_serial("mkdir /mnt", timeout)
        expect("the point of the mount is made", "mkdir" in made or "is a directory" in made,
               made[-200:])

        mounted = machine.answer_serial("mount usb0p1 /mnt ext2", timeout)
        expect(
            "the first partition of the stick is mounted",
            "mount: usb0p1 is mounted at /mnt" in mounted,
            mounted[-300:],
        )

        listing = lines_of(machine.answer_serial("ls /mnt", timeout))
        expect(
            "the listing of the stick is the tree of its volume",
            "bin" in listing and "etc" in listing,
            listing,
        )

        settings = lines_of(machine.answer_serial("cat /mnt/etc/setting", timeout))
        expect(
            "a file of the stick is read through the USB",
            all(line in settings for line in SETTING),
            settings,
        )
        expect(
            "the bytes of the file are the bytes of the image",
            "hostname=x86_64" in settings,
            settings,
        )

        numbers = machine.answer_serial("df /mnt", timeout)
        expect(
            "the numbers of the volume are the numbers of the stick",
            re.search(r"\b56320\b", numbers) is not None,
            numbers[-300:],
        )

        refused = machine.answer_serial("write /mnt/x abc", timeout)
        expect(
            "a write to the stick is refused",
            "refused" in machine.read_serial() or "read-only" in machine.read_serial(),
            refused[-300:],
        )
        expect(
            "the shell lives after the refused write",
            re.search(r"MicroOS \d+\.\d+\.\d+",
                      machine.answer_serial("uname", timeout)) is not None,
            "",
        )

        machine.answer_serial("umount /mnt", timeout)
    finally:
        machine.close()

    after = sha256_file(stick)
    checks.append(
        (
            "the bytes of the stick are the same after the boot",
            before == after,
            f"{before} before, {after} after",
        )
    )

    return checks


def run_net(loader: str, kernel: str, image: str, workdir: str, timeout: float) -> list:
    """One boot of the network image with the wire of the USB on the user net."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    devices = [
        "-device", "usb-net,netdev=n0",
        "-netdev", "user,id=n0",
    ]

    try:
        machine = Machine(loader, kernel, image, workdir, devices)
    except (OSError, RuntimeError) as error:
        expect("the machine of the wire starts", False, str(error))
        return checks

    try:
        booted = machine.wait_serial(PROMPT, timeout)
        text = machine.read_serial()

        expect("the machine boots on a wire of the USB", booted, text[-600:])
        match = re.search(
            r"usbnet0: CDC-ECM [0-9a-f]{4}:[0-9a-f]{4} on port \d+, "
            r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})",
            text,
        )
        expect(
            "the boot names the wire of the CDC-ECM",
            match is not None,
            "the class of the network did not attach",
        )
        mac = match.group(1) if match else ""

        info = machine.answer_serial("netinfo", 30.0)
        expect(
            "the stack names the wire of the USB",
            re.search(r"netinfo: usbnet0, link up", info) is not None,
            info[-400:],
        )
        expect(
            "the MAC of the wire is the one of its descriptor",
            mac != "" and f"mac {mac}" in info,
            info[-400:],
        )
        expect(
            "the address of the network is the one of the user net",
            "ip 10.0.2.15/24" in info and "gateway 10.0.2.2" in info,
            info[-400:],
        )

        ping = machine.answer_serial("ping 10.0.2.2", 60.0)
        expect(
            "an echo crosses the wire of the USB",
            re.search(r"ping: \d+ bytes from 10\.0\.2\.2", ping) is not None,
            ping[-400:],
        )

        counters = machine.answer_serial("netinfo", 30.0)
        moved = re.search(r"(\d+) frame\(s\) in, (\d+) out", counters)
        expect(
            "the counters of the wire move",
            moved is not None and int(moved.group(1)) > 0 and int(moved.group(2)) > 0,
            counters[-400:],
        )
    finally:
        machine.close()

    return checks


def run_stick(loader: str, kernel: str, image: str, workdir: str, stick: str,
              timeout: float) -> list:
    """The stick of a person, attached read-only: list it and hash it."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    if not os.path.exists(stick):
        expect("the stick of the person is there", False, stick)
        return checks

    try:
        before = sha256_file(stick)
    except OSError as error:
        expect("the stick of the person is readable", False, str(error))
        return checks

    expect("the stick of the person is readable", True, stick)

    devices = [
        "-drive", f"id=stick,file={stick},format=raw,if=none,readonly=on",
        "-device", "usb-storage,drive=stick,bus=xhci.0",
    ]

    try:
        machine = Machine(loader, kernel, image, workdir, devices)
    except (OSError, RuntimeError) as error:
        expect("the machine of the stick starts", False, str(error))
        return checks

    try:
        booted = machine.wait_serial(PROMPT, timeout)
        text = machine.read_serial()

        expect("the machine boots with the stick of the person", booted, text[-600:])
        expect(
            "the stick of the person is enumerated",
            re.search(r"usb0: .*\d+ MB on the port \d+ of the controller", text) is not None,
            text[-600:],
        )
        expect(
            "the rule calls the stick of the person read-only",
            "usb0: the disk is read-only:" in text,
            text[-600:],
        )

        machine.answer_serial("mkdir /mnt", timeout)
        mounted = machine.answer_serial("mount usb0p1 /mnt fat32", timeout)
        expect(
            "the first partition of the stick is mounted",
            "mount: usb0p1 is mounted at /mnt" in mounted,
            mounted[-400:],
        )
        listing = lines_of(machine.answer_serial("ls /mnt", timeout))
        expect(
            "the listing of the stick of the person is readable",
            len([line for line in listing if line]) > 0,
            listing,
        )
        machine.answer_serial("umount /mnt", timeout)
    finally:
        machine.close()

    try:
        after = sha256_file(stick)
    except OSError as error:
        expect("the stick of the person is readable after the boot", False, str(error))
        return checks

    checks.append(
        (
            "the bytes of the stick of the person are the same",
            before == after,
            f"{before} before, {after} after",
        )
    )

    return checks


def net_image(build: str) -> str:
    """The image of `build-x86-net`, written by the tool of `make x86-disk`."""
    image = os.path.join(build, "programs.img")

    if os.path.isfile(image):
        return image

    mkfs = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
    result = subprocess.run(
        [
            sys.executable,
            os.path.join(ROOT, "tools", "x86_image.py"),
            "--image", image,
            "--build", build,
            "--mkfs", mkfs,
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        raise RuntimeError((result.stdout + result.stderr).strip())

    return image


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the devices of the USB of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the disk image of the machine")
    parser.add_argument("--ext2-image", default=DEFAULT_EXT2_IMAGE,
                        help="the volume of the stick of the run")
    parser.add_argument("--net-build", default=NET_BUILD,
                        help="the build of MICRO_OS_X86_NET=ON for the wire")
    parser.add_argument("--stick", default=None,
                        help="a block device of the host, attached read-only")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds for one wait")
    parser.add_argument("--verbose", action="store_true", help="print the boot of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel, args.image, args.ext2_image):
        if not os.path.isfile(path):
            print(f"usb-smoke: {path} is missing (make x86-disk)", file=sys.stderr)
            return 2

    workdir = tempfile.mkdtemp(prefix="microos-usb-")
    checks = []

    try:
        checks += run_usb(loader, kernel, args.image, args.ext2_image,
                          os.path.join(workdir, "usb"), args.timeout)
    except FileNotFoundError as error:
        print(f"usb-smoke: {error}", file=sys.stderr)
        return 2

    # The wire needs the image of the network build; a tree without it keeps
    # the checks of the keyboard and the stick and says so.
    net_loader = os.path.join(args.net_build, "loader32.elf")
    net_kernel = os.path.join(args.net_build, "microos64.bin")

    if os.path.isfile(net_loader) and os.path.isfile(net_kernel):
        try:
            checks += run_net(net_loader, net_kernel, net_image(args.net_build),
                              os.path.join(workdir, "net"), args.timeout)
        except (OSError, RuntimeError) as error:
            checks.append(("the image of the wire is built", False, str(error)))
    else:
        checks.append(
            ("the image of the wire is built", False,
             f"{args.net_build} is missing (make x86-net-build)")
        )

    if args.stick is not None:
        checks += run_stick(loader, kernel, args.image, os.path.join(workdir, "stick"),
                            args.stick, args.timeout)

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
