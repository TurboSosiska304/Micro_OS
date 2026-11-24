#!/usr/bin/env python3
"""Micro OS — the board half of the session of 600 seconds.

`tools/board_soak.sh` builds, flashes and resets the board; this process is
the one that spends the ten minutes with it. One boot, one session, the
same load every cycle: the programs of `/bin`, files written and removed,
the display, the radio, telnet and the shell itself. A `Panic`, a `Guru
Meditation`, a backtrace, a reboot, a command that should pass and does not,
or a silence of the console is a failure.

    tools/board_soak_session.py --port /dev/ttyACM0 [--minutes N]
                                [--seconds N] [--keep-log]

The memory, the block count of the volume and the tasks of the shell are
read before and after the load; a small drift is allowed because a driver
may cache a buffer, and the report prints both numbers so that a reader
sees what the allowance covers.
"""

from __future__ import annotations

import argparse
import datetime
import os
import random
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - the board tools need pyserial
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise SystemExit(3)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = "microos> "
DEFAULT_MINUTES = 10.0

FATAL = [
    "Panic",
    "PANIC",
    "panic:",
    "Guru Meditation",
    "Core dump",
    "Backtrace:",
    "Rebooting",
    "assert failed",
    "Assertion failed",
    "abort()",
    "*** ERROR",
]

PS_ROW = re.compile(r"^\s*(\d+)\s+\d+\s+\w+\s+(\S+)\s*$", re.MULTILINE)
FD_ROW = re.compile(r"^fds\s+(\d+)\s*$", re.MULTILINE)
HEAP_FREE = re.compile(r"heap:\s*(\d+) KB free")
DF_FREE = re.compile(r"blocks:\s*(\d+)\s+of\s+(\d+)\s+free")

BOOT_EXPECT = ["MicroOS shell ready"]


class BoardFailure(RuntimeError):
    """The console or the board broke the rules of the session."""


class Command:
    __slots__ = ("text", "expect", "forbid", "soft")

    def __init__(self, text: str, expect=(), forbid=(), soft: bool = False) -> None:
        self.text = text
        self.expect = list(expect)
        self.forbid = list(forbid)
        self.soft = soft


def matches(answer: str, expectation) -> bool:
    if isinstance(expectation, re.Pattern):
        return expectation.search(answer) is not None
    return expectation in answer


class Board:
    """One serial port and the shell on the other end."""

    def __init__(self, port: str, baud: int, hang: float, timeout: float) -> None:
        self.port = port
        self.baud = baud
        self.hang = hang
        self.timeout = timeout
        self.serial: serial.Serial | None = None
        self.transcript = ""
        self.boot_seconds = 0.0
        self.last_output = time.monotonic()

    # --- the port -------------------------------------------------------

    def open(self) -> None:
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=0.2)
        except Exception as error:  # noqa: BLE001 - the message is of the port
            raise BoardFailure(f"cannot open {self.port}: {error}") from error

    def close(self) -> None:
        if self.serial is not None:
            self.serial.close()
            self.serial = None

    def reset(self) -> None:
        """The auto reset of the StickC Plus2: IO0 high, a pulse on EN."""
        assert self.serial is not None
        self.serial.reset_input_buffer()
        self.serial.dtr = False
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False
        time.sleep(0.2)
        self.transcript = ""
        self.last_output = time.monotonic()

    def _read(self) -> str:
        assert self.serial is not None
        data = self.serial.read(256)
        if not data:
            return ""
        text = data.decode("utf-8", errors="replace")
        self.transcript += text
        self.last_output = time.monotonic()
        return text

    def _fatal(self, text: str) -> str | None:
        for marker in FATAL:
            if marker in text:
                return marker
        return None

    def boot(self) -> str:
        """Read until the shell is ready; return the boot part of the console."""
        started = time.monotonic()
        start = len(self.transcript)

        while True:
            self._read()
            fresh = self.transcript[start:]
            marker = self._fatal(fresh)
            if marker is not None:
                raise BoardFailure(f"'{marker}' in the boot")
            if "MicroOS shell ready" in fresh and fresh.count(PROMPT) >= 1:
                self.boot_seconds = time.monotonic() - started
                return fresh
            if time.monotonic() - started >= self.timeout:
                raise BoardFailure(
                    f"the board did not reach its shell in {self.timeout:.0f} s"
                )
            if time.monotonic() - self.last_output >= self.timeout:
                raise BoardFailure("the board wrote nothing at the boot")

    def run(self, command: str) -> str:
        """Type one line and return the answer up to the next prompt."""
        assert self.serial is not None
        before = self.transcript.count(PROMPT)
        start = len(self.transcript)
        self.serial.write((command + "\n").encode("ascii", errors="replace"))
        self.serial.flush()
        started = time.monotonic()

        while True:
            self._read()
            fresh = self.transcript[start:]
            marker = self._fatal(fresh)
            if marker is not None:
                raise BoardFailure(f"'{marker}' in the answer of {command!r}")
            if self.transcript.count(PROMPT) > before:
                return fresh
            if time.monotonic() - started >= self.timeout:
                raise BoardFailure(f"{command!r} did not answer in {self.timeout:.0f} s")
            if time.monotonic() - self.last_output >= self.hang:
                raise BoardFailure(f"the console was silent for {self.hang:.0f} s")


