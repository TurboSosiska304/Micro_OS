#!/usr/bin/env python3
"""Micro OS — the fork of a program of 64 MiB (ADR 0057).

`fork()` copies the page tables of the parent and shares its frames: a page is
copied the first time either side writes it, and the child reads the memory of
the parent as it was at the call. The promise of 2.0 is that this is cheap - a
program of 64 MiB forks within a millisecond - and this tool is the gate of
it, on the machine:

    exec /bin/forksmoke

The program maps 64 MiB of anonymous memory, writes one byte of every page,
forks, writes four pages in the child and reads the same four in the parent.
The port writes the line of the call:

    x86-64: fork (pid N) shares 16404 page(s) in 342 us (tables 322 us)

and one line for every page that the fork did not copy and a write did:

    x86-64: copy on write of 0x0000000011003000, the page the fork shared

The checks: the count of the shared pages is the memory of the program and not
a few pages of it; the time of the call is under the ceiling of a millisecond;
the parent still sees what it wrote before the fork and the child sees its own
writes; the child ends with 0; the pages the child wrote were copied (a line
each); and the shell answers after the program, which is to say the machine
did not stop.

    tools/x86_fork_smoke.py [--image PATH] [--build DIR] [--cpu CPU]
                            [--timeout SECONDS] [--verbose]

The exit status is 0 when every check held, 1 when one did not and 2 when the
image or the kernel is missing.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qemu_session import Machine, MachineError  # noqa: E402

# The promise of M2 of 2.0: the call of a fork of 64 MiB is a millisecond.
CEILING_US = 1000

# The pages of the program: 64 MiB of anonymous memory, the image, the stack
# and the vectors of the entry. The mapping is 16384 pages and the rest is a
# score, so a count below this is a fork that did not share the memory.
MIN_SHARED = 16384

# The line of the port, and the line of the pages a task copied on write.
FORK_LINE = re.compile(r"x86-64: fork \(pid (\d+)\) shares (\d+) page\(s\) in (\d+) us "
                       r"\(tables (\d+) us\)")
COPY_LINE = re.compile(r"x86-64: the task \(pid (\d+)\) copied (\d+) page\(s\) on write")


class Report:
    def __init__(self, verbose: bool = False) -> None:
        self.checks: list[tuple[str, bool, str]] = []
        self.verbose = verbose

    def expect(self, name: str, condition: bool, detail: str = "") -> bool:
        self.checks.append((name, bool(condition), detail))

        if self.verbose or not condition:
            mark = "ok  " if condition else "FAIL"
            print(f"  [{mark}] {name}")

            if not condition and detail:
                for line in str(detail).strip().splitlines()[-12:]:
                    print(f"         {line}")

        return bool(condition)

    def failed(self) -> list:
        return [check for check in self.checks if not check[1]]


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the fork of a program of 64 MiB (ADR 0057)")
    parser.add_argument("--image", default=os.path.join("build-x86", "programs.img"))
    parser.add_argument("--build", default="build-x86")
    parser.add_argument("--cpu", default="qemu64")
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not os.path.exists(args.image):
        print(f"fork-smoke: no image at {args.image}; run `make x86-disk`", file=sys.stderr)
        return 2

    if not os.path.exists(os.path.join(args.build, "microos64.bin")):
        print(f"fork-smoke: no kernel in {args.build}; run `make x86-build`", file=sys.stderr)
        return 2

    report = Report(args.verbose)

    try:
        with Machine(args.build, args.image, extra_args=["-cpu", args.cpu]) as machine:
            banner = machine.open(args.timeout)
            report.expect("the machine boots to a shell", "the kernel is up" in banner, banner)

            block = machine.run("exec /bin/forksmoke", args.timeout)
            if args.verbose:
                print(block)

            report.expect("the program starts with 64 MiB in 16384 pages",
                          "PROGRAM: forksmoke starts, 67108864 bytes of memory in 16384 pages"
                          in block, block)
            report.expect("the warm-up fork is done", "the warm-up fork is done" in block, block)

            # The line of the call: the warm-up shares a score of pages and the
            # fork of 64 MiB shares all of them. The latter is the one under
            # the ceiling.
            forks = [
                (int(pid), int(shared), int(us), int(tables))
                for pid, shared, us, tables in FORK_LINE.findall(block)
            ]
            big = [fork for fork in forks if fork[1] >= MIN_SHARED]

            report.expect("the fork of the 64 MiB is reported", len(big) == 1,
                          f"{len(forks)} fork(s): {forks}")

            if big:
                pid, shared, us, tables = big[0]
                print(f"fork-smoke: the fork of {shared} page(s) took {us} us "
                      f"(the ceiling is {CEILING_US})")
                report.expect("the fork shares the memory of the program",
                              shared >= MIN_SHARED, f"{shared} page(s)")
                report.expect("the fork of 64 MiB is under a millisecond",
                              us < CEILING_US, f"{us} us")
                report.expect("the tables are the part of the call that matters",
                              tables <= us and tables * 2 >= us,
                              f"the tables {tables} us of the call {us} us")

            # The pages the child wrote are copies, and the child says the
            # count of them when it ends (the kernel's line, one per task and
            # not one per page).
            copies = COPY_LINE.findall(block)
            best = max((int(count) for _, count in copies), default=0)
            report.expect("the pages the child wrote are copied on write", best >= 4,
                          f"the copied counts are {copies}")

            report.expect("the parent sees the old pages and the child its writes",
                          "PROGRAM: the parent sees the old pages, the child its own writes"
                          in block, block)
            report.expect("the child ends with 0", "PROGRAM: the child ended with 0" in block, block)
            report.expect("the program ends with 0", "the program exited with 0" in block, block)
            report.expect("the fork of 64 MiB is a copy of the pages",
                          "PROGRAM: the fork of 64 MiB is a copy of the pages and the two sides are "
                          "two" in block, block)

            alive = machine.run("exec /bin/echo alive", args.timeout)
            report.expect("the shell runs a program after the fork", "alive" in alive, alive)
    except MachineError as error:
        print(f"fork-smoke: {error}", file=sys.stderr)
        return 1

    failed = report.failed()

    for name, _, detail in failed:
        print(f"fork-smoke: FAIL {name}")

        if detail:
            for line in str(detail).strip().splitlines()[-6:]:
                print(f"    {line}")

    print(f"fork-smoke: {len(report.checks) - len(failed)} of {len(report.checks)} checks passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
