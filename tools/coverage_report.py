#!/usr/bin/env python3
"""Micro OS — the coverage of the portable kernel, counted by gcov.

The gate of 1.0 asks for a line coverage of the kernel and a threshold the
build has to meet. The report is made from the files of a host build with
MICRO_OS_COVERAGE=ON: every .gcda of the build is handed to gcov, and the
JSON of each source is merged by (path, line), so a source compiled into
two binaries is one line and not two.

    tools/coverage_report.py [--build DIR] [--path PREFIX] [--threshold N]
                             [--verbose]

Exit status: 0 when the coverage is at or above the threshold, 1 when it is
below, 2 when there is nothing to measure (a build without --coverage).
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build-coverage"
DEFAULT_PATH = "components/kernel"
DEFAULT_THRESHOLD = 70.0


def find_gcda(build: Path) -> list:
    """Every counter file of the build, in a stable order."""
    return sorted(build.rglob("*.gcda"))


def read_json(path: Path) -> dict:
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        return json.load(stream)


def run_gcov(gcda: Path, work: Path) -> list:
    """The JSON reports gcov writes for one counter file."""
    gcov = shutil.which("gcov")
    if gcov is None:
        raise RuntimeError("gcov is not on PATH")

    directory = Path(tempfile.mkdtemp(prefix="gcov-", dir=work))
    result = subprocess.run(
        [gcov, "--json-format", str(gcda.resolve())],
        cwd=directory,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gcov failed for {gcda}: {result.stdout.strip()}")

    return sorted(directory.glob("*.gcov.json.gz"))


def source_of(report: dict, name: str) -> str:
    """The absolute path of a source of a report.

    gcov writes the path of the compiler's working directory and the name
    of the source the way the compiler got it. CMake passes the source as a
    path relative to the tree in one build and as an absolute one in
    another, so the candidates are tried in order and the first file that
    exists wins.
    """
    base = report.get("current_working_directory", "")

    candidates = []
    if os.path.isabs(name):
        candidates.append(name)
    else:
        candidates.append(os.path.join(base, name))
        candidates.append(os.path.join(str(ROOT), name))
        candidates.append(name)

    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.normpath(os.path.abspath(candidate))

    return os.path.normpath(os.path.abspath(candidates[0]))


def collect(build: Path, prefix: str) -> dict:
    """The executable lines of `prefix`, and whether any run reached each.

    A line is one line of one source, whatever the number of binaries that
    compiled it: the union of the runs is what the tests reached.
    """
    wanted = os.path.normpath(str(ROOT / prefix))
    lines = {}

    with tempfile.TemporaryDirectory(prefix="microos-coverage-") as work:
        for gcda in find_gcda(build):
            for path in run_gcov(gcda, Path(work)):
                report = read_json(path)
                for source in report.get("files", []):
                    name = source_of(report, source.get("file", ""))
                    if name != wanted and not name.startswith(wanted + os.sep):
                        continue
                    table = lines.setdefault(name, {})
                    for line in source.get("lines", []):
                        number = line.get("line_number")
                        if number is None:
                            continue
                        hit = line.get("count", 0) > 0
                        table[number] = table.get(number, False) or hit

    return lines


def percentage(hit: int, total: int) -> float:
    return 100.0 * hit / total if total > 0 else 0.0


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the coverage of the portable kernel")
    parser.add_argument("--build", default=str(DEFAULT_BUILD), help="the coverage build directory")
    parser.add_argument("--path", default=DEFAULT_PATH, help="the subtree of the sources to count")
    parser.add_argument(
        "--threshold",
        type=float,
        default=DEFAULT_THRESHOLD,
        help=f"the least percentage of lines (default: {DEFAULT_THRESHOLD:g})",
    )
    parser.add_argument("--verbose", action="store_true", help="the numbers of every file")
    args = parser.parse_args(argv)

    build = Path(args.build)
    if not build.is_dir():
        print(f"coverage: {build} is not a build directory", file=sys.stderr)
        print("run it with -DMICRO_OS_COVERAGE=ON: make coverage", file=sys.stderr)
        return 2

    try:
        lines = collect(build, args.path)
    except RuntimeError as error:
        print(f"coverage: {error}", file=sys.stderr)
        return 2

    if not lines:
        print(f"coverage: no counters for {args.path} in {build}", file=sys.stderr)
        print("was the build made with -DMICRO_OS_COVERAGE=ON?", file=sys.stderr)
        return 2

    rows = []
    total_hit = 0
    total_lines = 0

    for source in sorted(lines):
        table = lines[source]
        hit = sum(1 for value in table.values() if value)
        total = len(table)
        total_hit += hit
        total_lines += total
        rows.append((source, hit, total))

    if args.verbose:
        for source, hit, total in rows:
            print(f"  {percentage(hit, total):6.1f}%  {hit:5d}/{total:<5d} {source}")

    percent = percentage(total_hit, total_lines)
    verdict = "PASS" if percent >= args.threshold else "FAIL"

    print(
        f"coverage: {total_hit}/{total_lines} line(s) "
        f"({percent:.1f}%) in {args.path}, threshold {args.threshold:g}%: {verdict}"
    )

    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
