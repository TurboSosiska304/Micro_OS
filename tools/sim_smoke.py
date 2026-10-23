#!/usr/bin/env python3
"""Micro OS — the smoke test of the host simulator.

The unit tests check the kernel piece by piece. This script checks the
whole thing at once: the simulator boots, the subsystems come up, the shell
answers. It is the cheapest end to end test of the kernel and it runs in a
couple of seconds.

Usage:
    tools/sim_smoke.py [--binary PATH] [--timeout SECONDS] [--verbose]

The exit status is 0 when the simulator behaved and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

# The commands that are fed to the shell of the simulator.
COMMANDS = [
    "help",
    "uname",
    "ps",
    "uptime",
    "free",
    "abi",
    "echo smoke-test",
    "pwd",
    "dmesg",
]

# The noise of the heartbeat task: it writes to the console in parallel with
# the shell, so its lines are cut out before the output is checked.
HEARTBEAT = re.compile(r"heartbeat: uptime \d+ ms, \d+ task\(s\)")


def normalize(output: str) -> str:
    """The output of the simulator without the noise of the heartbeat."""
    return HEARTBEAT.sub("heartbeat: uptime", output)


def run_simulator(binary: str, timeout: float) -> tuple:
    """Feed the commands to the simulator and collect the output."""
    process = subprocess.Popen(
        [binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    try:
        stdout, _ = process.communicate("\n".join(COMMANDS) + "\n", timeout=timeout)
        status = process.returncode
    except subprocess.TimeoutExpired:
        # The main loop of the simulator never ends, the timeout is the
        # normal way to stop it.
        process.terminate()
        try:
            stdout, _ = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, _ = process.communicate()
        status = None

    return normalize(stdout or ""), status


def check(output: str) -> list:
    """The expectations about the output of the simulator."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    # --- the boot
    expect("the kernel prints its name", "MicroOS" in output)
    expect("the board is reported", "board: host_sim" in output)
    expect("the architecture is reported", "arch:" in output)
    expect("the scheduler comes up", "[ ok ] sched" in output)
    expect("the tasks come up", "[ ok ] tasks" in output)
    expect("the subsystems are ready", "subsystem(s) ready" in output)

    # --- the shell
    expect("the shell starts", "MicroOS shell ready" in output)

    help_text = output.split("microos> help", 1)
    expect(
        "help lists the commands",
        len(help_text) > 1 and "list the available commands" in help_text[1],
    )

    uname_text = output.split("microos> uname", 1)
    expect(
        "uname prints the version",
        len(uname_text) > 1 and re.search(r"MicroOS \d+\.\d+\.\d+", uname_text[1]) is not None,
    )

    ps_text = output.split("microos> ps", 1)
    expect(
        "ps prints the table of the tasks",
        len(ps_text) > 1 and re.search(r"pid\s+prio\s+state\s+name", ps_text[1]) is not None,
    )
    expect(
        "ps lists the heartbeat task",
        len(ps_text) > 1 and "heartbeat" in ps_text[1],
    )

    uptime_text = output.split("microos> uptime", 1)
    expect("uptime prints the time", len(uptime_text) > 1 and "uptime:" in uptime_text[1])

    free_text = output.split("microos> free", 1)
    expect("free prints the memory", len(free_text) > 1 and "heap:" in free_text[1])

    # --- the ABI
    abi_text = output.split("microos> abi", 1)
    if len(abi_text) > 1:
        section = abi_text[1]
        rows = re.findall(r"^(\d+)\s+([a-z_][a-z0-9_]*)$", section, re.MULTILINE)
        summary = re.search(r"(\d+) call\(s\) registered", section)

        expect("abi prints the table of the calls", len(rows) > 0, f"{len(rows)} row(s)")
        expect("abi prints the summary", summary is not None)
        if summary is not None:
            expect(
                "the summary of the abi matches the table",
                int(summary.group(1)) == len(rows),
                f"{summary.group(1)} call(s) in the summary, {len(rows)} row(s) in the table",
            )
        names = [name for _, name in rows]
        for call in ("exit", "read", "write", "open", "waitpid", "malloc"):
            expect(f"the call {call} is registered", call in names)
    else:
        expect("abi prints the table of the calls", False, "the command produced no output")

    # --- the shell
    echo_text = output.split("microos> echo smoke-test", 1)
    expect(
        "echo prints its argument",
        len(echo_text) > 1 and re.search(r"^smoke-test\s*$", echo_text[1], re.MULTILINE) is not None,
    )

    pwd_text = output.split("microos> pwd", 1)
    expect("pwd prints the current directory", len(pwd_text) > 1 and "/" in pwd_text[1])

    dmesg_text = output.split("microos> dmesg", 1)
    expect(
        "dmesg prints the log of the kernel",
        len(dmesg_text) > 1 and "[ ok ] sched" in dmesg_text[1],
    )

    return checks


def main(argv: list) -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    parser = argparse.ArgumentParser(description="the smoke test of the host simulator")
    parser.add_argument(
        "--binary",
        default=os.path.join(root, "build-host", "ports", "port_host", "microos_sim"),
        help="the simulator to run",
    )
    parser.add_argument("--timeout", type=float, default=20.0, help="how long to wait, in seconds")
    parser.add_argument("--verbose", action="store_true", help="print the output of the simulator")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.binary) or not os.access(args.binary, os.X_OK):
        print(f"the simulator is not found: {args.binary}", file=sys.stderr)
        print("build it first: make build", file=sys.stderr)
        return 2

    output, status = run_simulator(args.binary, args.timeout)

    if args.verbose:
        print(output)

    if status is not None and status < 0:
        print(f"the simulator died with the signal {-status}", file=sys.stderr)
        return 1

    checks = check(output)
    failed = [c for c in checks if not c[1]]

    print("Micro OS — the smoke test of the simulator")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)
    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