class Load:
    def __init__(self, rng: random.Random) -> None:
        self.rng = rng
        self.cycle = 0

    def cycle_commands(self) -> list:
        n = self.cycle
        self.cycle += 1
        tag = f"s{n}-{self.rng.randrange(1000):03d}"

        out = [
            Command("uname", ["MicroOS"]),
            Command("uptime", [re.compile(r"up \d+ s")]),
            Command("free", ["heap:"]),
            Command("df", ["blocks:", "free"]),
            Command("modules", ["0 problem(s)"]),
            Command("mfetch", ["OS", "Board"]),
            Command("display rotate 90", ["display rotation: 90"]),
            Command("display rotate 0", ["display rotation: 0"]),
            Command("exec /bin/hello", ["PROGRAM: hello from /bin"]),
            Command("exec /bin/hello one two", ["PROGRAM: argv[1] one"]),
            Command(f"write /tmp/soak {tag}", ["bytes written"]),
            Command("cat /tmp/soak", [tag]),
            Command("echo more >> /tmp/soak", forbid=("cannot", "No space")),
            Command("cat /tmp/soak", [tag, "more"]),
            Command("exec /bin/cat /tmp/soak", [tag]),
            Command("mkdir /tmp/d", forbid=("mkdir:",)),
            Command("write /tmp/d/b body", ["bytes written"]),
            Command("rm -r /tmp/d", forbid=("rm:",)),
            Command("exec /log/.foreign", ["exec format error"]),
            Command("fsck", ["consistent"]),
            Command("telnet on", ["telnet: on"], soft=True),
            Command("telnet off", ["telnet: off"], soft=True),
            Command("wifi status", ["wifi"], soft=True),
            Command("date", [re.compile(r"\d{4}-\d{2}-\d{2}")]),
            # The systems of /etc (ADR 0021): the file is written and applied
            # at once, and given back to the defaults so the next command sees
            # the picture of the release. A board without a panel of the
            # version that has the settings keeps its own files and warns, so
            # the check is soft.
            Command("config get display.rotation", ["display.rotation="], soft=True),
            Command("config set display.rotation 90", ["display.rotation=90"], soft=True),
            Command("config set font.scale 2", ["font.scale=2"], soft=True),
            Command("config set display.rotation 0", ["display.rotation=0"], soft=True),
            Command("config set font.scale 1", ["font.scale=1"], soft=True),
            Command("sleep 50", ["slept 50 ms"]),
            Command("kill 999999999", ["no such process"]),
            Command("abi", ["call(s) registered"]),
            Command("ps", [re.compile(r"pid\s+prio\s+state"), "shell"]),
            Command("ulimit", [FD_ROW]),
        ]

        if n % 5 == 0:
            out.append(Command("fsck -y", ["consistent"]))
            out.append(Command("config sync", ["saved"], soft=True))

        return out


