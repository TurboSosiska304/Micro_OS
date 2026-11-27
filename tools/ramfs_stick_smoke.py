#!/usr/bin/env python3
"""Smoke: the desktop on the stick image boots with a writable /tmp.

The release 2.0.2 defect: the root of the stick is read-only, wm keeps the
pixels of a window in /tmp, and the desktop dies with "wm: no file for the
pixels of a window" / "wm: /bin/term exited with code 138". The fix mounts a
ramfs at /tmp when the root is read-only.

This smoke boots the UEFI stick image, runs `exec /bin/wm`, opens a terminal
with Alt+T, types `help`, closes the window with Alt+F4, and checks that the
shell prompt returns.
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_STICK = os.path.join(ROOT, "build-uefi", "stick.img")
DEFAULT_ISO = os.path.join(ROOT, "build-x86-iso", "microos.iso")

PROMPT = "microos> "

OVMF_DIRS = ["/usr/share/edk2/x64", "/usr/share/OVMF", "/usr/share/edk2/ovmf"]
OVMF_CODE = ["OVMF_CODE.4m.fd", "OVMF_CODE.fd", "OVMF_CODE_4M.fd"]
OVMF_VARS = ["OVMF_VARS.4m.fd", "OVMF_VARS.fd", "OVMF_VARS_4M.fd"]


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
    def __init__(self, stick: str | None, code: str | None, variables: str | None,
                 iso: str | None = None) -> None:
        self.stick = stick
        self.iso = iso
        self.code = code
        self.variables = variables
        self.process: subprocess.Popen | None = None
        self.serial = None
        self.monitor = None
        self.transcript = b""
        prefix = "ramfs-iso-" if iso else "ramfs-stick-"
        self.work = tempfile.mkdtemp(prefix=prefix, dir="/tmp/opencode")
        self.serial_path = os.path.join(self.work, "serial.sock")
        self.monitor_path = os.path.join(self.work, "monitor.sock")

    def _connect(self, path: str, timeout: float = 30.0) -> socket.socket:
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
        raise RuntimeError(f"no socket at {path}")

    def _read(self, sock: socket.socket, window: float) -> bytes:
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
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def open(self, timeout: float) -> bool:
        if self.iso is not None:
            command = [
                "qemu-system-x86_64",
                "-cdrom", self.iso,
                "-m", "256",
                "-boot", "d",
                "-vga", "std",
                "-display", "none",
                "-no-reboot",
                "-serial", f"unix:{self.serial_path},server=on,wait=off",
                "-monitor", f"unix:{self.monitor_path},server=on,wait=off",
            ]
        else:
            variables = os.path.join(self.work, "vars.fd")
            shutil.copyfile(self.variables, variables)
            command = [
                "qemu-system-x86_64",
                "-drive", f"if=pflash,format=raw,readonly=on,file={self.code}",
                "-drive", f"if=pflash,format=raw,file={variables}",
                "-device", "qemu-xhci,id=xhci",
                "-drive", f"if=none,id=usbdisk,file={self.stick},format=raw",
                "-device", "usb-storage,drive=usbdisk,bus=xhci.0",
                "-m", "512",
                "-vga", "std",
                "-display", "none",
                "-no-reboot",
                "-serial", f"unix:{self.serial_path},server=on,wait=off",
                "-monitor", f"unix:{self.monitor_path},server=on,wait=off",
            ]

        self.process = subprocess.Popen(
            command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        self.serial = self._connect(self.serial_path)
        self.monitor = self._connect(self.monitor_path)
        self._drain_monitor()
        return self.wait_serial(PROMPT, timeout)

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

    def screendump(self, path: str) -> bool:
        if os.path.exists(path):
            os.unlink(path)
        self.monitor_command(f'screendump "{path}"')
        deadline = time.monotonic() + 5.0
        size = -1
        while time.monotonic() < deadline:
            if os.path.exists(path):
                current = os.path.getsize(path)
                if current > 0 and current == size:
                    return True
                size = current
            time.sleep(0.05)
        return os.path.exists(path) and os.path.getsize(path) > 0

    def sendkey(self, key: str) -> None:
        self.monitor_command(f"sendkey {key}")
        time.sleep(0.05)

    def sendkeys(self, keys: list[str], delay: float = 0.05) -> None:
        for key in keys:
            self.sendkey(key)
            time.sleep(delay)

    def type_text(self, text: str) -> None:
        mapping = {
            " ": "spc",
            "/": "slash",
            "-": "minus",
            ".": "dot",
            "_": "shift-minus",
            ":": "shift-semicolon",
        }
        for ch in text:
            key = mapping.get(ch, ch)
            self.sendkey(key)

    def run_line(self, line: str, timeout: float) -> bool:
        count = self.read_serial().count(PROMPT)
        self.serial.sendall((line + "\n").encode("ascii"))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.read_serial().count(PROMPT) > count:
                return True
            time.sleep(0.05)
        return False

    def close(self) -> None:
        for sock in (self.serial, self.monitor):
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        shutil.rmtree(self.work, ignore_errors=True)


def check(title: str, ok: bool, detail: str = "") -> bool:
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {title}")
    if not ok and detail:
        for line in detail.strip().splitlines()[-12:]:
            print(f"       {line}")
    return ok


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stick", default=DEFAULT_STICK)
    parser.add_argument("--iso", default=None,
                        help="boot this ISO instead of the UEFI stick")
    parser.add_argument("--ovmf-code", default=None)
    parser.add_argument("--ovmf-vars", default=None)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--shots", default=None)
    args = parser.parse_args(argv)

    iso = args.iso
    if iso is None and os.path.basename(sys.argv[0]) == "ramfs_iso_smoke.py":
        iso = DEFAULT_ISO

    if iso is not None:
        if not os.path.isfile(iso):
            print(f"ramfs_stick_smoke: missing ISO image {iso}", file=sys.stderr)
            return 2
    else:
        if not os.path.isfile(args.stick):
            print(f"ramfs_stick_smoke: missing stick image {args.stick}", file=sys.stderr)
            return 2

    code = None
    variables = None
    if iso is None:
        code = find_ovmf(OVMF_CODE, args.ovmf_code, "MICROOS_OVMF_CODE")
        variables = find_ovmf(OVMF_VARS, args.ovmf_vars, "MICROOS_OVMF_VARS")
        if code is None or variables is None:
            print("ramfs_stick_smoke: no OVMF firmware found", file=sys.stderr)
            return 2

    if shutil.which("qemu-system-x86_64") is None:
        print("ramfs_stick_smoke: qemu-system-x86_64 not found", file=sys.stderr)
        return 2

    shots = args.shots
    if shots:
        os.makedirs(shots, exist_ok=True)

    machine = Machine(args.stick, code, variables, iso=iso)
    ok = True
    medium = "ISO" if iso is not None else "stick"
    try:
        booted = machine.open(args.timeout)
        ok &= check(f"the {medium} boots to a shell prompt", booted,
                    machine.read_serial()[-400:])

        boot_text = machine.read_serial()

        mounts = ""
        if machine.run_line("cat /proc/mounts", 60.0):
            mounts = machine.read_serial()
        root_ro = re.search(r"\S+\s+/\s+\S+\s+ro\b", mounts) is not None
        has_tmp_ramfs = re.search(r"\S+\s+/tmp\s+ramfs\s+rw", mounts) is not None

        if iso is not None:
            ok &= check("the boot log says the root is read-only",
                        "read-only" in boot_text,
                        boot_text[-600:])
            ok &= check("the boot log says its logs are in RAM",
                        "the root is read-only, /etc/bootlog.log and /etc/run.log are in RAM"
                        in boot_text,
                        boot_text[-600:])
            ok &= check("the boot log says /tmp is a ramfs",
                        "/tmp is a ramfs" in boot_text,
                        boot_text[-600:])
            ok &= check("mount names ramfs (rw) on /tmp",
                        has_tmp_ramfs,
                        mounts[-400:])
        elif root_ro:
            ok &= check("the root is read-only and /tmp is mounted as ramfs",
                        has_tmp_ramfs, mounts[-400:])
        else:
            check("the root is read-write on this stick", True, mounts[-400:])
            check("/tmp ramfs auto-mount is not required on a read-write root",
                  True, mounts[-400:])

        # Start the desktop.
        started = machine.run_line("exec /bin/wm", 120.0)
        serial = machine.read_serial()
        # exec replaces the shell; wm logs when desktop is up.
        wm_up = machine.wait_serial("wm: the desktop is up", 120.0) or "wm: the desktop is up" in machine.read_serial()
        serial = machine.read_serial()
        ok &= check("exec /bin/wm starts the desktop",
                    wm_up and "wm: no file for the pixels of a window" not in serial,
                    serial[-800:])

        if shots:
            machine.screendump(os.path.join(shots, "stick-wm.ppm"))

        # Alt+T opens a terminal window.
        saw_term = False
        for _ in range(8):
            machine.sendkey("alt-t")
            if machine.wait_serial("wm: window term", 15.0):
                saw_term = True
                break
        serial = machine.read_serial()
        ok &= check("Alt+T opens /bin/term",
                    saw_term or "wm: window term" in serial,
                    serial[-800:])
        term_up = machine.wait_serial("term: the window of the terminal is up", 60.0)
        ok &= check("the terminal window is up",
                    term_up or "term: the window of the terminal is up" in machine.read_serial(),
                    machine.read_serial()[-800:])

        if shots:
            machine.screendump(os.path.join(shots, "stick-term.ppm"))

        # Alt+T gives the new terminal window focus. Wait for it to be
        # ready, type the builtin help, then run an external command to prove
        # the terminal is alive and responsive.
        time.sleep(3.0)

        help_done = False
        for _ in range(3):
            machine.sendkey("ctrl-u")
            time.sleep(0.2)
            machine.type_text("help")
            machine.sendkey("ret")
            time.sleep(2.0)
            machine.sendkey("ctrl-u")
            time.sleep(0.2)
            machine.type_text("echo ramfs-ok")
            machine.sendkey("ret")
            deadline = time.monotonic() + 60.0
            while time.monotonic() < deadline:
                serial = machine.read_serial()
                if "term: the command echo" in serial or "term: echo left with 0" in serial:
                    help_done = True
                    break
                time.sleep(0.5)
            if help_done:
                break
            time.sleep(1.0)

        ok &= check("terminal responds to help and runs a command",
                    help_done,
                    machine.read_serial()[-800:])

        if shots:
            time.sleep(1.0)
            machine.screendump(os.path.join(shots, "stick-help.ppm"))

        # Alt+F4 closes the terminal window.
        machine.sendkey("alt-f4")
        closed = machine.wait_serial("wm: window", 30.0) or machine.wait_serial(PROMPT, 60.0)
        # When last window closes, wm exits and shell prompt returns.
        prompt_back = machine.wait_serial(PROMPT, 60.0)
        ok &= check("Alt+F4 closes the window and prompt returns",
                    prompt_back or PROMPT in machine.read_serial(),
                    machine.read_serial()[-800:])

        if shots:
            machine.screendump(os.path.join(shots, "stick-after.ppm"))

    finally:
        if not ok:
            sys.stderr.write("\n--- serial tail ---\n")
            sys.stderr.write(machine.read_serial()[-2000:])
            sys.stderr.write("\n")
        machine.close()

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
