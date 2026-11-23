#!/usr/bin/env python3
"""Micro OS — the smoke test of a volume that loses the power in a write.

The other smokes of the machine end their session with the shell still
answering. This one ends it the way a board on a battery ends: in the middle
of a write of a file, with the machine gone. The question is the one of
ADR 0038, the record with a checksum: does the volume come back?

  * the machine boots again on the same image, which is the whole point -
    a volume whose header or table a write destroyed is a volume lost;
  * `fsck` either finds nothing or names what the interrupted write left,
    and `fsck -y` ends with a consistent volume and nothing left unfixed;
  * the files that were whole before the reset are whole after it: the
    content of the file written first comes back byte for byte, and the
    file the reset interrupted is either absent or shorter than the write
    that stopped, never a file of bytes nobody wrote;
  * a second `fsck` after the repair has nothing left to do.

The reset is the death of the QEMU process, not a reboot: `-no-reboot` means
a machine that resets ends, so a test that wants the volume of a power cut
takes the process away while the write is in flight.

    python3 tools/x86_mfs_smoke.py [--build DIR] [--mkfs PATH] [--keep]
                                   [--mib 16] [--delay 0.7] [--timeout 180]

The exit status is 0 when every check passed, 2 when a tool or an image is
missing, and 1 when a check failed.
"""

from __future__ import annotations

import argparse
import os
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

# The volume of the test: small enough to be built in a second, large enough
# for the file the reset interrupts.
VOLUME_MB = 16

# The file that is whole before the reset and has to be whole after it.
KEEP_PATH = "/keep.txt"
KEEP_TEXT = "these bytes were whole before the reset"

# The file the reset interrupts: 16 MiB at the rate of the disk of QEMU is
# about two seconds, so a kill a second into it is a write in flight.
BIG_MIB = 16
BIG_DELAY = 0.7

CHECKS: list = []


def check(title: str, ok: bool, detail: str = "") -> None:
    CHECKS.append((title, bool(ok), detail))


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


def reset_in_write(build_dir: str, image: str, delay: float, timeout: float) -> tuple:
    """Boot, write a file that stays, start a big write and take the power.

    The power is the death of the process and not a `close()`: a machine that
    is asked to stop flushes what it holds, and a board that loses its battery
    does not. Returns the words of the machine and whether it was still
    writing when it went, so that a failure of the test has the evidence in
    it.
    """
    machine = Machine(build_dir, image)

    machine.open(timeout=timeout)

    machine.run(f"echo {KEEP_TEXT} > {KEEP_PATH}", timeout=60.0)

    # The write that the reset interrupts: the line goes in, and the prompt
    # is not waited for, because the machine will not answer.
    machine.send_line(f"exec /bin/mkbig write /big.bin {BIG_MIB}")
    time.sleep(delay)

    writing = machine.alive()
    process = machine.process

    if process is not None and process.poll() is None:
        process.kill()
        process.wait()

    transcript = machine.close()

    return transcript, writing


def timing_hint(transcript: str) -> str:
    return transcript[-400:] if transcript else "(no words)"


def main(argv: list) -> int:
    global BIG_MIB

    parser = argparse.ArgumentParser(description="the smoke test of a reset in a write")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the directory of the kernel")
    parser.add_argument("--mkfs", default=DEFAULT_MKFS, help="the host tool of MicroFS")
    parser.add_argument("--dir", default=None, help="where the image is written")
    parser.add_argument("--mib", type=int, default=BIG_MIB, help="the size of the big file")
    parser.add_argument("--delay", type=float, default=BIG_DELAY,
                        help="the seconds before the power goes")
    parser.add_argument("--timeout", type=float, default=180.0, help="the seconds a boot gets")
    parser.add_argument("--keep", action="store_true", help="keep the image of the run")
    parser.add_argument("--verbose", action="store_true", help="print the consoles as well")
    args = parser.parse_args(argv)

    BIG_MIB = args.mib

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

    work = args.dir if args.dir is not None else tempfile.mkdtemp(prefix="microos-reset-",
                                                                  dir=args.build)
    os.makedirs(work, exist_ok=True)
    image = os.path.join(work, "reset.img")

    try:
        if build(args.build, args.mkfs, image) != 0:
            check("the image of the volume is written", False, image)
        else:
            check("the image of the volume is written", True)

            transcript, writing = reset_in_write(args.build, image, args.delay, args.timeout)

            if args.verbose:
                print(transcript)

            check("the power went while the machine was writing",
                  writing and "wrote " not in transcript.split("microos> ")[-1],
                  timing_hint(transcript))
    except MachineError as error:
        check("the machine was writing when the power went", False, str(error))

    # --- the same image, a second boot: the volume has to be there
    try:
        with Machine(args.build, image) as machine:
            banner = machine.open(timeout=args.timeout)

            if args.verbose:
                print(banner)

            check("the machine boots again on the volume the reset interrupted",
                  "root: the root is mfs on ata0p1" in banner, timing_hint(banner))

            answer = machine.run("df /", timeout=120.0)
            check("df / counts the blocks of the volume after the reset",
                  "blocks:" in answer and "inodes:" in answer, answer)

            # The file that was whole before the reset is whole after it.
            answer = machine.run(f"cat {KEEP_PATH}", timeout=60.0)
            check("the file written before the reset is whole", KEEP_TEXT in answer, answer)

            # The check of the volume: it may find the write that stopped, and
            # it may find nothing, but it must not fail to walk the volume.
            answer = machine.run("fsck", timeout=180.0)
            found = "problem(s)" in answer
            check("fsck walks the volume the reset left",
                  "the volume is consistent" in answer or found, answer)

            answer = machine.run("fsck -y", timeout=180.0)
            check("fsck -y leaves a consistent volume",
                  "the volume is consistent" in answer and "not repaired" not in answer, answer)

            answer = machine.run("fsck", timeout=180.0)
            check("the volume is consistent after the repair",
                  "the volume is consistent" in answer, answer)

            # The interrupted file is either not there or a file whose bytes
            # are the ones that were written: nothing of another object.
            answer = machine.run("ls /", timeout=60.0)
            check("the directory of the volume is readable", "keep.txt" in answer, answer)

            answer = machine.run("df /", timeout=120.0)
            check("the file the reset interrupted is not a hole in the volume",
                  "blocks:" in answer, answer)
    except MachineError as error:
        check("the machine boots again on the volume the reset interrupted", False, str(error))

    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)

    failed = [item for item in CHECKS if not item[1]]

    print("Micro OS — a reset in the middle of a write")
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
