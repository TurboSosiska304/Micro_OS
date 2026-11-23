#!/usr/bin/env python3
"""Micro OS — the smoke test of the two-core x86-64 port.

`tools/x86_smoke.py` boots the kernel of one core and reads what it says.
This one boots the same machine with KERNEL_SMP on and two processors in QEMU
(ADR 0012), and its question is the one the kernel of many cores has to
answer: are the cores the firmware reports really started, does each of them
take its own ticks, and can the scheduler keep a task on one of them without
ever claiming it on both?

    tools/x86_smp_smoke.py [--build DIR] [--image PATH] [--timeout SECONDS]
                           [--seconds N] [--verbose]

The boot lines are the first half of the answer: two processors reported by
the MADT, two in use, and the kernel's own count of the cores it runs. The
second half is the `smp` command of the shell, which is the report of the
scheduler:

  * `smp` prints the cores, their APIC ids and their ticks, checks the run
    queues and says which core ran the command;
  * `smp workers 8` puts eight tasks on the machine and prints the core each
    of them ran on - the proof that both cores take work, and not only the
    one that boots;
  * `smp ping` sends an IPI to the other core and reads the count of the
    local APIC timer around it, which is the interrupt path a core of this
    machine needs to be preempted at all.

A second `smp` after the load is the check of the clock: the ticks of the
core 1 have to be strictly greater than in the first one, or the AP timer is
not running and the core 1 would only be a label. The queues are consistent
in both answers, and no line of the transcript says a core did not answer,
that an exception was raised, that the machine stops, or that the kernel
panicked. The machine also boots `/bin/hello` out of the disk in ring 3 and
reaches its shell, so a kernel that started two cores and lost the program
or the console fails here like it does in the smoke of one core.

The commands go out on the prompt, the way the other tools of the port type
them, and the session runs for at least `--seconds` seconds (15 by default)
so that the second `smp` reads a clock that has moved. The exit status is 0
when every check passed, 1 when one did not (the console of the machine is
printed then), and 2 when a file or QEMU is missing.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86-smp")
DEFAULT_IMAGE = os.path.join(ROOT, "build-x86", "programs.img")

# The prompt of the shell: the commands go out when it is printed, so a line
# is never typed while the shell is answering the one before it. The serial
# FIFO of the emulated chip is small and a line typed too early is a line it
# reads as two.
PROMPT = "microos> "

# The commands of the session and what each of them is for. The second `smp`
# is the one that reads the clock after the workers and the IPI, and its
# answer is compared with the first.
COMMANDS = [
    "smp",
    "smp workers 8",
    "smp ping",
    "smp",
]

# What the boot has to say before the shell exists: the loader verified the
# image, the port came up, the scheduler is there and the map is W^X. The
# lines are the same the smoke of one core reads (ADR 0020, ADR 0007).
EXPECTED = [
    "Micro OS: image signature ok (sha-256, ed25519)",
    "board: qemu_x86_64",
    "port: x86_64, contract",
    "[ ok ] sched",
    "subsystem(s) ready",
    "the kernel is up",
    "x86-64: the map is W^X",
    "MicroOS shell ready",
]

# The words that mean the machine of two cores did not come up whole. The
# first one is the sentence of the port for an AP that never answered the
# start-up IPI; it may not be in a transcript of a run that uses both cores.
FATAL = [
    "did not answer",
    "exception",
    "the machine stops here",
    "panic",
]

# The APIC timer and the APIC id of the lines of `smp`: the id is the one the
# port read out of the MADT, and the ticks are the count of the core itself.
# The port prints the id in hexadecimal (`apic 0x1`); the pattern takes the
# decimal form of the contract (`apic 1`) as well, so either spelling of the
# line is the line of that core.
SMP_CPU = re.compile(r"smp: cpu (\d+) apic (?:0x)?[0-9a-fA-F]+ ticks (\d+) current (\S+)")


def core_line(cpu: int) -> re.Pattern:
    """The line of one core of `smp`, with the APIC id in decimal or hex."""
    return re.compile(rf"smp: cpu {cpu} apic (?:0x)?0*{cpu:x} ticks \d+ current \S+")

# A strip of the escape sequences of a console that turns them on: the checks
# look for the text of a line and a colour in front of a word would hide it.
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")


def strip_ansi(text: str) -> str:
    """The text of the console without the escape sequences of a colour."""
    return ANSI.sub("", text)


def run_machine(loader: str, kernel: str, image: str, timeout: float, seconds: float,
                hang: float = 20.0) -> str:
    """Boot the two-core machine, drive the shell and return the transcript.

    The output is read while the machine runs; a command goes out only when
    the prompt of the one before is on the console. Two clocks are kept: the
    total deadline of the run, and the silence of the console while an answer
    is still owed - a machine that stops writing in the middle of a command
    is a hang and not a slow answer, and the checks below say what is missing
    instead of waiting out the whole timeout.

    After the last answer the machine is left at its prompt until the session
    is `seconds` old: the second `smp` has to read a clock that has moved,
    which is a fact about time and not only about the number of commands.
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
        # The two processors of the gate of ADR 0012. The firmware then lists
        # both in the MADT of ACPI and the port has a second core to start.
        "-smp",
        "2",
        "-nographic",
        "-no-reboot",
        # The disk of the machine: the programs of /bin are files of the
        # MicroFS volume in it, as in the smoke of one core (ADR 0008).
        "-drive",
        f"file={image},format=raw,if=ide,index=0,cache=writeback",
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
    text = ""
    sent = 0
    started = time.monotonic()
    last_output = started
    deadline = started + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.1)
            now = time.monotonic()

            if not ready:
                # An answer is still owed: a console that says nothing for
                # `hang` seconds is a machine that stopped, not a slow one.
                if text.count(PROMPT) <= len(COMMANDS) and now - last_output >= hang:
                    break

                # Every answer is in; the session is kept alive until it is
                # old enough for the second `smp` to be a check of the clock.
                if text.count(PROMPT) > len(COMMANDS) and now - started >= seconds:
                    break

                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                break  # the machine went away on its own

            transcript += chunk
            last_output = time.monotonic()
            text = strip_ansi(transcript.decode("utf-8", errors="replace"))

            # One command per prompt: the shell prints the prompt before it
            # reads, so the line is written only when the shell is waiting.
            while sent < len(COMMANDS) and text.count(PROMPT) > sent:
                process.stdin.write((COMMANDS[sent] + "\n").encode("ascii"))
                process.stdin.flush()
                sent += 1

            if text.count(PROMPT) > len(COMMANDS) and time.monotonic() - started >= seconds:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        # Whatever the machine wrote before it stopped is part of the answer.
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return strip_ansi(transcript.decode("utf-8", errors="replace"))


