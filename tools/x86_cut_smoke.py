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
  * the point of every cut is a fraction of the write the baseline measured
    on this host (15-75 % of it), not a wall-clock guess: a write of 6 MiB
    takes 0.7 s on one machine and 3 s on another, and a point after the end
    is a cut that missed the middle and says so, not a verdict on the file;
  * the same blocks are booted again: the mount replays the journal or
    discards the draft, and then `fsck` has to find **nothing** - a volume
    that needs a repair after every cut is a volume that did not have a
    journal;
  * the file written before the cut is whole, byte for byte;
  * the big file is either **whole** (`/bin/mkbig verify` agrees with every
    byte) or **not there at all** - never a prefix, never a mixture;
  * the points are a share of the write the probe measured on this host
    (15-75 % of it), so a cut lands in the middle by the rule and not by
    luck. A run of the write that is faster than the probe is retried, and a
    point that is still after the end is "a cut that missed the middle" - a
    failure of the gate, not a verdict on the file;
  * the file is whole or absent and the count of both is printed. The
    baseline (no cut) proves the whole path; the replay of a committed
    transaction is proved by the host test (`test_mfs_journal.c`), because
    the window between the commit and the checkpoint is milliseconds and no
    wall clock aims at it.

    python3 tools/x86_cut_smoke.py [--build DIR] [--mkfs PATH]
                                   [--cuts 20] [--mib 6] [--keep] [--verbose]

The exit status is 0 when every check passed, 2 when a tool or an image is
missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
import random
import re
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
    """One write with no cut: the file is whole and it takes this long.

    The number comes from the line of `mkbig` (`wrote N bytes in X ms`), which
    is the write itself and not the shell around it: it is the length the cut
    points of the gate are a fraction of. The wall clock of the call is the
    answer when the line is not there.
    """
    with Machine(build_dir, image) as machine:
        machine.open(timeout=timeout)
        machine.run(f"echo {KEEP_TEXT} > {KEEP_PATH}", timeout=60.0)
        start = time.monotonic()
        answer = machine.run(f"exec /bin/mkbig write /big.bin {MIB}", timeout=300.0)
        seconds = time.monotonic() - start
        keeps = machine.run(f"cat {KEEP_PATH}", timeout=60.0)
        verify = machine.run(f"exec /bin/mkbig verify /big.bin {MIB}", timeout=300.0)

    match = re.search(r"mkbig: wrote \d+ bytes in (\d+) ms", answer)
    if match is not None:
        seconds = int(match.group(1)) / 1000.0

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


# The share of the measured write a cut aims at: the first fifth of it and the
# last fifth are not the middle, and everything between them is. A retry of a
# cut that missed (the write of this run was faster than the baseline) aims
# lower still.
CUT_LOW = 0.15
CUT_HIGH = 0.75
CUT_RETRY_LOW = 0.10
CUT_RETRY_HIGH = 0.50
CUT_ATTEMPTS = 3


def cut_delay(seconds: float, rng: random.Random, retry: bool) -> float:
    """A point inside the write, as a share of the length it measured."""
    low = CUT_RETRY_LOW if retry else CUT_LOW
    high = CUT_RETRY_HIGH if retry else CUT_HIGH

    return round(rng.uniform(low, high) * seconds, 3)


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

        absent = 0
        whole = 0
        replayed = 0
        discarded = 0
        missed = 0
        attempts = 0

        print(f"the baseline write of {MIB} MiB takes {duration:.2f} s; every cut "
              f"is a point of {int(CUT_LOW * 100)}-{int(CUT_HIGH * 100)} % of it")

        for index in range(args.cuts):
            hit = False
            transcript = ""
            delay = 0.0

            # A cut aims inside the write; a run that was faster than the
            # baseline can finish it before the power goes, and that is a cut
            # that missed the middle - it is tried again, lower, and it is not
            # a verdict on the file.
            for trial in range(1, CUT_ATTEMPTS + 1):
                attempts += 1
                delay = cut_delay(duration, rng, retry=(trial > 1))
                shutil.copyfile(base, image)

                try:
                    transcript, writing = cut_once(args.build, image, delay, args.timeout)
                except MachineError as error:
                    transcript = str(error)
                    writing = False
                    break

                if args.verbose:
                    print(transcript)

                if writing and "mkbig: wrote " not in transcript:
                    hit = True
                    break

                if args.verbose:
                    print(f"  cut {index + 1}: {delay:.3f} s missed the write")

            check(f"cut {index + 1}: the cut reached the middle of the write", hit,
                  f"a cut that missed the middle after {trial} attempt(s) "
                  f"(last point {delay:.3f} s of {duration:.3f} s): {short(transcript)}")

            if not hit:
                missed += 1

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

        # The gate is about a cut inside the write: a point after its end is a
        # cut that missed the middle, and it fails as the gate and not as the
        # file. The file is asked the same question either way.
        check("every cut reached the middle of the write", missed == 0,
              f"a cut that missed the middle: {missed} of {args.cuts} "
              f"(up to {CUT_ATTEMPTS} attempt(s) each)")
        check("the boot log names the journal of the cut", replayed + discarded > 0,
              f"{replayed} of {args.cuts} boots replayed, {discarded} discarded")

        print(f"  {absent} cut(s) left no file, {whole} left a whole file; "
              f"{replayed} replayed, {discarded} discarded; "
              f"{attempts} attempt(s), {missed} missed the middle")
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
