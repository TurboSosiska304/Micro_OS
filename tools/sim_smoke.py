#!/usr/bin/env python3
"""Micro OS — the smoke test of the host simulator.

The unit tests check the kernel piece by piece. This script checks the
whole thing at once: the simulator boots, the subsystems come up, the
volume of MicroFS and the configuration of /etc work, the shell answers.
It is the cheapest end to end test of the kernel and it runs in a couple
of seconds.

Usage:
    tools/sim_smoke.py [--binary PATH] [--timeout SECONDS] [--verbose]

The exit status is 0 when the simulator behaved and 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# The commands that are fed to the shell of the simulator, in this order: the
# checks below read the answer of a command by its place in the list.
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
    "ls /",
    "write /etc/smoke hello",
    "cat /etc/smoke",
    "df",
    "config sync",
    "config",
    "mkfs",
    "mkfs -y",
    "ls /",
]

# The prompt of the shell. It is written in one call, so the log of another
# task never splits it: the answers are separated by it and read by position,
# which does not depend on the echo of the typed line.
PROMPT = "microos> "

# The noise of the tasks that run beside the shell: the heartbeat and the
# workers write to the console in parallel, so their numbers are cut out
# before the output is checked.
HEARTBEAT = re.compile(r"heartbeat: uptime \d+ ms, \d+ task\(s\)")


def normalize(output: str) -> str:
    """The output of the simulator without the noise of the heartbeat."""
    return HEARTBEAT.sub("heartbeat: uptime", output)


def answers(output: str) -> list:
    """The blocks of the transcript, one per command of COMMANDS.

    The block of a command holds the echo of the line and the answer of the
    shell. The echo can be cut by the log of another task; the prompt cannot,
    so the commands are matched by their place and not by their text.
    """
    return output.split(PROMPT)[1:]


def run_simulator(binary: str, timeout: float, env: dict) -> tuple:
    """Feed the commands to the simulator and collect the output."""
    process = subprocess.Popen(
        [binary],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
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

    blocks = answers(output)

    def answer(command: str) -> str:
        """The answer of one command of the list, empty when it did not run."""
        try:
            index = COMMANDS.index(command)
        except ValueError:
            return ""
        return blocks[index] if index < len(blocks) else ""

    # --- the boot
    expect("the kernel prints its name", "MicroOS" in output)
    expect("the board is reported", "board: host_sim" in output)
    expect("the architecture is reported", "arch:" in output)
    expect("the scheduler comes up", "[ ok ] sched" in output)
    expect("the volume is registered", "[ ok ] storage" in output)
    expect("the volume is a file of the machine", "sim: the volume is /" in output)
    expect("the filesystem comes up", "[ ok ] fs" in output)
    expect("the volume is mounted", "KB mounted" in output)
    expect("the tasks come up", "[ ok ] tasks" in output)
    expect("the subsystems are ready", "subsystem(s) ready" in output)

    # --- the shell
    expect("the shell starts", "MicroOS shell ready" in output)
    expect(
        "the answers follow the commands",
        len(blocks) >= len(COMMANDS),
        f"{len(blocks)} block(s)",
    )

    expect("help lists the commands", "list the available commands" in answer("help"))

    expect(
        "uname prints the version",
        re.search(r"MicroOS \d+\.\d+\.\d+", answer("uname")) is not None,
    )

    ps_text = answer("ps")
    expect(
        "ps prints the table of the tasks",
        re.search(r"pid\s+prio\s+state\s+name", ps_text) is not None,
    )
    expect("ps lists the heartbeat task", "heartbeat" in ps_text)
    expect("ps lists the shell task", "shell" in ps_text)

    expect(
        "uptime prints the time",
        re.search(r"^up \d+ s", answer("uptime"), re.MULTILINE) is not None,
    )
    expect("free prints the memory", "heap:" in answer("free"))

    # --- the ABI
    section = answer("abi")
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

    # --- the shell
    expect(
        "echo prints its argument",
        re.search(r"^smoke-test\s*$", answer("echo smoke-test"), re.MULTILINE) is not None,
    )
    expect("pwd prints the current directory", "/" in answer("pwd"))
    expect("dmesg prints the log of the kernel", "[ ok ] sched" in answer("dmesg"))

    # --- the filesystem
    root = answer("ls /")
    expect("ls / lists the procfs", "proc/" in root)
    expect("ls / lists the devfs", "dev/" in root)

    written = answer("write /etc/smoke hello")
    expect("write creates a file", "byte(s) written to /etc/smoke" in written)
    expect("cat prints what write wrote", "hello" in answer("cat /etc/smoke"))

    df_text = answer("df")
    expect("df prints the volume", re.search(r"blocks: \d+ of \d+ free", df_text) is not None)
    expect("df knows the block size", "KB each" in df_text)
    expect("df prints the inodes", "inodes:" in df_text)

    # --- the configuration
    expect(
        "config sync saves /etc",
        re.search(r"restored, \d+ saved", answer("config sync")) is not None,
    )
    expect("config lists the key of /etc", "smoke=hello" in answer("config"))

    # --- the volume is made again
    expect("mkfs refuses without -y", "usage: mkfs -y" in answer("mkfs"))
    expect("mkfs formats the volume", "the volume is formatted" in answer("mkfs -y"))

    root = answer("ls /")
    expect("ls / works after a format", "proc/" in root and "dev/" in root)

    return checks


def make_env(workdir: str) -> dict:
    """The environment that gives this run a volume and a store of its own.

    The volume and the key-value store of the simulator live in the files of
    the machine, so a run that used the default ones would depend on the run
    before it: the second `make smoke` would find a volume that is already
    formatted, /etc that is already there and a store with the key of the
    last run.
    """
    env = dict(os.environ)
    env["MICROOS_VOLUME"] = os.path.join(workdir, "volume.img")
    env["MICROOS_KV_DIR"] = os.path.join(workdir, "kv")

    return env


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
    parser.add_argument(
        "--keep",
        action="store_true",
        help="keep the volume and the store of the run (they are removed by default)",
    )
    args = parser.parse_args(argv)

    if not os.path.isfile(args.binary) or not os.access(args.binary, os.X_OK):
        print(f"the simulator is not found: {args.binary}", file=sys.stderr)
        print("build it first: make build", file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="microos-sim-")
    try:
        output, status = run_simulator(args.binary, args.timeout, make_env(workdir))
    finally:
        if args.keep:
            print(f"the volume of the run is kept in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

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
