#!/usr/bin/env python3
"""Micro OS — the budget of the internal SRAM of the board.

The static part of the internal RAM of the ESP32 is the `.dram0.data` and
`.dram0.bss` sections: the state of the kernel and of the drivers, the stacks
the scheduler hands out, the buffers of the libraries. It is checked here
against a budget, next to the budget of the image, because the flash is not
the resource that runs out first on this board.

The numbers come from the build of the firmware and not from a table that goes
stale: `build*/micro_os.map` of the linker, or the JSON of `idf.py size
--format json` saved as `build*/idf_size.json`. A checkout without a firmware
build is not a failure: the check skips itself with the reason, and the
release workflow, which builds before it checks, runs it for real.

    tools/sram_budget.py                    # the build directories of the tree
    tools/sram_budget.py --map FILE         # one map
    tools/sram_budget.py --size-json FILE   # one JSON of `idf.py size`
    tools/sram_budget.py --json report.json # the report as JSON
    tools/sram_budget.py -v                 # the successful measurements too

The exit status is 0 when the static DRAM fits and 1 when it does not.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys
from dataclasses import dataclass

# The internal SRAM of the ESP32 that the linker script gives to the data is
# 0x2c200 bytes (176 KB). The heap and the applications take what the static
# state leaves, and 48 KB of that is the reserve of the project (AGENTS.md);
# 128 KB is the ceiling of the static state, so that the reserve stays real.
SRAM_BUDGET_BYTES = 128 * 1024

# The files of a build that carry the numbers, in the order they are tried.
MAP_NAMES = ("micro_os.map",)
SIZE_JSON_NAMES = ("idf_size.json", "size.json")

# The output sections of the linker script of the ESP32 that hold the static
# internal RAM. The first line of each is the one of the memory map.
DRAM_SECTION_RE = re.compile(
    r"^\.dram0\.(data|bss)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s*$", re.MULTILINE
)
MEMORY_RE = re.compile(r"^dram0_0_seg\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", re.MULTILINE)


@dataclass
class Measurement:
    """The static DRAM of one build, in bytes."""

    source: str
    data: int
    bss: int
    region: int | None = None
    region_name: str = ""

    @property
    def used(self) -> int:
        return self.data + self.bss

    @property
    def headroom(self) -> int:
        return SRAM_BUDGET_BYTES - self.used


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def measure_map(path: str) -> Measurement:
    """The sizes of `.dram0.data` and `.dram0.bss` of a linker map."""
    text = read_text(path)
    sizes = {"data": None, "bss": None}

    for match in DRAM_SECTION_RE.finditer(text):
        name = match.group(1)
        if sizes[name] is None:
            sizes[name] = int(match.group(2), 16)

    if sizes["data"] is None and sizes["bss"] is None:
        raise ValueError(f"{path}: no .dram0.data or .dram0.bss section (not an ESP-IDF map?)")

    region_match = MEMORY_RE.search(text)
    region = int(region_match.group(1), 16) if region_match else None

    return Measurement(
        source=path,
        data=sizes["data"] or 0,
        bss=sizes["bss"] or 0,
        region=region,
        region_name="dram0_0_seg" if region is not None else "",
    )


def measure_size_json(path: str) -> Measurement:
    """The sizes of a report of `idf.py size --format json`."""
    report = json.loads(read_text(path))

    for key in ("dram_data", "dram_bss"):
        if key not in report:
            raise ValueError(f"{path}: no '{key}' (not a size report of ESP-IDF?)")

    region = report.get("dram_total")
    region = int(region) if region is not None else None

    return Measurement(
        source=path,
        data=int(report["dram_data"]),
        bss=int(report["dram_bss"]),
        region=region,
        region_name="dram0_0_seg" if region is not None else "",
    )


def build_directories(root: str) -> list:
    """The build directories of the tree, the host ones included."""
    directories = []

    try:
        names = sorted(os.listdir(root))
    except OSError:
        return directories

    for name in names:
        if not name.startswith("build"):
            continue

        path = os.path.join(root, name)
        if os.path.isdir(path):
            directories.append(path)

    return directories


def collect_measurements(root: str, maps: list, size_jsons: list) -> list:
    """Every build of the tree that has the numbers of its static DRAM.

    A build with a JSON report is measured from it, because that format is the
    one ESP-IDF promises; a build with only a map is measured from the map.
    """
    if maps or size_jsons:
        measurements = []

        for path in maps:
            measurements.append(measure_map(path))

        for path in size_jsons:
            measurements.append(measure_size_json(path))

        return measurements

    measurements = []

    for build in build_directories(root):
        json_path = None
        for name in SIZE_JSON_NAMES:
            candidate = os.path.join(build, name)
            if os.path.isfile(candidate):
                json_path = candidate
                break

        if json_path is not None:
            measurements.append(measure_size_json(json_path))
            continue

        candidates = [os.path.join(build, name) for name in MAP_NAMES]
        candidates = [path for path in candidates if os.path.isfile(path)]

        if not candidates:
            candidates = sorted(glob.glob(os.path.join(build, "*.map")))

        if candidates:
            measurements.append(measure_map(candidates[0]))

    return measurements


def describe(measurement: Measurement) -> str:
    """One line about a measurement, for the report of the integrity check."""
    text = (
        f"{measurement.source}: {measurement.used} bytes "
        f"(data {measurement.data} + bss {measurement.bss}) of {SRAM_BUDGET_BYTES}"
    )

    if measurement.region is not None:
        text += f", the region {measurement.region_name} is {measurement.region}"

    return text


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the budget of the static DRAM of Micro OS")
    parser.add_argument(
        "--root",
        default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        help="the root of the repository",
    )
    parser.add_argument("--map", action="append", default=[], metavar="FILE", help="a linker map")
    parser.add_argument(
        "--size-json",
        action="append",
        default=[],
        metavar="FILE",
        help="a report of `idf.py size --format json`",
    )
    parser.add_argument("--json", metavar="FILE", help="write the report as JSON")
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="print the passing numbers too"
    )
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)

    try:
        measurements = collect_measurements(root, args.map, args.size_json)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"sram: cannot measure the build: {exc}", file=sys.stderr)
        return 1

    if not measurements:
        print(
            "sram: skip, no ESP-IDF build "
            "(build*/micro_os.map or build*/idf_size.json) is there"
        )
        return 0

    over = [measurement for measurement in measurements if measurement.used > SRAM_BUDGET_BYTES]

    for measurement in measurements:
        mark = "FAIL" if measurement in over else "ok"
        if mark == "ok" and not args.verbose:
            continue
        print(f"sram: {mark}: {measurement.source}")
        print(f"      static DRAM {measurement.used} bytes against {SRAM_BUDGET_BYTES}")
        print(
            f"      data {measurement.data} + bss {measurement.bss}, "
            f"headroom {measurement.headroom}"
        )
        if measurement.region is not None:
            print(f"      the region {measurement.region_name} is {measurement.region} bytes")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "ok": not over,
                    "budget": SRAM_BUDGET_BYTES,
                    "measurements": [
                        {
                            "source": measurement.source,
                            "data": measurement.data,
                            "bss": measurement.bss,
                            "used": measurement.used,
                            "headroom": measurement.headroom,
                            "region": measurement.region,
                        }
                        for measurement in measurements
                    ],
                },
                handle,
                indent=2,
            )
            handle.write("\n")

    if over:
        print(
            f"sram: the static DRAM is over its budget "
            f"({', '.join(str(item.used) for item in over)} bytes)"
        )
        return 1

    print(f"sram: the static DRAM fits its budget ({SRAM_BUDGET_BYTES} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
