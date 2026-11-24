#!/usr/bin/env python3
"""Micro OS — the boot of the x86-64 medium on a firmware, from a CD and a stick.

`tools/x86_iso_smoke.py` boots the image in the machine of a BIOS, from a CD
drive. This is the other half of the medium: the same file started by a
firmware of UEFI, which reads no catalogue of El Torito first — it looks for a
FAT partition of the image and starts `EFI/BOOT/BOOTX64.EFI` from it — and the
same file written to a USB stick, which is how a person boots a machine that
has no drive at all.

    python3 tools/x86_iso_uefi_smoke.py [--iso PATH] [--firmware uefi|bios]
                                       [--media cdrom|usb|both]
                                       [--ovmf-code PATH] [--ovmf-vars PATH]
                                       [--timeout SECONDS] [--keep]

Four boots, and each of them is a machine of QEMU:

    uefi + cdrom   OVMF and a DVD drive    the El Torito record of the EFI
    uefi + usb     OVMF and a USB stick    the ESP of the GPT, found by the
                                           firmware on a disk it was not told
                                           about
    bios + cdrom   SeaBIOS and a DVD drive the El Torito record of the BIOS
    bios + usb     SeaBIOS and a USB stick the MBR written by
                                           `limine bios-install`

The medium is not modified and the firmware is not told what to boot: the
machine is started with the image and nothing else, and the transcript is read
for the boot (the loader verified the kernel, the root image was mounted, the
program of /bin ran) and for the shell (the prompt, `uname`, `ls /bin`,
`cat /etc/setting`). The exit status is 0 when every boot of the run reached
the shell and answered, 1 otherwise, and 2 when a tool of the machine is
missing.

OVMF is the EDK2 build of the distribution: /usr/share/edk2/x64 (Arch,
CachyOS), /usr/share/OVMF (Debian, Fedora) and /usr/share/edk2/ovmf. The code
is read-only and the variables are a copy in a directory of the run, so that a
boot does not teach the next one what to do.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86-iso")
DEFAULT_ISO = os.path.join(BUILD, "microos.iso")

PROMPT = "microos> "

# The questions of a person at the shell of a machine that came from a stick:
# the kernel, the programs, and the settings the root image carries.
COMMANDS = [
    "uname",
    "ls /bin",
    "cat /etc/setting",
]

# The firmwares of this machine, code and variables. The first pair that is
# there is the one of the run; --ovmf-code and --ovmf-vars name another.
OVMF_DIRS = ["/usr/share/edk2/x64", "/usr/share/OVMF", "/usr/share/edk2/ovmf"]
OVMF_CODE = ["OVMF_CODE.4m.fd", "OVMF_CODE.fd", "OVMF_CODE_4M.fd"]
OVMF_VARS = ["OVMF_VARS.4m.fd", "OVMF_VARS.fd", "OVMF_VARS_4M.fd"]


def find_ovmf(names: list, explicit: str | None, env: str) -> str | None:
    """The first file of `names` in the firmware directories, or the flag."""
    override = explicit or os.environ.get(env)

    if override:
        return override if os.path.isfile(override) else None

    for directory in OVMF_DIRS:
        for name in names:
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                return path

    return None


def firmware_arguments(code: str, variables: str) -> list:
    """The two flash chips of the machine of OVMF: the code and its variables."""
    return [
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive",
        f"if=pflash,format=raw,file={variables}",
    ]


def medium_arguments(iso: str, media: str) -> list:
    """The image as the medium it is on: a CD drive or a USB stick.

    The two are not the same machine. A CD is handed to the firmware by the
    catalogue of El Torito, and `-drive media=cdrom` puts the image behind an
    ATAPI drive; a stick is a disk the firmware has to find for itself, and
    `usb-storage` puts the image behind a controller that has to be probed and
    a partition table that has to be read. A boot from the second one is the
    proof that the image is a stick.
    """
    if media == "cdrom":
        return [
            "-drive",
            f"if=none,id=cd0,file={iso},format=raw,media=cdrom",
            "-device",
            "ide-cd,drive=cd0",
        ]

    return [
        "-device",
        "qemu-xhci,id=xhci",
        "-drive",
        f"if=none,id=usbdisk,file={iso},format=raw",
        "-device",
        "usb-storage,drive=usbdisk,bus=xhci.0",
    ]


def run_machine(iso: str, firmware: str, media: str, code: str | None, variables: str | None,
                timeout: float) -> str:
    """Boot the image on the firmware of the run and drive the shell."""
    command = ["qemu-system-x86_64"]

    if firmware == "uefi":
        assert code is not None and variables is not None
        command += firmware_arguments(code, variables)

    command += medium_arguments(iso, media)
    command += [
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
        # A BIOS tries the stick before the floppy and the empty disk of the
        # machine; the firmware of UEFI reads its own boot order, which a fresh
        # copy of the variables builds out of the devices it finds.
        "-boot",
        "c" if media == "usb" else "d",
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    fd = process.stdout.fileno()
    transcript = b""
    sent = 0
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                break

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            # One command per prompt: the shell prints the prompt before it
            # reads, so the line goes out only when the shell waits for it.
            while sent < len(COMMANDS) and text.count(PROMPT) > sent:
                process.stdin.write((COMMANDS[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            if text.count(PROMPT) > len(COMMANDS):
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


def answers(output: str, commands: list) -> list:
    """The answer of every command, split by the prompt of the shell."""
    blocks = output.split(PROMPT)[1:]
    return [blocks[index] if index < len(blocks) else "" for index in range(len(commands))]


def check(output: str, firmware: str, media: str) -> list:
    """The boot of one medium, line by line.

    The names of the lines are the same for the four machines: the way in is
    the only part that changes with the firmware and with the medium, and the
    march from the loader to the shell is the same one `x86-iso-smoke` reads
    from a BIOS and a CD.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the way in: the firmware found the medium and started the loader
    if firmware == "uefi":
        started = re.search(r"BdsDxe: starting Boot\w+ \"([^\"]*)\"", output)
        device = started.group(1) if started else ""
        if media == "usb":
            expect(
                "the firmware started the image from the USB stick",
                "USB" in device,
                "the firmware was not told what to boot, and the device it started "
                f"is {device or 'none'}: no ESP was found on the stick",
            )
        else:
            expect(
                "the firmware started the image from the DVD drive",
                "DVD" in device,
                "the firmware started " + (device or "nothing"),
            )
    else:
        expect(
            "the BIOS started the image from the disk it was told to boot",
            re.search(r"Booting from (Hard Disk|DVD/CD)", output) is not None,
            "the BIOS did not boot the medium",
        )

    expect(
        "the loader of the image started and verified the kernel",
        "Micro OS: image signature ok" in output,
        "the boot did not reach the loader of the port",
    )
    expect(
        "the kernel of the port came up",
        re.search(r"MicroOS \d+\.\d+\.\d+", output) is not None,
    )
    expect(
        "neither module of the boot lies over the kernel image",
        "a module of the boot lies over the kernel image" not in output,
        "the loader refused the layout of the medium",
    )

    # --- the medium: a read-only volume, mounted from the second module
    expect(
        "the boot found the root image of the medium",
        "x86-64: the boot carries a read-only volume" in output,
        "the second module of the boot is not the root image",
    )
    expect(
        "the root of the machine is the volume of the medium",
        "x86-64: the root volume is boot0p1" in output,
    )
    expect(
        "the volume of the medium is not formatted again",
        "is not formatted yet" not in output,
        "the boot formatted the volume of the boot medium",
    )
    expect(
        "the boot ran a program out of the medium",
        "PROGRAM: hello from /bin" in output and "the program exited with 0" in output,
        "the program of /bin did not run at the boot",
    )

    # --- the shell of the machine that came from the medium
    expect(
        "the machine reached the shell",
        PROMPT in output,
        f"the machine wrote {len(output)} byte(s) and stopped"
        + stopped_hint(output),
    )

    replies = answers(output, COMMANDS)
    expect("every command is answered", all(replies), "a command got no answer")
    expect(
        "uname names the system of the image",
        # The suffix of a candidate is part of the name (`1.8.0-rc3`): the
        # first release that carried its own medium was a candidate, and this
        # check met it for the first time with nothing to match it.
        re.search(r"MicroOS \d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)? x86_64", replies[0])
        is not None,
        replies[0],
    )
    expect(
        "the programs came with the root image",
        all(re.search(rf"(?m)^\s*{name}\r?$", replies[1]) is not None
            for name in ("hello", "cat", "ls")),
        replies[1],
    )
    expect(
        "the setting came with the root image",
        "hello" in replies[2] and "hostname=x86_64" in replies[2],
        replies[2],
    )

    return checks


