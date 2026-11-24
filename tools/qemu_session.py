#!/usr/bin/env python3
"""Micro OS — one boot of the x86-64 port under the control of a script.

The smoke tests of the port drive QEMU themselves, each with its own copy of
the read loop. The campaign of 1.0 needs the same loop in four tools (the
soak, the image builder, the disk stress and the network part of the soak),
so it lives here once: boot the machine, send a line only when the shell
prints its prompt, and hand the block between two prompts back to the caller.

    from qemu_session import Machine

    with Machine(build, image) as machine:
        banner = machine.boot()
        block = machine.run("uname")

The class does not decide what a good answer is: it only says whether the
machine answered, went away or stopped writing. The caller knows the
expectations of its own commands.

The prompt is the pacing signal on purpose. The serial FIFO of the emulated
chip is small, and a line typed while the shell is still answering the
previous one is a line it reads as two. Waiting for the prompt makes a
session of an hour as orderly as a session of a minute.
"""

from __future__ import annotations

import os
import select
import subprocess
import time

PROMPT = "microos> "
PROMPT_BYTES = PROMPT.encode("ascii")

# The buses a disk of the port can be on: the legacy IDE channel of ADR 0008,
# the virtio-blk device of ADR 0037, the SATA controller of a chipset
# (ADR 0041) and the NVMe controller of a modern laptop (ADR 0042). The image
# is one file and the bus is the machine, so a smoke test of the same image on
# another bus is the same bytes of the medium and another device under them.
DISKS = ("ide", "virtio", "ahci", "nvme")


def drive_args(image: str, disk: str = "ide") -> list:
    """The `-drive` arguments of a disk image on one of the buses.

    The SATA disk is attached to the first port of the AHCI controller of the
    machine (the bus `ide.0` of `-M q35`): the controller is the one the
    chipset has, and the drive is a device of that bus and not of the legacy
    channel.
    """
    if disk == "virtio":
        return ["-drive", f"file={image},format=raw,if=virtio,cache=writeback"]

    if disk == "ahci":
        return [
            "-drive", f"file={image},format=raw,if=none,id=ahci0,cache=writeback",
            "-device", "ide-hd,drive=ahci0,bus=ide.0",
        ]

    if disk == "nvme":
        return [
            "-drive", f"file={image},format=raw,if=none,id=nvme0,cache=writeback",
            "-device", "nvme,serial=microos,drive=nvme0",
        ]

    return ["-drive", f"file={image},format=raw,if=ide,index=0,cache=writeback"]


def machine_args(disk: str = "ide") -> list:
    """The `-machine` arguments the bus needs, empty for the default machine.

    The legacy channel and the virtio card are devices of the i440fx machine
    of the port; the SATA controller of a chipset is the one of the q35
    machine, which has no IDE channel at all - the disk of a laptop.
    """
    return ["-M", "q35"] if disk == "ahci" else []


def disk_name(disk: str = "ide") -> str:
    """The name the port gives the device of that bus ("ata0", "virtio0").

    The boot names the first partition of it by that name plus "p1", and the
    checks of a smoke test are about the words the port prints.
    """
    if disk == "virtio":
        return "virtio0"

    if disk == "ahci":
        return "ahci0"

    if disk == "nvme":
        return "nvme0"

    return "ata0"


def root_name(disk: str = "ide") -> str:
    """The first partition of the disk, which is the root of the boot."""
    return disk_name(disk) + "p1"


class MachineError(RuntimeError):
    """The machine is not there any more."""


class MachineClosed(MachineError):
    """QEMU went away before the session ended (a reset, a triple fault)."""


class MachineTimeout(MachineError):
    """The machine wrote nothing for longer than the caller allows."""


