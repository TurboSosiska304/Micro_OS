#!/usr/bin/env python3
"""Micro OS — the numbers of the x86-64 port, one boot, fixed sizes.

A release is compared with the one before it by a table of numbers and not by
an impression, and the table has to be the same table every time. This tool
writes one image of the machine, boots it once, runs `/bin/bench` of the
build over five shapes of work and prints what came back:

    bench: syscall_getpid <value> calls/s     the door and a call of no work
    bench: fork_exec      <value> runs/s     fork, execve /bin/echo, waitpid
    bench: pipe           <value> MiB/s      4 MiB through a pipe, both ends
    bench: file_write     <value> MiB/s      16 MiB in 64 KiB blocks
    bench: file_read      <value> MiB/s      the same file, read back
    bench: console        <value> lines/s    200 lines on the serial console
    bench: shell_pipe     <value> MiB/s      `cat file | wc -c`, wall clock

The first six lines are the clock of the machine and the seventh is the wall
clock of the host: the shell pipeline prints no rate of its own, and the time
it takes is the time of the line as a person waits for it. Both kinds are the
same run of the same fixed sizes and are compared between releases on the same
machine; the value of a row is what changes, never the size.

The program is not in the image of the release: the tool puts it there for the
run (`--program /bin/bench=`), the way `tools/x86_volume_smoke.py` puts
`/bin/mkbig`. A measurement is a tool of the gate and not a command of the
machine.

    python3 tools/x86_bench.py [--build DIR] [--mkfs PATH] [--fs mfs|ext2]
                               [--dir DIR] [--json [PATH]] [--keep] [--verbose]

Exit status: 0 when every row is there and greater than zero, 2 when a tool or
a build is missing, 1 when a row failed.

Two shapes of the kernel are worth reading before a number means anything:

  * the pipe of the kernel holds 256 bytes and answers EAGAIN after a bounded
    wait (ADR 0014), so the 4 MiB of the pipe test is sixteen thousand
    handoffs of 256 bytes and the rate is the scheduler's, not the copy's.
    The loop of `/bin/bench` is the same loop after a release gives the pipe a
    real buffer, which is why the number moves and the test does not.
  * the volume is a file of the host and QEMU reads it through the page cache
    of the host: `file_read` is the rate of a warm cache of the host and not of
    a disk of a machine, and it is the same warm cache for every release.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from qemu_session import Machine, MachineError
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

# The sizes of the run. They are the contract of the table: a release that
# changes one of them changes the meaning of the row, so a new size is a new
# row and not an edit of this dictionary. The program takes them from the
# command line and does not choose; this is the only place they are written.
PARAMETERS = {
    "syscall_calls": 100_000,
    "fork_runs": 20,
    "pipe_bytes": 4 * 1024 * 1024,
    "file_mib": 16,
    "console_lines": 200,
    "pipeline_bytes": 4 * 1024 * 1024,
    "chunk_bytes": 65536,
}

# The disk is larger than the tree because the run writes a file of 16 MiB and
# a file of 4 MiB on the root before it reads them: 64 MiB is the volume of
# the release with the room of the two files.
IMAGE_MB = 64

# The wall clock of a line is the clock of the guest plus the console, the echo
# of the command and the prompt. A host with another build on it starves the
# emulator and adds much more than that, and a rate of a starved run is not a
# number of the release: the tool warns and a person throws the run away.
STARVATION_FACTOR = 2
STARVATION_MS = 2000

PIPE_FILE = "/bench.bin"
FILE_FILE = "/bench.big"

# The rows of the table in the order they are printed, with the unit the
# program prints and a sentence for the human summary. `guest` is False for
# the one row the host measures.
ROWS = [
    ("syscall_getpid", "calls/s", True, "getpid() in a loop"),
    ("fork_exec", "runs/s", True, "fork + execve /bin/echo + waitpid"),
    ("pipe", "MiB/s", True, "4 MiB through one pipe, both ends"),
    ("file_write", "MiB/s", True, "16 MiB written in 64 KiB blocks"),
    ("file_read", "MiB/s", True, "16 MiB read in 64 KiB blocks"),
    ("console", "lines/s", True, "200 lines written to the console"),
    ("shell_pipe", "MiB/s", False, "cat /bench.bin | wc -c (wall clock)"),
]

# The name of a step of the run and the row it feeds: the command is a mode of
# `/bin/bench` and the row is what a release is compared by, and two of them
# are not the same word.
STEP_ROWS = {"syscall": "syscall_getpid", "fork": "fork_exec",
             "filewrite": "file_write", "fileread": "file_read"}

TABLE_LINE = re.compile(r"^bench: (?P<name>[a-z_]+) (?P<value>[0-9]+\.[0-9]+) (?P<unit>\S+)$")
NOTE_LINE = re.compile(
    r"^bench note: (?P<name>[a-z_]+) (?P<count>[0-9]+) \w+ in (?P<ms>[0-9]+) ms$"
)


@contextlib.contextmanager
def stdout_to_stderr():
    """Send the writes of a child to the error stream for the length of a block.

    `tools/x86_image.py` runs the host tool of MicroFS, and the tool prints
    one line of its own to the standard output. That line is a note for a
    person and not a row of the table: with `--json -` the standard output is
    the document a caller parses, and a line of mkfs in it is a broken JSON.
    The descriptor itself is moved, not `sys.stdout`, because a child process
    writes to the descriptor the tool inherited.
    """
    sys.stdout.flush()
    saved = os.dup(1)

    try:
        os.dup2(2, 1)
        yield
    finally:
        sys.stdout.flush()
        os.dup2(saved, 1)
        os.close(saved)


def commit() -> str:
    """The short hash of the tree, when it is a git checkout."""
    try:
        result = subprocess.run(
            ["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
            capture_output=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"

    return result.stdout.decode("utf-8", "replace").strip() or "unknown"


def parse_block(block: str) -> tuple:
    """The rows and the notes of one answer of the shell."""
    rows = {}
    notes = {}

    for line in block.replace("\r", "").splitlines():
        match = TABLE_LINE.match(line.strip())
        if match is not None:
            rows[match.group("name")] = (float(match.group("value")), match.group("unit"))
            continue

        match = NOTE_LINE.match(line.strip())
        if match is not None:
            notes[match.group("name")] = int(match.group("ms"))

    return rows, notes


def run_step(machine: Machine, command: str, timeout: float) -> tuple:
    """Type one line, return its answer and the milliseconds of the host.

    The milliseconds are the wall clock of the whole line: the echo of the
    command, the work, the console and the prompt. They are reported beside
    the rate of the guest because the two answer different questions, and the
    pipeline of the shell has no other clock to be measured by.
    """
    started = time.monotonic()
    answer = machine.run(command, timeout=timeout, hang=timeout)
    wall_ms = int((time.monotonic() - started) * 1000)

    return answer, wall_ms


def starved(results: list) -> list:
    """The rows whose line took far longer on the host than in the machine."""
    return [
        result["name"] for result in results
        if result["guest_ms"] is not None
        and result["wall_ms"] is not None
        and result["wall_ms"] > result["guest_ms"] * STARVATION_FACTOR + STARVATION_MS
    ]


def human_summary(results: list, boot_seconds: float, seconds: float, fs: str, out) -> None:
    print("Micro OS — the numbers of the x86-64 port", file=out)
    print("=" * 78, file=out)
    print(f"  boot {boot_seconds:.1f} s, the run {seconds:.1f} s,"
          f" the image {IMAGE_MB} MB of {fs}", file=out)
    print("", file=out)
    print(f"  {'name':<16} {'value':>14} {'unit':<9} {'guest':>10} {'host':>9}  what", file=out)
    print(f"  {'-' * 16} {'-' * 14} {'-' * 9} {'-' * 10} {'-' * 9}  {'-' * 34}", file=out)

    for result in results:
        guest = f"{result['guest_ms']} ms" if result["guest_ms"] is not None else "-"
        host = f"{result['wall_ms']} ms" if result["wall_ms"] is not None else "-"
        print(f"  {result['name']:<16} {result['value']:>14.2f} {result['unit']:<9}"
              f" {guest:>10} {host:>8}  {result['what']}", file=out)

    print("=" * 78, file=out)

    slow = starved(results)

    if slow:
        print(f"  the host was busy: {', '.join(slow)} took far longer on the host"
              " than in the machine, and the run is not a number of the release", file=out)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the numbers of the x86-64 port")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--fs", choices=("mfs", "ext2"), default="mfs",
                        help="the filesystem of the root (default: mfs)")
    parser.add_argument("--dir", default=None, help="where the image of the run is written")
    parser.add_argument("--json", metavar="PATH", nargs="?", const="-", default=None,
                        help="also write the table as JSON (default: standard output)")
    parser.add_argument("--timeout", type=float, default=900.0,
                        help="the seconds one line may take (default: 900)")
    parser.add_argument("--keep", action="store_true", help="keep the image of the run")
    parser.add_argument("--verbose", action="store_true",
                        help="print the console of every line")
    args = parser.parse_args(argv)

    for path in ("loader32.elf", "microos64.bin", "bench.mox"):
        if not os.path.isfile(os.path.join(args.build, path)):
            print(f"missing {args.build}/{path}: run make x86-build", file=sys.stderr)
            return 2

    if not os.path.isfile(args.mkfs):
        print(f"missing {args.mkfs}: run make build", file=sys.stderr)
        return 2

    if not shutil.which("qemu-system-x86_64"):
        print("missing qemu-system-x86_64", file=sys.stderr)
        return 2

    if args.dir is not None:
        work = args.dir
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="microos-bench-", dir=args.build)

    image = os.path.join(work, f"bench-{args.fs}.img")
    program = f"/bin/bench={os.path.join(args.build, 'bench.mox')}"

    if os.path.exists(image):
        os.unlink(image)

    started = time.monotonic()

    with (stdout_to_stderr() if args.json == "-" else contextlib.nullcontext()):
        written = build_image(image, args.build, args.mkfs, IMAGE_MB, fs=args.fs, data_mb=0,
                              extra=["--program", program])

    if written != 0:
        print("the image of the run could not be written", file=sys.stderr)
        return 1

    rows = {}
    notes = {}
    wall_ms = {}
    boot_seconds = 0.0
    failure = None

    steps = [
        ("fill", "exec /bin/bench fill"
                 f" {PIPE_FILE} {PARAMETERS['pipeline_bytes'] // (1024 * 1024)}"),
        ("syscall", f"exec /bin/bench syscall {PARAMETERS['syscall_calls']}"),
        ("fork", f"exec /bin/bench fork {PARAMETERS['fork_runs']}"),
        ("pipe", f"exec /bin/bench pipe {PARAMETERS['pipe_bytes']}"),
        ("filewrite", f"exec /bin/bench filewrite {FILE_FILE} {PARAMETERS['file_mib']}"),
        ("fileread", f"exec /bin/bench fileread {FILE_FILE} {PARAMETERS['file_mib']}"),
        ("console", f"exec /bin/bench console {PARAMETERS['console_lines']}"),
        ("shell_pipe", f"cat {PIPE_FILE} | wc -c"),
    ]

    try:
        with Machine(args.build, image) as machine:
            machine.open(timeout=args.timeout)
            boot_seconds = machine.boot_seconds

            for name, command in steps:
                answer, wall = run_step(machine, command, args.timeout)

                if args.verbose:
                    print(f"--- {command} ({wall / 1000.0:.2f} s)")
                    print(answer)

                block_rows, block_notes = parse_block(answer)
                rows.update(block_rows)
                notes.update(block_notes)
                wall_ms[STEP_ROWS.get(name, name)] = wall

                if name == "shell_pipe":
                    if str(PARAMETERS["pipeline_bytes"]) not in answer:
                        failure = (f"the pipeline carried no {PARAMETERS['pipeline_bytes']}"
                                   f" bytes: {answer.strip()[-200:]}")
                        break
                    # The count of the bytes is the number of the row and the
                    # host clock is the only time the line has: the rate is
                    # worked out here and not by the guest.
                    if wall <= 0:
                        failure = "the pipeline took no measurable time"
                        break
                    mib = PARAMETERS["pipeline_bytes"] / (1024 * 1024)
                    rows["shell_pipe"] = (mib / (wall / 1000.0), "MiB/s")
    except MachineError as error:
        print(f"the machine stopped: {error}", file=sys.stderr)
        failure = str(error)
    finally:
        if not args.keep and args.dir is None:
            shutil.rmtree(work, ignore_errors=True)

    seconds_all = time.monotonic() - started

    results = []
    for name, unit, guest, what in ROWS:
        row = rows.get(name)

        if row is None:
            if failure is None:
                failure = f"no row for {name}: the program did not report it"
            continue

        value, reported_unit = row

        if reported_unit != unit:
            failure = f"{name} was reported in {reported_unit}, not {unit}"
            continue

        if value <= 0:
            failure = f"{name} is {value}, not a rate"
            continue

        results.append({
            "name": name,
            "value": round(value, 2),
            "unit": unit,
            "guest_ms": notes.get(name) if guest else None,
            "wall_ms": wall_ms.get(name),
            "what": what,
        })

    if failure is None and len(results) != len(ROWS):
        failure = f"{len(results)} of {len(ROWS)} rows"

    if failure is not None:
        print(f"FAILED: {failure}", file=sys.stderr)
        for name, unit, guest, what in ROWS:
            row = rows.get(name)
            print(f"  {'ok ' if row else '-- '} {name} {row if row else ''}", file=sys.stderr)
        print("RESULT: FAIL")
        return 1

    # The table to the console of the human, and the JSON to a file or the
    # standard output: a caller that reads the JSON does not have to parse the
    # table, and a caller that reads the table does not have the JSON in it.
    # When the JSON takes the standard output the human half moves to the
    # error stream, so that neither reader sees the other one's lines.
    human = sys.stderr if args.json == "-" else sys.stdout

    for result in results:
        print(f"bench: {result['name']} {result['value']:.2f} {result['unit']}", file=human)

    human_summary(results, boot_seconds, seconds_all, args.fs, human)

    print("RESULT: PASS", file=human)

    if args.json is not None:
        document = {
            "microos_bench": 1,
            "commit": commit(),
            "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "fs": args.fs,
            "image_mb": IMAGE_MB,
            "boot_seconds": round(boot_seconds, 3),
            "run_seconds": round(seconds_all, 3),
            "parameters": PARAMETERS,
            "starved": starved(results),
            "results": results,
        }
        text = json.dumps(document, indent=2, ensure_ascii=False) + "\n"

        if args.json == "-":
            sys.stdout.write(text)
        else:
            with open(args.json, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