def stopped_hint(output: str) -> str:
    """The sentence of a boot that reached the kernel and then stopped.

    The one boot this port has in the kernel of this release is the heap of
    the allocator over the arena of the programs: the boot loader of the
    firmware leaves the modules in 32..80 MiB, the heap starts above them and
    runs into the physical arena at 64 MiB, and the first program that is
    loaded is written over the blocks of the kernel. The transcript of such a
    boot ends after `task: created 'program'`, which is what this says.
    """
    if "task: created 'program'" in output and "PROGRAM: hello from /bin" not in output:
        return ("\n      the boot stopped as the first program was created: the heap of the port "
                "lies over the arena of the programs (docs/ports/x86_64/boot.md, the debt of the "
                "firmware boot)")

    return ""


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the boot of the image on a firmware and a medium")
    parser.add_argument("--iso", default=DEFAULT_ISO)
    parser.add_argument("--firmware", choices=["uefi", "bios"], default="uefi")
    parser.add_argument("--media", choices=["cdrom", "usb", "both"], default="both")
    parser.add_argument("--ovmf-code", default=None)
    parser.add_argument("--ovmf-vars", default=None)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--keep", action="store_true", help="keep the transcript of the run")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.iso):
        print(f"missing {args.iso}: run make x86-iso", file=sys.stderr)
        return 2

    if not shutil.which("qemu-system-x86_64"):
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    code = None
    variables = None

    if args.firmware == "uefi":
        code = find_ovmf(OVMF_CODE, args.ovmf_code, "OVMF_CODE")
        variables = find_ovmf(OVMF_VARS, args.ovmf_vars, "OVMF_VARS")

        if code is None or variables is None:
            print("the firmware of UEFI is not installed: OVMF_CODE.4m.fd and "
                  "OVMF_VARS.4m.fd are in /usr/share/edk2/x64, /usr/share/OVMF or "
                  "/usr/share/edk2/ovmf", file=sys.stderr)
            print("(Arch: sudo pacman -S edk2-ovmf; Debian: sudo apt install ovmf)",
                  file=sys.stderr)
            return 2

    media = ["cdrom", "usb"] if args.media == "both" else [args.media]
    failed = 0
    total = 0
    workdir = tempfile.mkdtemp(prefix="microos-iso-")

    try:
        for medium in media:
            # A fresh copy of the variables for every boot: the firmware of one
            # run must not tell the next one where the image was.
            boot_vars = None
            if variables is not None:
                boot_vars = os.path.join(workdir, f"vars-{medium}.fd")
                shutil.copyfile(variables, boot_vars)

            output = run_machine(args.iso, args.firmware, medium, code, boot_vars, args.timeout)

            if args.keep:
                path = args.iso + f".{args.firmware}-{medium}.transcript"
                with open(path, "w", encoding="utf-8") as file:
                    file.write(output)
                print(f"the transcript is in {path}")

            checks = check(output, args.firmware, medium)
            total += len(checks)

            for name, ok, detail in checks:
                print(f"{'ok  ' if ok else 'FAIL'}  [{medium}] {name}")

                if not ok:
                    failed += 1

                    if detail:
                        print(f"      {detail}")

            if failed:
                print(f"\n{args.firmware}/{medium}: the machine wrote {len(output)} byte(s)")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failed:
        print(f"\n{failed} check(s) failed of {total}")
        return 1

    firmware_name = "UEFI" if args.firmware == "uefi" else "BIOS"
    print(f"\nthe medium boots on the {firmware_name}: {total} check(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
