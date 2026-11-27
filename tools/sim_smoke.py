#!/usr/bin/env python3
"""Micro OS — the smoke test of the host simulator.

The unit tests check the kernel piece by piece. This script checks the
whole thing at once: the simulator boots, the subsystems come up, the
volume of MicroFS and the configuration of /etc work, the shell answers,
and the HTTP client fetches a page of the test process over the loopback.
It is the cheapest end to end test of the kernel and it runs in a couple
of seconds.

Usage:
    tools/sim_smoke.py [--binary PATH] [--timeout SECONDS] [--verbose]

The exit status is 0 when the simulator behaved and 1 otherwise.
"""

from __future__ import annotations

import argparse
import http.server
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

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
    "tree /etc",
    "tree -d /etc",
    "df",
    "config sync",
    "config",
    "mkfs",
    "mkfs -y",
    "ls /",
    "df /nope",
    "umount /proc",
    "path \"\"",
    "cd /proc",
    "pwd",
    "cd .",
    "pwd",
    "cd //proc//",
    "pwd",
    "cd ..",
    "pwd",
    "ls ..",
    "cd /",
    "modules",
    "date",
    "mfetch",
    "battery",
    "wifi status",
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


def run_simulator(binary: str, timeout: float, env: dict, commands: list) -> tuple:
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
        stdout, _ = process.communicate("\n".join(commands) + "\n", timeout=timeout)
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


def check(output: str, commands: list) -> list:
    """The expectations about the output of the simulator."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    blocks = answers(output)

    def answer(command: str, occurrence: int = 0) -> str:
        """The answer of one command of the list, empty when it did not run.

        A command may be typed twice to see the two sides of a rule - `pwd`
        after `cd .` and after `cd ..` - so the occurrence decides which
        block is the answer.
        """
        indexes = [index for index, line in enumerate(commands) if line == command]
        if occurrence >= len(indexes):
            return ""
        index = indexes[occurrence]
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
    # The ring keeps the newest messages; early boot lines may already have
    # aged out after the configuration and log tasks have written theirs.
    expect("dmesg prints the log of the kernel", "sim: init is up" in answer("dmesg"))

    # --- the filesystem
    root = answer("ls /")
    expect("ls / lists the procfs", "proc/" in root)
    expect("ls / lists the devfs", "dev/" in root)

    written = answer("write /etc/smoke hello")
    expect("write creates a file", "6 bytes written to /etc/smoke" in written)
    expect("cat prints what write wrote", "hello" in answer("cat /etc/smoke"))

    # --- the tree: -d draws the directories and counts every entry
    tree = answer("tree /etc")
    tree_d = answer("tree -d /etc")
    counts = re.search(r"(\d+) director(?:y|ies), (\d+) files?", tree)
    counts_d = re.search(r"(\d+) director(?:y|ies), (\d+) files?", tree_d)
    expect("tree draws the branches of /etc", "|-- " in tree or "`-- " in tree, tree)
    expect("tree counts the entries of /etc", counts is not None, tree)
    expect("tree -d draws no file", "clock" not in tree_d and "setting" not in tree_d, tree_d)
    expect(
        "tree -d counts the same files as tree",
        counts is not None and counts_d is not None and counts.groups() == counts_d.groups(),
        f"{tree_d} against {tree}",
    )
    expect(
        "tree -d counts the files it did not draw",
        counts_d is not None and int(counts_d.group(2)) > 0,
        tree_d,
    )

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
    expect(
        "df refuses a path that is not there",
        "df: /nope: no such file or directory" in answer("df /nope"),
        answer("df /nope"),
    )
    expect(
        "a filesystem of the kernel is not unmounted",
        "/proc is mounted by the kernel" in answer("umount /proc"),
        answer("umount /proc"),
    )
    expect(
        "path refuses the empty value",
        "an empty value is not a list of directories" in answer('path ""'),
        answer('path ""'),
    )

    # --- `.` and `..` in a path: the shell normalizes before the call
    expect("cd into a directory works", "/proc" in answer("pwd", 1), answer("pwd", 1))
    expect("cd . stays in the directory", "/proc" in answer("pwd", 2), answer("pwd", 2))
    expect(
        "repeated and trailing slashes are one path",
        "/proc" in answer("pwd", 3),
        answer("pwd", 3),
    )
    expect(
        "cd .. goes to the parent",
        re.search(r"(?m)^/\s*$", answer("pwd", 4)) is not None,
        answer("pwd", 4),
    )
    expect(
        "ls .. lists the parent",
        any(name in answer("ls ..") for name in ("proc/", "dev/", "etc/")),
        answer("ls .."),
    )

    # --- the stubs of the board: the drivers of the host
    modules = answer("modules")
    expect("modules reports the PSRAM of the host", "psram" in modules)
    expect("modules reports the volume as the flash", "flash" in modules)
    expect("modules reports the clock", "rtc" in modules)
    expect("modules reports the missing radio", "wifi" in modules)
    expect("the doctor finds no problem", "0 problem(s)" in modules)

    expect(
        "date prints the moment in the shape of the board",
        re.search(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", answer("date")) is not None,
    )

    fetch = answer("mfetch")
    expect("mfetch prints the operating system", "OS" in fetch)
    expect("mfetch prints the machine", "Machine" in fetch)
    expect("mfetch prints the memory with a bar", "Memory" in fetch and "[" in fetch)
    expect("mfetch prints the cores the kernel runs", "core(s)" in fetch)
    expect("mfetch has no color codes without `color`", "\x1b[" not in fetch)

    expect("battery says the host has none", "no battery" in answer("battery"))
    expect("wifi status says there is no radio", "no radio" in answer("wifi status"))

    # --- the network over the loopback of the process
    http_command = commands[-1]
    http_answer = answer(http_command)
    expect(
        "http gets the page of the host",
        re.search(r"HTTP 200, \d+ byte", http_answer) is not None
        and "microos-sim-http" in http_answer,
        http_answer,
    )

    return checks


class PageHandler(http.server.BaseHTTPRequestHandler):
    """The page the HTTP client of the simulator fetches over the loopback."""

    def do_GET(self) -> None:  # noqa: N802 - the name of http.server
        body = b"microos-sim-http\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args) -> None:
        """The smoke test has no access log to print."""
        del args


def start_http_server() -> tuple:
    """A page of this process on a port of the loopback, and its URL."""
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PageHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    return server, f"http://127.0.0.1:{server.server_address[1]}/"


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
    server, page = start_http_server()
    commands = list(COMMANDS)
    commands.append(f"http get {page}")

    try:
        output, status = run_simulator(args.binary, args.timeout, make_env(workdir), commands)
    finally:
        server.shutdown()
        server.server_close()
        if args.keep:
            print(f"the volume of the run is kept in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    if args.verbose:
        print(output)

    if status is not None and status < 0:
        print(f"the simulator died with the signal {-status}", file=sys.stderr)
        return 1

    checks = check(output, commands)
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