def check(output: str) -> list:
    """The expectations about the boot and the shell of the two-core machine."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the boot
    for line in EXPECTED:
        expect(line, line in output)

    # --- the processors the kernel uses (ADR 0012)
    expect(
        "the firmware lists both processors of the machine",
        re.search(r"x86-64: 2 cpu\(s\) reported by the firmware", output) is not None,
        "the MADT of ACPI was not read, or QEMU was not given -smp 2",
    )
    expect(
        "the kernel starts both of them",
        re.search(r"x86-64: 2 cpu\(s\) reported by the firmware, 2 in use", output) is not None,
        "the port reports two processors but uses fewer",
    )
    expect(
        "the scheduler counts the two cores",
        re.search(r"cpu: 2 core\(s\) of the machine are used by the kernel", output) is not None,
        "k_sched_init() was given a core count of one",
    )
    # A core that did not answer the start-up IPI is the failure the AP
    # bring-up exists to avoid: the boot may not say it and must not be quiet
    # about a core it left behind either. The sentence of the branch before
    # the bring-up ("not written yet") is a lie in a kernel that runs two.
    for word in FATAL:
        expect(
            f"the transcript has no '{word}'",
            word not in output,
            "the machine did not come up whole",
        )
    expect(
        "the boot does not say the AP bring-up is missing",
        "the AP bring-up is not written yet" not in output,
        "the port still prints the sentence of the one-core phase",
    )

    # --- the program of the boot and the shell
    expect("the program writes to the console", "PROGRAM: hello from /bin" in output)
    # The number is read from CS by the program itself: the proof that the
    # entry went through an iretq into ring 3 on this machine too.
    expect("the program runs in ring 3", "PROGRAM: cpl 3" in output)

    # The answers, separated by the prompt. The echo of the typed line is
    # part of the block; the checks below look for the answer itself.
    blocks = output.split(PROMPT)[1:]

    def answer(index: int) -> str:
        return blocks[index] if index < len(blocks) else ""

    expect(
        f"every command is answered ({len(COMMANDS)})",
        len(blocks) > len(COMMANDS),
        f"{len(blocks)} block(s) for {len(COMMANDS)} command(s)",
    )

    first = answer(0)
    # --- the report of the scheduler, before the load
    expect(
        "the command names the cores in use",
        re.search(
            r"smp: 2 core\(s\) in use of 2 reported, apic 0x[0-9a-fA-F]+", first
        )
        is not None,
        first,
    )
    expect(
        "the report has the line of the core 0",
        core_line(0).search(first) is not None,
        first,
    )
    expect(
        "the report has the line of the core 1",
        core_line(1).search(first) is not None,
        first,
    )
    expect("the report checks the queues", "smp: queues consistent" in first, first)
    expect(
        "the command says which core ran it",
        re.search(r"smp: the command ran on cpu [01]", first) is not None,
        first,
    )

    # The ticks of the core 1 in the first answer are the baseline of the
    # check of the timer; the line has to exist for the comparison below.
    first_ticks = {
        int(match.group(1)): int(match.group(2))
        for match in SMP_CPU.finditer(first)
    }

    # --- the workers: both cores take work
    workers = answer(1)
    expect(
        "a worker ran on the core 0",
        re.search(r"smp: worker \d+ on cpu 0", workers) is not None,
        workers,
    )
    expect(
        "a worker ran on the core 1",
        re.search(r"smp: worker \d+ on cpu 1", workers) is not None,
        workers,
    )
    expect(
        "every worker is done",
        re.search(r"smp: 8 worker\(s\) done", workers) is not None,
        workers,
    )

    # --- the interrupt path between the cores
    ping = answer(2)
    expect(
        "the ping names the core and the IPI count",
        re.search(r"smp: ping cpu \d+: ipi count \d+ -> \d+", ping) is not None,
        ping,
    )
    expect("the ping answers its own core", "smp: ping ok" in ping, ping)

    # --- the clock of the second core after the load
    last = answer(3)
    expect("the second report checks the queues", "smp: queues consistent" in last, last)
    last_ticks = {
        int(match.group(1)): int(match.group(2))
        for match in SMP_CPU.finditer(last)
    }
    for cpu in (0, 1):
        expect(
            f"the core {cpu} answered its ticks both times",
            cpu in first_ticks and cpu in last_ticks,
            f"first {first_ticks}, last {last_ticks}",
        )
        expect(
            f"the ticks of the core {cpu} moved",
            cpu in first_ticks and cpu in last_ticks and last_ticks[cpu] > first_ticks[cpu],
            f"the core {cpu}: {first_ticks.get(cpu)} -> {last_ticks.get(cpu)}",
        )

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the smoke test of the two-core x86-64 port")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the build with KERNEL_SMP on")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="the image with /bin of the disk")
    parser.add_argument("--timeout", type=float, default=60.0, help="the seconds the machine gets")
    parser.add_argument(
        "--seconds",
        type=float,
        default=15.0,
        help="the shortest session: the second smp reads a clock that moved",
    )
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(args.build, "loader32.elf")
    kernel = os.path.join(args.build, "microos64.bin")

    for path in (loader, kernel, args.image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-smp-build x86-disk", file=sys.stderr)
            return 2

    # The commands of the session do not write the volume, but the copy keeps
    # the image of the build untouched if a future command does; it is the
    # same rule the smoke of one core follows.
    run_image = os.path.join(args.build, "programs-smp-smoke.img")
    shutil.copyfile(args.image, run_image)

    try:
        output = run_machine(loader, kernel, run_image, args.timeout, args.seconds)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        if os.path.exists(run_image):
            os.remove(run_image)

    if args.verbose:
        print(output)

    checks = check(output)
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the smoke test of the two-core x86-64 port")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail.replace(chr(13), '')}")

    print("=" * 60)
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
