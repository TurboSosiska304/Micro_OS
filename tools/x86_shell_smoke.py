#!/usr/bin/env python3
"""Micro OS — the shell smoke: lists, redirects and job control.

The general x86 smoke proves the boot and a long list of commands. This one
drives the keys a transcript of lines cannot express: Ctrl+Z stops the
foreground job, bg continues it, and Ctrl+C ends it without taking the shell
down. It also checks the list operators, short-circuiting, 2>&1, a pipe and a
faulting program followed by a living prompt.

    tools/x86_shell_smoke.py [--image PATH] [--build DIR] [--timeout SECONDS]

The exit status is 0 when every check passed, 1 otherwise, and 2 when QEMU or
the built machine is missing. QEMU must be free: the script does not stop a
machine somebody else is running.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile

from qemu_session import Machine, MachineError


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_IMAGE = os.path.join(BUILD, "programs.img")


def tidy(text: str) -> str:
    """The serial console ends a line with CRLF; the checks are about text."""
    return text.replace("\r", "")


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the shell smoke of the x86-64 port")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the MicroFS image with /bin")
    parser.add_argument("--build", default=BUILD, help="the x86-64 build directory")
    parser.add_argument("--disk", default="ide", help="the disk bus: ide, virtio, ahci or nvme")
    parser.add_argument("--timeout", type=float, default=90.0, help="seconds for the whole session")
    args = parser.parse_args(argv)

    loader = os.path.join(args.build, "loader32.elf")
    kernel = os.path.join(args.build, "microos64.bin")
    for path in (loader, kernel, args.image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build and make x86-disk", file=sys.stderr)
            return 2

    if shutil.which("qemu-system-x86_64") is None:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    checks: list[tuple[str, bool, str]] = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    temporary = tempfile.NamedTemporaryFile(
        prefix="microos-shell-smoke-", suffix=".img", dir="/tmp/opencode", delete=False
    )
    image = temporary.name
    temporary.close()
    shutil.copyfile(args.image, image)

    try:
        with Machine(args.build, image, disk=args.disk, extra_args=["-smp", "2"]) as machine:
            machine.open(timeout=min(40.0, args.timeout))

            block = machine.run("echo list-one; echo list-two")
            expect("semicolon runs both commands", "list-one\nlist-two" in tidy(block), block)
            block = machine.run("echo list-three && echo list-four")
            expect("&& runs the right side after success", "list-three\nlist-four" in tidy(block), block)
            block = machine.run("cat /shell-missing && echo list-forbidden")
            expect("&& short-circuits a failure", tidy(block).count("list-forbidden") == 1, block)
            block = machine.run("cat /shell-missing || echo list-recovered")
            expect("|| recovers a failure", "list-recovered" in block, block)

            block = machine.run("echo builtin > /tmp/shell-out")
            expect("a builtin obeys >", "not supported" not in block, block)
            block = machine.run("cat /tmp/shell-out")
            expect("the builtin redirection wrote the file", "builtin" in block, block)
            machine.run("cat < /shell-missing > /tmp/shell-err 2>&1")
            block = machine.run("cat /tmp/shell-err")
            expect("2>&1 redirects stderr", "/shell-missing" in block, block)

            block = machine.run("cat /etc/setting | cat")
            expect("a pipe still carries the stream", "hostname=x86_64" in block, block)

            launch = machine.run("sleep 3000 &")
            found = re.search(r"\[(\d+)\]\s+(\d+)", launch)
            expect("background launch prints [job] pid", found is not None, launch)
            if found is None:
                raise MachineError("the background job did not print its number and pid")

            number = found.group(1)
            block = machine.run("jobs")
            expect("jobs shows the running command", "Running" in block and "sleep 3000" in block, block)

            machine.send_line(f"fg %{number}")
            expect("fg announces the continued job", machine.wait_for("Continued", timeout=5.0))
            machine.send_raw(b"\x1a")
            block = machine.wait_prompt(timeout=10.0)
            expect("Ctrl+Z gives the terminal back", "^Z" in block, block)

            block = machine.run("jobs")
            expect("jobs shows the stopped command", "Stopped" in block, block)
            block = machine.run(f"bg %{number}")
            expect("bg continues the job", "Running" in block, block)
            block = machine.run("jobs")
            expect("jobs shows the background job running", "Running" in block, block)

            machine.send_line(f"fg %{number}")
            expect("fg takes the job back", machine.wait_for("Continued", timeout=5.0))
            machine.send_raw(b"\x03")
            block = machine.wait_prompt(timeout=10.0)
            expect("Ctrl+C returns the prompt", block != "", block)
            expect("the shell is alive after Ctrl+C", machine.alive())
            block = machine.run("jobs")
            expect("the interrupted job is finished", "Done" in block, block)

            block = machine.run("exec /bin/crash")
            expect("the faulting program starts", "PROGRAM: crash writes to address zero" in block, block)
            block = machine.run("echo shell-alive")
            expect("a program fault does not take the shell", "shell-alive" in block, block)
    except (MachineError, OSError) as error:
        print(f"shell smoke: {error}", file=sys.stderr)
        return 1 if not isinstance(error, FileNotFoundError) else 2
    finally:
        if os.path.exists(image):
            os.remove(image)

    failed = [check for check in checks if not check[1]]
    for name, ok, detail in failed:
        print(f"FAIL: {name}: {detail[-500:]}")

    print(f"shell smoke: {len(checks) - len(failed)}/{len(checks)} check(s) passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