class Session:
    def __init__(self, args, board: Board, load: Load) -> None:
        self.args = args
        self.board = board
        self.load = load
        self.errors: list = []
        self.warnings: list = []
        self.commands = 0
        self.cycles = 0
        self.baseline: dict = {}
        self.final: dict = {}

    def fail(self, command: str, reason: str, answer: str = "") -> None:
        self.errors.append(
            {"command": command, "reason": reason, "answer": answer[-240:].replace("\r", "")}
        )

    def warn(self, command: str, reason: str) -> None:
        self.warnings.append({"command": command, "reason": reason})

    def execute(self, command: Command) -> str:
        answer = self.board.run(command.text)
        self.commands += 1

        for expectation in command.expect:
            if not matches(answer, expectation):
                message = f"no {expectation!r} in the answer"
                if command.soft:
                    self.warn(command.text, message)
                else:
                    self.fail(command.text, message, answer)
                break

        for forbidden in command.forbid:
            if matches(answer, forbidden):
                self.fail(command.text, f"the answer holds {forbidden!r}", answer)
                break

        return answer

    def sample(self) -> dict:
        sample = {}
        for text, expect, parse in (
            ("free", ["heap:"], self._parse_free),
            ("ulimit", [FD_ROW], self._parse_ulimit),
            ("ps", [re.compile(r"pid\s+prio\s+state")], self._parse_ps),
            ("df", ["blocks:"], self._parse_df),
        ):
            sample.update(parse(self.execute(Command(text, expect))))
        return sample

    @staticmethod
    def _parse_free(answer: str) -> dict:
        match = HEAP_FREE.search(answer)
        return {"heap_free_kb": int(match.group(1))} if match else {}

    @staticmethod
    def _parse_ulimit(answer: str) -> dict:
        match = FD_ROW.search(answer)
        return {"fds": int(match.group(1))} if match else {}

    @staticmethod
    def _parse_ps(answer: str) -> dict:
        rows = PS_ROW.findall(answer)
        return {"tasks": len(rows)}

    @staticmethod
    def _parse_df(answer: str) -> dict:
        match = DF_FREE.search(answer)
        return {"free_blocks": int(match.group(1))} if match else {}

    def run(self) -> dict:
        # The board image has no /tmp: the file part of the load needs one
        # directory, and a session that found it already there (a volume of
        # the boot before) is not a failure.
        self.execute(Command("mkdir /tmp", soft=True))

        self.baseline = self.sample()
        started = time.monotonic()
        deadline = started + self.args.seconds

        while time.monotonic() < deadline:
            for command in self.load.cycle_commands():
                if time.monotonic() >= deadline:
                    break
                self.execute(command)
            self.cycles += 1

        self.final = self.sample()
        elapsed = time.monotonic() - started

        baseline, final = self.baseline, self.final

        if "heap_free_kb" in baseline and "heap_free_kb" in final:
            drift = baseline["heap_free_kb"] - final["heap_free_kb"]
            if drift > self.args.mem_drift_kb:
                self.fail("<memory>",
                          f"the heap lost {drift} KB (the limit is {self.args.mem_drift_kb} KB)")

        if "free_blocks" in baseline and "free_blocks" in final:
            drift = baseline["free_blocks"] - final["free_blocks"]
            if drift > self.args.blocks_drift:
                self.fail(
                    "<volume>",
                    f"the volume lost {drift} block(s) "
                    f"(the limit is {self.args.blocks_drift})",
                )

        if "fds" in baseline and "fds" in final and baseline["fds"] != final["fds"]:
            self.fail(
                "<descriptors>",
                f"the descriptor limit moved: {baseline['fds']} -> {final['fds']}",
            )

        if "tasks" in baseline and "tasks" in final and baseline["tasks"] != final["tasks"]:
            self.fail("<tasks>", f"the task table moved: {baseline['tasks']} -> {final['tasks']}")

        return {
            "seconds": elapsed,
            "boot_seconds": self.board.boot_seconds,
            "commands": self.commands,
            "cycles": self.cycles,
            "errors": self.errors,
            "warnings": self.warnings,
            "baseline": baseline,
            "final": final,
        }


