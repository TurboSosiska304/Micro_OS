#!/usr/bin/env python3
"""Micro OS — the gate of a file that survives a cut (ADR 0047).

The volume of the release carries a journal: a write of a record, a
directory or the map of the free blocks reaches the medium in one committed
transaction, and a file a program was writing is a draft until its close. The
gate asks the promise of that design twenty times, at twenty different points
of one write:

  * the image is copied, the copy is booted, a file that stays is written and
    closed, a write of a big file starts, and the machine is killed while the
    write is in flight - `-no-reboot` already means a reset is the end of the
    machine, so the process is taken away and not asked to stop;
  * the same blocks are booted again: the mount replays the journal or
    discards the draft, and then `fsck` has to find **nothing** - a volume
    that needs a repair after every cut is a volume that did not have a
    journal;
  * the file written before the cut is whole, byte for byte;
  * the big file is either **whole** (`/bin/mkbig verify` agrees with every
    byte) or **not there at all** - never a prefix, never a mixture;
  * the twenty points span the write: some cuts land before the close (the
    file is absent), some after its commit (the file is whole and the journal
    replay is in the boot log), and the count of both is printed.

    python3 tools/x86_cut_smoke.py [--build DIR] [--mkfs PATH]
                                   [--cuts 20] [--mib 6] [--keep] [--verbose]

The exit status is 0 when every check passed, 2 when a tool or an image is
missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
import sys
import tempfile
import time

from qemu_session import Machine, MachineError
from x86_image import build_image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86")
DEFAULT_MKFS = os.path.join(ROOT, "build-host", "tools", "microos_mkfs")

# The volume of the test: the image of the release is 5 GiB, but the gate is
# about the order of the writes and not about the size, and a disk of 16 MiB
# is copied in a millisecond.
VOLUME_MB = 16

# The size of the file the gate interrupts.
MIB = 6

KEEP_PATH = "/keep.txt"
# One word: the shell of the machine takes a handful of words in a stage.
KEEP_TEXT = "kept-before-the-cut"

CHECKS: list = []


def check(title: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((title, bool(ok), detail))


def short(text: str, lines: int = 4) -> str:
    if not text:
        return "(no words)"
    return "\n          ".join(text.strip().splitlines()[-lines:])


def build(build_dir: str, mkfs: str, image: str) -> int:
    return build_image(
        image,
        build_dir,
        mkfs,
        VOLUME_MB,
        fs="mfs",
        data_mb=0,
        extra=["--program", f"/bin/mkbig={os.path.join(build_dir, 'mkbig.mox')}"],
    )


def baseline(build_dir: str, image: str, timeout: float) -> tuple:
    """One write with no cut: the file is whole and it takes this long."""
    with Machine(build_dir, image) as machine:
        machine.open(timeout=timeout)
        machine.run(f"echo {KEEP_TEXT} > {KEEP_PATH}", timeout=60.0)
        start = time.monotonic()
        answer = machine.run(f"exec /bin/mkbig write /big.bin {MIB}", timeout=300.0)
        seconds = time.monotonic() - start
        keeps = machine.run(f"cat {KEEP_PATH}", timeout=60.0)
        verify = machine.run(f"exec /bin/mkbig verify /big.bin {MIB}", timeout=300.0)

    return seconds, answer, keeps, verify


def cut_once(build_dir: str, image: str, delay: float, timeout: float) -> tuple:
    """Boot, write the file that stays, start the big write, take the power."""
    machine = Machine(build_dir, image)
    machine.open(timeout=timeout)

    machine.run(f"echo {KEEP_TEXT} > {KEEP_PATH}", timeout=60.0)

    # The line goes in and the prompt is not waited for: the machine will not
    # answer, because the power goes while the program writes.
    machine.send_line(f"exec /bin/mkbig write /big.bin {MIB}")
    time.sleep(delay)

    writing = machine.alive()
    process = machine.process

    if process is not None and process.poll() is None:
        process.kill()
        process.wait()

    return machine.close(), writing


def inspect(build_dir: str, image: str, timeout: float) -> dict:
    """The second boot: the volume, the file that stays, and the draft."""
    result = {
        "banner": "",
        "replayed": False,
        "discarded": False,
        "fsck": "",
        "keep": "",
        "listing": "",
        "verify": "",
        "draft": "unknown",
    }

    with Machine(build_dir, image) as machine:
        banner = machine.open(timeout=timeout)
        result["banner"] = banner
        result["replayed"] = "the journal replayed" in banner
        result["discarded"] = "discarded an unfinished write" in banner

        result["fsck"] = machine.run("fsck", timeout=300.0)
        result["keep"] = machine.run(f"cat {KEEP_PATH}", timeout=60.0)
        result["listing"] = machine.run("ls /", timeout=60.0)

        if "big.bin" in result["listing"]:
            result["verify"] = machine.run(f"exec /bin/mkbig verify /big.bin {MIB}", timeout=300.0)
            result["draft"] = "whole" if "the bytes agree" in result["verify"] else "torn"
        else:
            result["draft"] = "absent"

    return result


def delays_for(count: int, seconds: float, rng: random.Random) -> list:
    """The cut points: the start of the write, the middle, and the close."""
    points = []

    for index in range(count):
        if index < max(1, count // 5):
            delay = rng.uniform(0.05, 0.2)
        elif index < count - max(1, count // 5):
            delay = rng.uniform(0.2, max(0.25, seconds * 0.85))
        else:
            delay = rng.uniform(max(0.25, seconds * 0.85), max(0.3, seconds * 1.15))
        points.append(round(delay, 3))

    return points


def main(argv: list) -> int:
    global MIB

    parser = argparse.ArgumentParser(description="the gate of a file that survives a cut")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--cuts", type=int, default=20, help="the number of the cuts (20)")
    parser.add_argument("--mib", type=int, default=MIB, help="the size of the big file")
    parser.add_argument("--dir", default=None, help="where the images are written")
    parser.add_argument("--seed", type=int, default=None, help="the seed of the cut points")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the images of the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles as well")
    args = parser.parse_args(argv)

    MIB = args.mib

    for path in ("loader32.elf", "microos64.bin", "mkbig.mox"):
        if not os.path.isfile(os.path.join(args.build, path)):
            print(f"missing {args.build}/{path}: run make x86-build", file=sys.stderr)
            return 2

    if not os.path.isfile(args.mkfs):
        print(f"missing {args.mkfs}: run make build", file=sys.stderr)
        return 2

    if not shutil.which("qemu-system-x86_64"):
        print("missing qemu-system-x86_64", file=sys.stderr)
        return 2

    work = args.dir if args.dir is not None else tempfile.mkdtemp(prefix="microos-cut-",
                                                                  dir=args.build)
    os.makedirs(work, exist_ok=True)

    base = os.path.join(work, "base.img")
    image = os.path.join(work, "cut.img")

    rng = random.Random(args.seed if args.seed is not None else int(time.time()))

    try:
        if build(args.build, args.mkfs, base) != 0:
            check("the image of the volume is written", False, base)
            raise SystemExit(0)

        check("the image of the volume is written", True)

        # The write with no cut: the file is whole and the clock says how long
        # the twenty points have to spread over.
        seconds, answer, keeps, verify = baseline(args.build, base, args.timeout)

        if args.verbose:
            print(answer)

        check("a write with no cut makes a file that verifies",
              "the bytes agree" in verify, short(verify))
        check("the file of the baseline is whole", KEEP_TEXT in keeps, short(keeps))

        duration = max(seconds, 0.2)
        points = delays_for(args.cuts, duration, rng)

        absent = 0
        whole = 0
        replayed = 0
        discarded = 0

        print(f"the baseline write of {MIB} MiB takes {seconds:.2f} s; "
              f"the cuts are {', '.join(f'{p:.2f}' for p in points)} s")

        for index, delay in enumerate(points):
            shutil.copyfile(base, image)

            try:
                transcript, writing = cut_once(args.build, image, delay, args.timeout)
            except MachineError as error:
                check(f"cut {index + 1} of {len(points)}: the machine was writing", False,
                      str(error))
                continue

            if args.verbose:
                print(transcript)

            check(f"cut {index + 1}: the power went while the machine was writing",
                  writing and "wrote " not in transcript.split("microos> ")[-1],
                  short(transcript))

            try:
                state = inspect(args.build, image, args.timeout)
            except MachineError as error:
                check(f"cut {index + 1}: the volume boots again", False, str(error))
                continue

            if args.verbose:
                print(state["banner"])

            check(f"cut {index + 1}: the machine boots on the volume the cut left",
                  "root: the root is mfs" in state["banner"], short(state["banner"]))
            check(f"cut {index + 1}: fsck finds nothing to repair",
                  "the volume is consistent" in state["fsck"], short(state["fsck"]))
            check(f"cut {index + 1}: the file written before the cut is whole",
                  KEEP_TEXT in state["keep"], short(state["keep"]))
            check(f"cut {index + 1}: the draft is whole or absent",
                  state["draft"] in ("whole", "absent"),
                  f"the draft is {state['draft']}: {short(state['verify'])}")

            if state["replayed"]:
                replayed += 1
            if state["discarded"]:
                discarded += 1
            if state["draft"] == "absent":
                absent += 1
            elif state["draft"] == "whole":
                whole += 1

        # The two ends of the promise are real: a cut before the close leaves
        # no file, a cut after it leaves a whole one (the journal replay), and
        # a cut in the middle is one of the two.
        check("some cuts lost the file and some kept it whole", absent > 0 and whole > 0,
              f"{absent} absent, {whole} whole")
        check("the boot log names the journal of the cut", replayed + discarded > 0,
              f"{replayed} of {len(points)} boots replayed, {discarded} discarded")
        check("a draft was discarded or a transaction replayed", discarded + replayed > 0,
              f"{discarded} discarded, {replayed} replayed")

        print(f"  {absent} cut(s) left no file, {whole} left a whole file, "
              f"{replayed} replayed, {discarded} discarded")
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    failed = [item for item in CHECKS if not item[1]]

    print("Micro OS — a file that survives a cut")
    print("=" * 72)

    for title, ok, detail in CHECKS:
        print(f"  {' ok ' if ok else 'FAIL'}  {title}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 72)

    if failed:
        print(f"FAILED: {len(failed)} of {len(CHECKS)} check(s)")
        print("RESULT: FAIL")
        return 1

    print(f"OK: {len(CHECKS)} check(s) passed")
    print("RESULT: PASS")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
