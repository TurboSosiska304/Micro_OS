#!/usr/bin/env python3
"""The pipeline of the smoke, five hundred times, on two cores.

A reproduction of the one failure that the session of two cores showed five
times in 844 cycles: `ls /bin | wc -l` answered nothing, so the number of the
files of the volume never reached the block between two prompts. The block of
a command is the text of the console from the prompt before it to the prompt
after it, and the number missing there means the answer of the last stage
arrived too late, or never.

Two cores, the image of `make soak-smp`, the same command in a loop, and the
count of the blocks without a number. It is not a gate: it is the tool of a
bug that a gate found once.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qemu_session import Machine, MachineClosed, MachineTimeout  # noqa: E402

BUILD = "build-x86-smp"
IMAGE = "build-x86-smp/soak.img"
ROUNDS = int(os.environ.get("ROUNDS", "500"))
LINE = "ls /bin | wc -l"
NUMBER = re.compile(r"(?m)^\s*\d+\s*$")


def main() -> int:
    # The point of this tool is the eighth core: without `-smp 8` the machine
    # is the one-core port and the hang it reproduces never arrives.
    machine = Machine(BUILD, IMAGE, extra_args=["-smp", "8"])

    try:
        machine.open(timeout=120.0)
    except (MachineClosed, MachineTimeout) as error:
        print(f"boot: {error}")
        return 2

    missing = 0

    for i in range(ROUNDS):
        try:
            machine.send_line(LINE)
            block = machine.wait_prompt(timeout=30.0)
        except (MachineClosed, MachineTimeout) as error:
            print(f"round {i}: {error}")
            return 2

        if NUMBER.search(block) is None:
            missing += 1
            print(f"round {i}: no number in {block[-200:]!r}")
            with open("/tmp/opencode/pipe-smp-miss.log", "ab") as handle:
                handle.write(block.encode("utf-8", "replace") + b"\n---\n")

    machine.close()
    print(f"{ROUNDS - missing} of {ROUNDS} answered")
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