def save_log(args, transcript: str, failed: bool) -> str | None:
    if not failed and not args.keep_log:
        return None

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(args.log_dir, f"board-soak-{stamp}.log")
    os.makedirs(args.log_dir, exist_ok=True)
    with open(path, "w", encoding="utf-8", errors="replace") as handle:
        handle.write(transcript)
    return path


def report(args, result: dict | None, errors: list, warnings: list, path: str | None) -> int:
    print("Micro OS — the board soak")
    print("=" * 68)
    good = not errors

    if result is not None:
        baseline, final = result["baseline"], result["final"]
        print(f"duration      {result['seconds']:.1f} s")
        print(f"boot          {result['boot_seconds']:.2f} s")
        print(f"cycles        {result['cycles']}")
        print(f"commands      {result['commands']}")
        print(
            "memory        heap free "
            f"{baseline.get('heap_free_kb', '?')} KB -> {final.get('heap_free_kb', '?')} KB"
        )
        print(
            "volume        free blocks "
            f"{baseline.get('free_blocks', '?')} -> {final.get('free_blocks', '?')}"
        )
        print(f"tasks         {baseline.get('tasks', '?')} -> {final.get('tasks', '?')}")
        print(f"descriptors   fds {baseline.get('fds', '?')} -> {final.get('fds', '?')}")

    print(f"errors        {len(errors)}")
    print(f"warnings      {len(warnings)}")

    for warning in warnings[:8]:
        print(f"    hint: {warning['command']}: {warning['reason']}")
    for error in errors[:10]:
        print(f"    FAIL: {error['command']}: {error['reason']}")
        if error["answer"]:
            lines = [
                line
                for line in error["answer"].splitlines()
                if line.strip() and "microos>" not in line
            ]
            if lines:
                print(f"          {lines[-1][:120]}")

    if path:
        print(f"log           {path}")

    print("=" * 68)
    if good:
        print(f"RESULT: PASS {int(args.seconds)}s")
        return 0

    print("RESULT: FAIL")
    return 1


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the board half of the session of 600 seconds")
    parser.add_argument("--port", default=None)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--minutes", type=float, default=DEFAULT_MINUTES)
    parser.add_argument("--seconds", type=float, default=None)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--hang-seconds", type=float, default=30.0)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--mem-drift-kb", type=int, default=8)
    parser.add_argument("--blocks-drift", type=int, default=8)
    parser.add_argument("--keep-log", action="store_true")
    parser.add_argument("--log-dir", default=os.path.join(ROOT, "build-host", "soak-logs"))
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.seconds is None:
        args.seconds = args.minutes * 60.0

    if args.port is None:
        print("board-soak: --port is required", file=sys.stderr)
        return 2

    board = Board(args.port, args.baud, args.hang_seconds, args.timeout)
    load = Load(random.Random(args.seed))
    session = Session(args, board, load)
    result = None
    errors: list = []
    warnings: list = []
    fatal: str | None = None

    try:
        board.open()
        if not args.no_reset:
            board.reset()
        boot = board.boot()
        if args.verbose:
            print(boot)
        for expectation in BOOT_EXPECT:
            if expectation not in boot:
                session.fail("<boot>", f"no {expectation!r} in the boot", boot)

        if not session.errors:
            result = session.run()
            errors = result["errors"]
            warnings = result["warnings"]
    except BoardFailure as error:
        fatal = str(error)
        errors = [{"command": "<board>", "reason": fatal, "answer": ""}]
        warnings = []
    finally:
        transcript = board.transcript
        board.close()

    path = save_log(args, transcript, bool(errors))
    status = report(args, result, errors, warnings, path)

    if fatal is not None and args.verbose:
        print(transcript)

    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