class Machine:
    """One QEMU process, one console, one shell."""

    def __init__(
        self,
        build: str,
        image: str,
        *,
        net: bool = False,
        hostfwd: int | None = None,
        memory: str = "256",
        disk: str = "ide",
        extra_args: list | None = None,
    ) -> None:
        self.build = build
        self.image = image
        self.disk = disk
        self.net = net
        self.hostfwd = hostfwd
        self.memory = memory
        self.extra_args = list(extra_args or [])
        self.process: subprocess.Popen | None = None
        self.transcript = b""
        self.pending = b""
        self.prompts = 0
        self.last_output = 0.0
        self.boot_seconds = 0.0

    # --- the command line of QEMU -------------------------------------

    def command(self) -> list:
        command = [
            "qemu-system-x86_64",
            "-kernel",
            os.path.join(self.build, "loader32.elf"),
            "-initrd",
            os.path.join(self.build, "microos64.bin"),
            "-m",
            self.memory,
            "-nographic",
            # A reset is the end of the machine, not a new boot: the session
            # of 1.0 is one boot, and a reboot is a failure of it.
            "-no-reboot",
        ]

        command += machine_args(self.disk)
        command += drive_args(self.image, self.disk)

        if self.net:
            forward = ""
            if self.hostfwd is not None:
                forward = f",hostfwd=tcp:127.0.0.1:{self.hostfwd}-:8080"
            command += [
                "-netdev",
                f"user,id=n0{forward}",
                # The legacy interface of virtio: the driver of the port
                # programs the I/O BAR and the split virtqueue.
                "-device",
                "virtio-net-pci,netdev=n0,disable-modern=on",
            ]

        command += self.extra_args
        return command

    # --- the console ---------------------------------------------------

    def open(self, timeout: float = 40.0) -> str:
        """Start QEMU and read until the first prompt; return the banner.

        The time from `open()` to the prompt is the boot of the machine, the
        number the soak reports. A boot that does not reach a shell in
        `timeout` seconds is a machine that hung before it could be asked
        anything, and the caller gets MachineTimeout (not an empty answer).
        """
        command = self.command()

        try:
            self.process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        except FileNotFoundError as error:
            raise MachineError("qemu-system-x86_64 is not installed") from error

        assert self.process.stdout is not None
        assert self.process.stdin is not None

        self.last_output = time.monotonic()
        started = self.last_output
        banner = self._read_until_prompt(timeout, hang=None)
        self.boot_seconds = time.monotonic() - started

        return banner

    def close(self) -> str:
        """Stop the machine and return whatever it wrote after the last read."""
        process = self.process
        if process is None:
            return ""

        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

        self._drain()
        assert process.stdout is not None
        assert process.stdin is not None
        process.stdout.close()
        process.stdin.close()
        self.process = None

        return self.transcript.decode("utf-8", errors="replace")

    def __enter__(self) -> "Machine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def send_line(self, text: str) -> None:
        """Type one line for the shell. The caller waits for its prompt."""
        process = self.process
        if process is None or process.stdin is None:
            raise MachineClosed("the machine is not running")

        process.stdin.write(text.encode("ascii", errors="replace") + b"\n")
        process.stdin.flush()

    def send_raw(self, data: bytes) -> None:
        """Type bytes that are not a line: Ctrl+C, for one."""
        process = self.process
        if process is None or process.stdin is None:
            raise MachineClosed("the machine is not running")

        process.stdin.write(data)
        process.stdin.flush()

    def wait_prompt(self, timeout: float = 20.0, hang: float | None = None) -> str:
        """Wait for the next prompt and return the block before it."""
        return self._read_until_prompt(timeout, hang)

    def run(self, command: str, timeout: float = 20.0, hang: float | None = None) -> str:
        """Type one command and return the block up to the prompt after it."""
        self.send_line(command)
        return self.wait_prompt(timeout, hang)

    def wait_for(self, marker: str, timeout: float = 5.0, hang: float | None = None) -> bool:
        """Read until `marker` arrives on the console; false on timeout.

        The soak needs it for the one key that is not a line: Ctrl+C is sent
        into the console while a program runs, and a byte typed at the prompt
        would be read as a line. Waiting for the first line of the program is
        the proof that its task exists. The bytes stay in `pending`, so the
        prompt after the program is read by `wait_prompt()` as usual.
        """
        process = self.process
        if process is None or process.stdout is None:
            raise MachineClosed("the machine is not running")

        descriptor = process.stdout.fileno()
        start = len(self.transcript)
        wanted = marker.encode("utf-8")
        deadline = time.monotonic() + timeout
        hang = timeout if hang is None else hang

        while wanted not in self.transcript[start:]:
            now = time.monotonic()
            if now >= deadline:
                return False
            if now - self.last_output >= hang:
                raise MachineTimeout(f"the machine wrote nothing for {hang:.0f} s")

            ready, _, _ = select.select([descriptor], [], [], 0.05)
            if not ready:
                continue

            chunk = os.read(descriptor, 4096)
            if not chunk:
                self._drain()
                raise MachineClosed("QEMU went away: reset, triple fault or exit")

            self.transcript += chunk
            self.pending += chunk
            self.last_output = time.monotonic()

        return True

    # --- the read loop --------------------------------------------------

    def _read_until_prompt(self, timeout: float, hang: float | None) -> str:
        process = self.process
        if process is None or process.stdout is None:
            raise MachineClosed("the machine is not running")

        descriptor = process.stdout.fileno()
        deadline = time.monotonic() + timeout
        hang = timeout if hang is None else hang

        while True:
            index = self.pending.find(PROMPT_BYTES)
            if index >= 0:
                block = self.pending[:index]
                self.pending = self.pending[index + len(PROMPT_BYTES) :]
                self.prompts += 1
                return block.decode("utf-8", errors="replace")

            now = time.monotonic()
            if now >= deadline:
                raise MachineTimeout(
                    f"the shell answered nothing in {timeout:.0f} s"
                )

            if now - self.last_output >= hang:
                raise MachineTimeout(
                    f"the machine wrote nothing for {hang:.0f} s"
                )

            ready, _, _ = select.select([descriptor], [], [], 0.05)
            if not ready:
                continue

            chunk = os.read(descriptor, 4096)
            if not chunk:
                self._drain()
                raise MachineClosed("QEMU went away: reset, triple fault or exit")

            self.transcript += chunk
            self.pending += chunk
            self.last_output = time.monotonic()

    def _drain(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            return
        descriptor = process.stdout.fileno()
        while True:
            ready, _, _ = select.select([descriptor], [], [], 0.05)
            if not ready:
                return
            chunk = os.read(descriptor, 4096)
            if not chunk:
                return
            self.transcript += chunk
