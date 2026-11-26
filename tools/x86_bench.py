#!/usr/bin/env python3
"""Micro OS — the numbers of the x86-64 port, one boot, fixed sizes.

A release is compared with the one before it by a table of numbers and not by
an impression, and the table has to be the same table every time. This tool
writes one image of the machine for every run, boots it, runs `/bin/bench` of
the build over the shapes of work below and prints what came back:

    bench: syscall_getpid <value> calls/s     the door and a call of no work
    bench: fork_exec      <value> runs/s     fork, execve /bin/echo, waitpid
    bench: pipe           <value> MiB/s      4 MiB through a pipe, both ends
    bench: file_write     <value> MiB/s      16 MiB in 64 KiB blocks
    bench: file_read      <value> MiB/s      the same file, read back
    bench: console        <value> lines/s    200 lines on the serial console
    bench: lat_syscall_p50 <value> ns         the middle call of getpid()
    bench: lat_syscall_p99 <value> ns         the tail of the same calls
    bench: lat_pipe_p50 <value> ns            the middle byte through a pipe
    bench: lat_pipe_p99 <value> ns            the tail of the same bytes
    bench: shell_pipe     <value> MiB/s      `cat file | wc -c`, wall clock

The machine rows are the clock of the machine and the last one is the wall
clock of the host: the shell pipeline prints no rate of its own, and the time
it takes is the time of the line as a person waits for it. Both kinds are the
same run of the same fixed sizes and are compared between releases on the same
machine; the value of a row is what changes, never the size.

A run is one sample and one sample is not a number of a release: the tool
boots the machine three times over a fresh image each time and every row of
the table is the median of those runs, printed as

    bench: <name> <median> <unit> (median of 3)

The spread of the runs is beside the median in the human summary (and in the
JSON): a row that moved by more than a tenth between the runs is a warning of
the host and not a number of the tree.

The program is not in the image of the release: the tool puts it there for the
run (`--program /bin/bench=`), the way `tools/x86_volume_smoke.py` puts
`/bin/mkbig`. A measurement is a tool of the gate and not a command of the
machine.

    python3 tools/x86_bench.py [--build DIR] [--mkfs PATH] [--fs mfs|ext2]
                               [--runs N] [--dir DIR] [--json [PATH]] [--keep]
                               [--verbose]

Exit status: 0 when every row is there and greater than zero in every run that
is in the median, 2 when a tool or a build is missing, 1 when a row failed.

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

from qemu_session import DISKS, Machine, MachineError
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
    "screen_cells": 160 * 50,
    "pipeline_bytes": 4 * 1024 * 1024,
    "chunk_bytes": 65536,
    "lat_samples": 2000,
    "lat_pipe_samples": 256,
}

# The disk is larger than the tree because the run writes a file of 16 MiB and
# a file of 4 MiB on the root before it reads them: 64 MiB is the volume of
# the release with the room of the two files.
IMAGE_MB = 64

# The wall clock of a line is the clock of the guest plus the console, the echo
# of the command and the prompt. A host with another build on it starves the
# emulator and adds much more than that, and a rate of a starved run is not a
# number of the release: the tool warns, drops that run and a person runs it
# again. A run that is starved in every row is the only one the tool itself
# refuses.
STARVATION_FACTOR = 2
STARVATION_MS = 2000

# The number of boots of one table and the disagreement of the runs that is a
# warning. A tenth is the noise of the host this table already knows
# (docs/bench.md): a row that moves more between two runs of one tree is the
# load of the machine of the day and not the tree.
DEFAULT_RUNS = 3
SPREAD_PERCENT = 10.0

PIPE_FILE = "/bench.bin"
FILE_FILE = "/bench.big"

# The rows of the table in the order they are printed: the name the program
# prints, the unit, whether the clock is the guest's, the name of the note
# line that carries the milliseconds and the sentence of the human summary.
# The two rows of a latency share one note and one command: the p50 and the
# p99 are two readings of the same histogram. `guest` is False for the one row
# the host measures.
ROWS = [
    ("syscall_getpid", "calls/s", True, "syscall_getpid", "getpid() in a loop"),
    ("fork_exec", "runs/s", True, "fork_exec", "fork + execve /bin/echo + waitpid"),
    ("pipe", "MiB/s", True, "pipe", "4 MiB through one pipe, both ends"),
    ("file_write", "MiB/s", True, "file_write", "16 MiB written in 64 KiB blocks"),
    ("file_read", "MiB/s", True, "file_read", "16 MiB read in 64 KiB blocks"),
    ("console", "lines/s", True, "console", "200 lines written to the console"),
    ("screen_text", "ns", True, "screen_text", "one 160x50 screen through the framebuffer"),
    ("lat_syscall_p50", "ns", True, "lat_syscall", "p50 of 2 000 calls of getpid()"),
    ("lat_syscall_p99", "ns", True, "lat_syscall", "p99 of the same calls"),
    ("lat_pipe_p50", "ns", True, "lat_pipe", "p50 of a byte through a pipe"),
    ("lat_pipe_p99", "ns", True, "lat_pipe", "p99 of the same bytes"),
    ("shell_pipe", "MiB/s", False, "shell_pipe", "cat /bench.bin | wc -c (wall clock)"),
]

# The name of a step of the run and the line of the table it feeds: the
# command is a mode of `/bin/bench` and the row is what a release is compared
# by, and two of them are not the same word.
STEP_ROWS = {"syscall": "syscall_getpid", "fork": "fork_exec",
             "filewrite": "file_write", "fileread": "file_read",
             "screen": "screen_text"}

TABLE_LINE = re.compile(r"^bench: (?P<name>[a-z0-9_]+) (?P<value>[0-9]+\.[0-9]+) (?P<unit>\S+)$")
NOTE_LINE = re.compile(
    r"^bench note: (?P<name>[a-z0-9_]+) (?P<count>[0-9]+) \w+ in (?P<ms>[0-9]+) ms$"
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


def median(values: list):
    """The middle of the samples of one row: the number of a run of runs.

    An even count has no middle sample, and the mean of the two middle ones is
    the answer the arithmetic asks for; the rows of this table are runs of
    three and of one, so it is a formality of the function.
    """
    if not values:
        return None

    ordered = sorted(values)

    if len(ordered) % 2:
        return ordered[len(ordered) // 2]

    half = len(ordered) // 2

    return (ordered[half - 1] + ordered[half]) / 2.0


def spread_percent(values: list) -> float:
    """How far apart the runs of one row are, against the middle of them."""
    middle = median(values)

    if middle is None or middle == 0:
        return 0.0

    return (max(values) - min(values)) / middle * 100.0


def table(rows: dict, notes: dict, wall_ms: dict, failure) -> tuple:
    """The rows of one run, out of the lines the console answered with."""
    results = []

    for name, unit, guest, note, what in ROWS:
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
            "guest_ms": notes.get(note) if guest else None,
            "wall_ms": wall_ms.get(note),
            "what": what,
        })

    if failure is None and len(results) != len(ROWS):
        failure = f"{len(results)} of {len(ROWS)} rows"

    return results, failure


def one_run(args, index: int) -> dict:
    """Write a fresh image, boot the machine once and read the table of it.

    Every run gets an image of its own: the program writes a file of 16 MiB
    into the volume, and the place of that write in the volume is part of the
    number of `file_write` (docs/bench.md). A second run on the image of the
    first would measure another filesystem and not the same row.
    """
    if args.dir is not None:
        work = args.dir
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="microos-bench-", dir=args.build)

    image = os.path.join(work, f"bench-{args.fs}-{index + 1}.img")
    program = f"/bin/bench={os.path.join(args.build, 'bench.mox')}"

    if os.path.exists(image):
        os.unlink(image)

    started = time.monotonic()

    try:
        with (stdout_to_stderr() if args.json == "-" else contextlib.nullcontext()):
            written = build_image(image, args.build, args.mkfs, IMAGE_MB, fs=args.fs, data_mb=0,
                                  extra=["--program", program], block_size=args.block_size)

        if written != 0:
            return {"results": [], "failure": "the image of the run could not be written",
                    "boot_seconds": 0.0, "seconds": time.monotonic() - started}

        rows = {}
        notes = {}
        wall_ms = {}
        boot_seconds = 0.0
        failure = None

        steps = [
            ("screen", f"exec /bin/bench screen {PARAMETERS['screen_cells']}"),
            ("fill", "exec /bin/bench fill"
                     f" {PIPE_FILE} {PARAMETERS['pipeline_bytes'] // (1024 * 1024)}"),
            ("syscall", f"exec /bin/bench syscall {PARAMETERS['syscall_calls']}"),
            ("fork", f"exec /bin/bench fork {PARAMETERS['fork_runs']}"),
            ("pipe", f"exec /bin/bench pipe {PARAMETERS['pipe_bytes']}"),
            ("filewrite", f"exec /bin/bench filewrite {FILE_FILE} {PARAMETERS['file_mib']}"),
            ("fileread", f"exec /bin/bench fileread {FILE_FILE} {PARAMETERS['file_mib']}"),
            ("console", f"exec /bin/bench console {PARAMETERS['console_lines']}"),
            ("lat_syscall", f"exec /bin/bench latsyscall {PARAMETERS['lat_samples']}"),
            ("lat_pipe", f"exec /bin/bench latpipe {PARAMETERS['lat_pipe_samples']}"),
            ("shell_pipe", f"cat {PIPE_FILE} | wc -c"),
        ]

        try:
            with Machine(args.build, image, disk=args.disk,
                         extra_args=["-vga", "std"]) as machine:
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

        results, failure = table(rows, notes, wall_ms, failure)

        if failure is not None:
            for name, unit, guest, note, what in ROWS:
                row = rows.get(name)
                print(f"  {'ok ' if row else '-- '} {name} {row if row else ''}", file=sys.stderr)

        return {"results": results, "failure": failure, "boot_seconds": boot_seconds,
                "seconds": time.monotonic() - started}
    finally:
        if not args.keep and args.dir is None:
            shutil.rmtree(work, ignore_errors=True)


def human_summary(results: list, boot_seconds: float, seconds: float, fs: str, runs: int,
                  out, block_size: int = 1024) -> None:
    print("Micro OS — the numbers of the x86-64 port", file=out)
    print("=" * 86, file=out)
    grain = f", blocks of {block_size} bytes" if fs == "ext2" else ""
    print(f"  boot {boot_seconds:.1f} s, the run {seconds:.1f} s over {runs}"
          f" {'run' if runs == 1 else 'runs'}, the image {IMAGE_MB} MB of {fs}{grain}", file=out)
    print("", file=out)
    print(f"  {'name':<16} {'median':>14} {'unit':<9} {'spread':>7} {'guest':>10} {'host':>9}"
          "  what", file=out)
    print(f"  {'-' * 16} {'-' * 14} {'-' * 9} {'-' * 7} {'-' * 10} {'-' * 9}"
          f"  {'-' * 34}", file=out)

    for result in results:
        guest = f"{result['guest_ms']} ms" if result["guest_ms"] is not None else "-"
        host = f"{result['wall_ms']} ms" if result["wall_ms"] is not None else "-"
        print(f"  {result['name']:<16} {result['value']:>14.2f} {result['unit']:<9}"
              f" {result['spread']:>6.1f}% {guest:>10} {host:>9}  {result['what']}", file=out)

    print("=" * 86, file=out)


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the numbers of the x86-64 port")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--disk", choices=DISKS, default="ide",
                        help="the bus of the disk of the run (default: ide)")
    parser.add_argument("--fs", choices=("mfs", "ext2"), default="mfs",
                        help="the filesystem of the root (default: mfs)")
    parser.add_argument("--block-size", type=int, choices=(1024, 2048, 4096), default=1024,
                        metavar="N",
                        help="the block of the ext2 volume in bytes (default: 1024)")
    parser.add_argument("--runs", type=int, default=DEFAULT_RUNS, metavar="N",
                        help=f"the boots the median is taken over (default: {DEFAULT_RUNS})")
    parser.add_argument("--dir", default=None, help="where the image of the run is written")
    parser.add_argument("--json", metavar="PATH", nargs="?", const="-", default=None,
                        help="also write the table as JSON (default: standard output)")
    parser.add_argument("--timeout", type=float, default=900.0,
                        help="the seconds one line may take (default: 900)")
    parser.add_argument("--keep", action="store_true", help="keep the image of the run")
    parser.add_argument("--verbose", action="store_true",
                        help="print the console of every line")
    args = parser.parse_args(argv)

    if args.runs < 1:
        parser.error("--runs takes a number of boots of at least one")

    if args.block_size != 1024 and args.fs != "ext2":
        parser.error("--block-size is the grain of the ext2 volume: MicroFS counts 1024")

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

    # The boots of the table, one after another and not at once: two emulators
    # beside each other starve each other, and a starved run is not a number.
    started = time.monotonic()
    runs = []

    for index in range(args.runs):
        if args.runs > 1:
            print(f"run {index + 1} of {args.runs}", file=sys.stderr)

        outcome = one_run(args, index)

        if outcome["failure"] is not None:
            print(f"FAILED: run {index + 1} of {args.runs}: {outcome['failure']}",
                  file=sys.stderr)
            print("RESULT: FAIL")
            return 1

        runs.append(outcome)

    seconds_all = time.monotonic() - started

    # A run the host starved is not a number of the release and leaves the
    # median, as the table of one run always said it did. When none is left
    # there is no table to report and the tool fails instead of printing the
    # spread of a busy machine as the number of the tree.
    starved_runs = [index for index, run in enumerate(runs) if starved(run["results"])]
    kept = [run for index, run in enumerate(runs) if index not in starved_runs]

    if not kept:
        print("FAILED: every run was starved by the host: nothing to take a median of",
              file=sys.stderr)
        print("RESULT: FAIL")
        return 1

    results = []

    for name, unit, guest, note, what in ROWS:
        samples = [row for run in kept for row in run["results"] if row["name"] == name]

        values = [row["value"] for row in samples]
        guests = [row["guest_ms"] for row in samples if row["guest_ms"] is not None]
        hosts = [row["wall_ms"] for row in samples if row["wall_ms"] is not None]

        results.append({
            "name": name,
            "value": round(median(values), 2),
            "median": round(median(values), 2),
            "unit": unit,
            "values": [round(value, 2) for value in values],
            "spread": round(spread_percent(values), 2),
            "guest_ms": int(median(guests)) if guests else None,
            "wall_ms": int(median(hosts)) if hosts else None,
            "what": what,
        })

    boot_seconds = median([run["boot_seconds"] for run in kept])

    # The table to the console of the human, and the JSON to a file or the
    # standard output: a caller that reads the JSON does not have to parse the
    # table, and a caller that reads the table does not have the JSON in it.
    # When the JSON takes the standard output the human half moves to the
    # error stream, so that neither reader sees the other one's lines.
    human = sys.stderr if args.json == "-" else sys.stdout

    human_summary(results, boot_seconds, seconds_all, args.fs, len(kept), human,
                  args.block_size)

    if starved_runs:
        print(f"  the host was busy in run{'' if len(starved_runs) == 1 else 's'} "
              f"{', '.join(str(index + 1) for index in starved_runs)}:"
              " that run is not in the median", file=human)

    for result in results:
        if result["spread"] > SPREAD_PERCENT:
            print(f"  the runs disagree: {result['name']} spread {result['spread']:.1f} %"
                  f" over {SPREAD_PERCENT:.0f} %: the row is the load of the host of the day",
                  file=human)

    lines = "run" if len(kept) == 1 else f"median of {len(kept)}"

    for result in results:
        print(f"bench: {result['name']} {result['value']:.2f} {result['unit']} ({lines})",
              file=human)

    print("RESULT: PASS", file=human)

    if args.json is not None:
        document = {
            "microos_bench": 2,
            "commit": commit(),
            "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "fs": args.fs,
            "block_size": args.block_size,
            "image_mb": IMAGE_MB,
            "runs": len(kept),
            "runs_asked": args.runs,
            "boot_seconds": round(boot_seconds, 3),
            "run_seconds": round(seconds_all, 3),
            "parameters": PARAMETERS,
            "starved": sorted({name for run in runs for name in starved(run["results"])}),
            "starved_runs": [index + 1 for index in starved_runs],
            "spread": {result["name"]: result["spread"] for result in results},
            "median": {result["name"]: result["value"] for result in results},
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
