#!/usr/bin/env python3
"""Micro OS — the disk of a stress session, filled to `ENOSPC` on purpose.

The session of 1.0 has to see a volume with no free block: only then do the
error paths of the filesystem run (a create that cannot take a block, a
write that cannot grow, the repair of what a full volume left). Typing a
write loop to fill 6.8 MB over a 128-byte line would take an hour, so the
volume is filled by the tool that makes it: one file, `/filler`, is written
by `microos_mkfs` (the same `--file` the image builder uses for a program).

The size is not a constant of the tree. Programs grow and take blocks, and a
number copied here would be a second, silent definition of how much the
volume holds. The tool builds the volume, asks the machine for `df`, and
moves the filler by the free space it sees, until a boot reports zero free
blocks. Two boots are the usual cost; the result is kept next to the image
and reused while the kernel is older than it.

    tools/soak_image.py --build build-x86 --image build-x86/soak.img
                        --mkfs build-host/tools/microos_mkfs [--if-needed]

The exit status is 0 when the image is full, 2 when QEMU or the tools are
missing, and 1 when the filler could not be made to fit.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

from qemu_session import Machine, MachineError
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# A starting guess of the filler: a little under the free space of an 8 MB
# image of this tree. The loop below moves it to the exact block.
FILLER_START = 6_900_000
FILLER_STEP = 4096
FILLER_TRIES = 12

MARKER = ".full"


def free_blocks(build: str, image: str, timeout: float) -> int | None:
    """Boot the image and read the free blocks of the volume at `/`."""
    try:
        with Machine(build, image) as machine:
            machine.open(timeout)
            block = machine.run("df", timeout=timeout)
    except MachineError as error:
        print(f"soak-image: {error}", file=sys.stderr)
        return None

    match = re.search(r"blocks:\s*(\d+)\s+of\s+(\d+)\s+free", block)

    if match is None:
        print(f"soak-image: df did not answer a volume: {block!r}", file=sys.stderr)
        return None

    return int(match.group(1))


def make_filler(path: str, size: int) -> None:
    """A sparse file of `size` zero bytes: mkfs reads zeros and writes blocks."""
    with open(path, "wb") as handle:
        handle.truncate(size)


def build_full_image(
    build: str,
    mkfs: str,
    image: str,
    size_mb: int = 8,
    timeout: float = 40.0,
    verbose: bool = False,
) -> bool:
    """Build `image` until its volume reports zero free blocks."""
    filler = image + ".filler"
    candidate = FILLER_START

    for attempt in range(FILLER_TRIES):
        make_filler(filler, candidate)
        extra = ["--dir", "/tmp", "--file", f"/filler={filler}"]
        rc = build_image(
            image,
            build_dir=build,
            mkfs=mkfs,
            size_mb=size_mb,
            verbose=verbose,
            extra=extra,
        )

        if rc != 0:
            # The filler did not fit: the volume is smaller than the guess.
            # The step is a fraction of the guess and not one block: a build
            # with a larger set of programs leaves a megabyte less than the
            # one the last constant was measured on, and a walk down by 4 KiB
            # would spend every attempt in the same failed neighbourhood.
            removed = max(FILLER_STEP, candidate // 32)
            candidate -= removed
            if verbose:
                print(
                    f"soak-image: {candidate + removed} bytes did not fit, trying {candidate}",
                    file=sys.stderr,
                )
            if candidate <= 0:
                break
            continue

        free = free_blocks(build, image, timeout)

        if free is None:
            return False

        if verbose:
            print(f"soak-image: attempt {attempt + 1}, filler {candidate}, free {free} block(s)")

        if free == 0:
            with open(image + MARKER, "w", encoding="utf-8") as handle:
                json.dump({"filler": candidate, "size_mb": size_mb}, handle)
            if os.path.exists(filler):
                os.remove(filler)
            return True

        candidate += free * 1024

    print("soak-image: the filler did not reach zero free blocks", file=sys.stderr)
    return False


def is_ready(image: str, build: str) -> bool:
    """Is the image there, full, and newer than the kernel it boots?"""
    if not os.path.isfile(image) or not os.path.isfile(image + MARKER):
        return False

    kernel = os.path.join(build, "microos64.bin")

    if os.path.isfile(kernel) and os.path.getmtime(kernel) > os.path.getmtime(image):
        return False

    return True


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the full volume of a stress session")
    parser.add_argument("--build", default=os.path.join(ROOT, "build-x86"))
    parser.add_argument(
        "--mkfs", default=os.path.join(ROOT, "build-host", "tools", "microos_mkfs")
    )
    parser.add_argument("--image", default=os.path.join(ROOT, "build-x86", "soak.img"))
    parser.add_argument("--size-mb", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument(
        "--if-needed",
        action="store_true",
        help="keep the image when it is newer than the kernel",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    for path in (os.path.join(args.build, "loader32.elf"),
                 os.path.join(args.build, "microos64.bin"),
                 args.mkfs):
        if not os.path.isfile(path):
            print(f"soak-image: missing {path}", file=sys.stderr)
            return 2

    if args.if_needed and is_ready(args.image, args.build):
        if args.verbose:
            print(f"soak-image: {args.image} is full and current")
        return 0

    if os.path.exists(args.image + MARKER):
        os.remove(args.image + MARKER)

    if not build_full_image(args.build, args.mkfs, args.image, args.size_mb, args.timeout,
                            args.verbose):
        return 1

    print(f"soak-image: {args.image}, no free block; the load starts with ENOSPC")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
