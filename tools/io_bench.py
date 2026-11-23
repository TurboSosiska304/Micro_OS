#!/usr/bin/env python3
"""Micro OS — the ruler of the grain of the I/O of the x86-64 machine.

The release moves a file through the calls in blocks; this tool measures what
that costs and what the size of the pipe behind a stage changes. It boots one
QEMU machine on an image of its own (the tree of the release plus `/bin/mkbig`
and `/bin/iobench`, which the image of a person does not carry), writes one
MiB of a pattern and one MiB of text on the volume, and runs the same list of
commands every time:

    cat /big.txt | wc -c                  a file through a pipe
    sed s/abc/xyz/ /big.txt > /tmp/sed    a filter that writes every line
    grep 0123 /big.txt > /tmp/grep        a filter that finds every line
    iobench pipe /big.bin 65536           the same copy, its calls counted
    iobench getc /big.txt                 a stream read one byte at a time
    iobench line /tmp/lines.txt 16132     a stream written one line at a time

Each command runs `--rounds` times (3 by default) and the shortest answer is
reported: the fastest of three is the number of the machine and not of a
neighbour of it. The output is one `MEASURE <name> <ms>` line per command and
the transcript of the lines the programs printed, so two runs of two trees can
be diffed by `--out`.

    tools/io_bench.py [--build DIR] [--mkfs PATH] [--rounds N]
                      [--out FILE] [--keep] [--verbose]
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time

from qemu_session import Machine
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

# The file of the measurement: 16132 lines of 65 bytes are 1,048,580 bytes,
# one MiB within 12 bytes, and the pattern of mkbig is a MiB of bytes.
LINES = 16132


def timed(machine: Machine, command: str, timeout: float = 300.0) -> tuple:
    started = time.monotonic()
    answer = machine.run(command, timeout=timeout, hang=timeout)
    return answer, time.monotonic() - started


def measurement(name: str, seconds: float) -> str:
    return f"MEASURE {name} {seconds * 1000.0:.1f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD)
    parser.add_argument("--mkfs", default=DEFAULT_MKFS)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--out", default=None)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    options = parser.parse_args()

    if shutil.which("qemu-system-x86_64") is None:
        print("io_bench: qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2

    work = tempfile.mkdtemp(prefix="microos-io-bench-")
    image = os.path.join(work, "bench.img")
    report = []

    def note(line: str) -> None:
        report.append(line)
        if options.verbose:
            print(line, flush=True)

    extra = [
        "--program",
        f"/bin/mkbig={os.path.join(options.build, 'mkbig.mox')}",
        "--program",
        f"/bin/iobench={os.path.join(options.build, 'iobench.mox')}",
    ]

    rc = build_image(image, options.build, options.mkfs, 16, extra=extra,
                     verbose=options.verbose)
    if rc != 0:
        print("io_bench: the image was not written", file=sys.stderr)
        return 2

    try:
        with Machine(options.build, image) as machine:
            machine.open()
            note("MEASURE boot %.1f" % (machine.boot_seconds * 1000.0))

            setup = [
                "exec /bin/mkbig write /big.bin 1",
                "mkdir /tmp",
                f"exec /bin/iobench line /big.txt {LINES}",
                "exec /bin/wc -c /big.bin",
                "exec /bin/wc -c /big.txt",
            ]
            for command in setup:
                answer, seconds = timed(machine, command, timeout=600.0)
                note(f"# {command}")
                note("# " + answer.strip().replace("\r\n", "\n").replace("\n", "\n# "))

            commands = [
                ("cat_pipe", "cat /big.txt | wc -c"),
                ("sed", "sed s/abc/xyz/ /big.txt > /tmp/sed.out"),
                ("grep", "grep 0123 /big.txt > /tmp/grep.out"),
                ("sed_size", "exec /bin/wc -c /tmp/sed.out"),
                ("grep_size", "exec /bin/wc -c /tmp/grep.out"),
                ("pipe_64k", "exec /bin/iobench pipe /big.bin 65536"),
                ("pipe_4k", "exec /bin/iobench pipe /big.bin 4096"),
                ("pipe_512", "exec /bin/iobench pipe /big.bin 512"),
                ("getc", "exec /bin/iobench getc /big.txt"),
                ("line", f"exec /bin/iobench line /tmp/lines.txt {LINES}"),
            ]

            for name, command in commands:
                best = None
                answer = ""
                for _ in range(max(1, options.rounds)):
                    answer, seconds = timed(machine, command, timeout=600.0)
                    best = seconds if best is None else min(best, seconds)
                note(f"# {command}")
                note("# " + answer.strip().replace("\r\n", "\n").replace("\n", "\n# "))
                note(measurement(name, best))
    finally:
        if options.keep:
            print(f"io_bench: the image is {image}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    if options.out:
        with open(options.out, "w", encoding="utf-8") as stream:
            stream.write("\n".join(report) + "\n")
    else:
        print("\n".join(report))

    return 0


if __name__ == "__main__":
    sys.exit(main())
