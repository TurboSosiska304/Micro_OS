#!/usr/bin/env python3
"""Micro OS — the policy of a page on x86-64 (ADR 0052): the attack suite.

The port has had page tables since 1.0 (ADR 0007): a program is a task of
ring 3 with a space of its own, the text is read-only, every data page is NX,
and the boot reads the tables back. What it did not have was a *policy* that
covers every page a program can reach and every way it can be wrong. This tool
is the gate of that policy, and every check below is one line of the machine:

  1. the boot says what the processor has - SMEP and SMAP on this machine of
     QEMU (`-cpu qemu64,+smep,+smap`), the supervisor bit of every kernel page
     otherwise - and the map of the kernel is W^X;
  2. a program that writes its own text dies with EFAULT, the address in the
     log and the error code of a write, and its task dies with 255;
  3. a program that jumps into its own stack dies with EFAULT and the error
     code of an instruction fetch, because the stack is data;
  4. a program that writes the kernel address dies with EFAULT and the error
     code of a present page - the supervisor bit is what stopped it, not a
     hole in the map;
  5. a program that touches a page it does not own dies with EFAULT and the
     address in the log;
  6. a program that asks `mmap` for a writable and executable page is refused
     with EINVAL and keeps running, and the same page with one bit less is a
     mapping it can use;
  7. a program that grows its heap and its mappings past the memory limit of
     its task is answered ENOMEM by both calls and keeps running;
  8. a program that uses 128 KiB of stack gets them: the port grows the stack
     by the pages of the frame, the line of the growth is in the log and every
     byte of the array reads back;
  9. a read and a write of a pipe that sleep in the middle of the copy keep
     the window of SMAP open on the context that opened it, and the next task
     does not inherit it;
 10. after every death the shell is alive and a program still runs: the fault
     took the task and not the machine;
 11. the W^X audit of every program that runs leaves its line in the log, and
     not one of them is refused.

Each attack is a program of the image (`ports/port_x86_64/programs/mmu_*.c`)
and not a command of the shell: the machine of the release carries them, so a
person can type the same line and read the same answer.

    tools/x86_mmu_smoke.py [--image PATH] [--build DIR] [--cpu CPU]
                           [--timeout SECONDS] [--verbose]

The exit status is 0 when every check held, 1 when one did not and 2 when the
image or QEMU is missing.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qemu_session import Machine, MachineError  # noqa: E402

# The audit line of every `exec` (ADR 0007 §9) and the boot's own map.
AUDIT = "x86-64: the map of a program is W^X"
AUDIT_BAD = "the map of the program is not W^X"
BOOT_AUDIT = "x86-64: the map is W^X"

# The processor of the suite. QEMU's qemu64 is the machine of every other
# smoke of the port and has neither bit of 2014; the two features are added
# here so that the gate of the policy runs on the processor that has them.
# `--cpu qemu64` runs the same suite on a machine whose boundary is only the
# supervisor bit, which is the other half of the promise.
DEFAULT_CPU = "qemu64,+smep,+smap"

# The line that proves the machine still runs a program after a fault.
ALIVE = "exec /bin/echo alive"


class Report:
    """The checks of the suite, in the order they were made."""

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


def check_death(report: Report, block: str, name: str, code: str, page: str,
                forbidden: str) -> None:
    """The common shape of an attack that ends its program.

    `code` is the error code of the page fault the processor pushed, as the
    port prints it, and `page` the beginning of the address CR2 held - the
    exact address is the frame of the program and moves with the compiler, the
    page it is in is what the attack is about. The lines are the death of the
    program, the reason the policy gives it and the status of the task;
    `forbidden` is the line the program would have printed if the page had
    done what it asked.
    """
    report.expect(f"{name}: the program starts", f"PROGRAM: {name} starts" in block, block)
    report.expect(f"{name}: the page fault of the attack is a page fault",
                  "the program faulted: vector 14" in block, block)
    report.expect(f"{name}: the access is the one the attack asked for",
                  f"error 0x{code:0>16}" in block, block)
    report.expect(f"{name}: the fault names the page {page}",
                  f"the program faulted: vector 14 at 0x{page}" in block, block)
    report.expect(f"{name}: the answer is EFAULT with the address in the log",
                  "EFAULT, the task ends, the machine stays" in block
                  and f"touched 0x{page}" in block, block)
    report.expect(f"{name}: the task ends with the status of a fault",
                  "the program exited with 255" in block, block)
    report.expect(f"{name}: the page did not do what the program asked",
                  forbidden not in block, block)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the attack suite of the MMU policy (ADR 0052)")
    parser.add_argument("--image", default=os.path.join("build-x86", "programs.img"))
    parser.add_argument("--build", default="build-x86")
    parser.add_argument("--cpu", default=DEFAULT_CPU)
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if not os.path.exists(args.image):
        print(f"mmu-smoke: no image at {args.image}; run `make x86-disk`", file=sys.stderr)
        return 2

    if not os.path.exists(os.path.join(args.build, "microos64.bin")):
        print(f"mmu-smoke: no kernel in {args.build}; run `make x86-build`", file=sys.stderr)
        return 2

    report = Report(args.verbose)
    execs = 0

    try:
        with Machine(args.build, args.image, extra_args=["-cpu", args.cpu]) as machine:
            banner = machine.open(args.timeout)
            execs += 1  # the boot runs /bin/hello through the call

            # --- the machine and its map ---------------------------------
            report.expect("the boot reaches the shell", "the kernel is up" in banner, banner)
            report.expect("the map of the kernel is W^X", BOOT_AUDIT in banner, banner)
            report.expect("the boot names the policy of the processor",
                          "SMEP and SMAP are off by policy" in banner
                          or "SMEP and SMAP are on" in banner
                          or "SMEP is on" in banner or "SMAP is on" in banner
                          or "the supervisor bit of every kernel page is the boundary" in banner,
                          banner)

            if "+smep" in args.cpu and "+smap" in args.cpu:
                report.expect("the processor of the suite cleared SMEP and SMAP",
                              "smep/smap off" in banner
                              and "SMEP and SMAP are off by policy" in banner, banner)

            # --- the attacks that end their program -----------------------
            text = machine.run("exec /bin/mmu_text", args.timeout)
            execs += 1
            report.expect("mmu_text: the program prints where its text is",
                          "the text is at 0x" in text, text)
            check_death(report, text, "mmu_text", "7", "0000000010", "the text took the write")

            stack = machine.run("exec /bin/mmu_execstack", args.timeout)
            execs += 1
            check_death(report, stack, "mmu_execstack", "15", "0000000015ff",
                        "the stack page executed")

            kernel = machine.run("exec /bin/mmu_kernel", args.timeout)
            execs += 1
            check_death(report, kernel, "mmu_kernel", "7", "ffffffff80000000",
                        "the kernel text took the write")
            report.expect("mmu_kernel: the page of the kernel is present and supervisor",
                          "error 0x0000000000000006" not in kernel, kernel)

            stray = machine.run("exec /bin/mmu_stray", args.timeout)
            execs += 1
            check_death(report, stray, "mmu_stray", "6", "0000000014000000",
                        "the stray page took the write")

            # --- the two calls that answer and let the program live --------
            heap = machine.run("exec /bin/mmu_heap", args.timeout)
            execs += 1
            report.expect("mmu_heap: the program starts", "PROGRAM: mmu_heap starts" in heap, heap)
            report.expect("mmu_heap: the limit of the task is the lowered one",
                          "PROGRAM: the limit is 131072 bytes" in heap, heap)
            report.expect("mmu_heap: brk past the limit answers ENOMEM",
                          "PROGRAM: brk past the limit answers 12" in heap, heap)
            report.expect("mmu_heap: mmap past the limit answers ENOMEM",
                          "PROGRAM: mmap past the limit answers 12" in heap, heap)
            report.expect("mmu_heap: the program is alive after both refusals",
                          "the heap stops at the limit with ENOMEM and the program is alive" in heap
                          and "the program exited with 0" in heap, heap)
            report.expect("mmu_heap: the heap of the kernel is not memory of a program",
                          "malloc of the kernel heap answers 0" in heap
                          and "realloc of a kernel block answers 0" in heap
                          and "free of a guessed kernel address answers 0" in heap
                          and "the heap of the kernel is not memory of the program" in heap, heap)

            wx = machine.run("exec /bin/mmu_wx", args.timeout)
            execs += 1
            report.expect("mmu_wx: a writable and executable page is refused",
                          "PROT_WRITE|PROT_EXEC is refused with 22" in wx, wx)
            report.expect("mmu_wx: a writable page with one bit less is a mapping",
                          "PROGRAM: the data mapping is at 0x" in wx, wx)
            report.expect("mmu_wx: the program is alive after the refusal",
                          "a page is code or it is data, and the call says so" in wx
                          and "the program exited with 0" in wx, wx)

            grown = machine.run("exec /bin/mmu_stack", args.timeout)
            execs += 1
            report.expect("mmu_stack: the stack grows on the fault",
                          "the stack of the program (pid" in grown
                          and "grew by" in grown and "page(s)" in grown, grown)
            report.expect("mmu_stack: 128 KiB of stack work",
                          "the stack grew by bytes 131072" in grown
                          and "a stack of 128 KiB above the first page works" in grown
                          and "the program exited with 0" in grown, grown)
            report.expect("mmu_stack: every page of the array reads back",
                          "PROGRAM: the pages of the stack read back 528" in grown, grown)

            io = machine.run("exec /bin/mmu_io", args.timeout)
            execs += 1
            report.expect("mmu_io: the read of a pipe survives the sleep in the copy",
                          "the read of the pipe came back after the sleep" in io, io)
            report.expect("mmu_io: the write of a pipe survives the sleep in the copy",
                          "the write of the pipe came back after the sleep" in io, io)
            report.expect("mmu_io: the window of the copy travels with the context",
                          "a copy that sleeps keeps its window" in io
                          and "the program exited with 0" in io, io)

            # --- the machine is alive after every death -------------------
            alive = machine.run(ALIVE, args.timeout)
            execs += 1
            report.expect("the shell runs a program after the attacks",
                          "alive" in alive and "the program exited with 0" in alive, alive)

            # --- the audit of every program that ran ----------------------
            transcript = machine.transcript.decode("utf-8", errors="replace")
            report.expect("no program of the run was refused as not W^X",
                          AUDIT_BAD not in transcript, transcript)
            report.expect("every program of the run is audited by the tables",
                          transcript.count(AUDIT) == execs,
                          f"{transcript.count(AUDIT)} audit line(s) for {execs} exec(s)")
    except MachineError as error:
        print(f"mmu-smoke: {error}", file=sys.stderr)
        return 1

    failed = report.failed()

    for name, _, detail in failed:
        print(f"mmu-smoke: FAIL {name}")

        if detail:
            for line in str(detail).strip().splitlines()[-6:]:
                print(f"    {line}")

    print(f"mmu-smoke: {len(report.checks) - len(failed)} of {len(report.checks)} checks passed")

    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
