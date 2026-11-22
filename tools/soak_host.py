#!/usr/bin/env python3
"""Micro OS — the host half of the session of 600 seconds.

The x86-64 soak is one machine; this is the other half of the same ten
minutes: the portable kernel over the host port, several tasks at once,
under the thread sanitizer. It runs while the machine of `make soak` runs,
and it has the same verdict: a warning of TSan, a deadlock (a stress that
stops printing), a leaked block of the allocator or a valgrind report of a
definite leak is a failure.

    tools/soak_host.py --tsan build-tsan/tools/microos_stress
                       --plain build-host/tools/microos_stress
                       [--minutes N | --seconds N] [--threads N]
                       [--skip-valgrind] [--strict-valgrind]

The default is 600 seconds. `--minutes 1` is the smoke test of the tool
itself. The process prints a progress line every two seconds; a silence
longer than `--hang-seconds` is the deadlock the gate is about, and the
watchdog here kills it and says so instead of waiting for the timeout.

The exit status is 0 for PASS, 1 for FAIL and 2 when a binary or valgrind
is missing.
"""

from __future__ import annotations

import argparse
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SECONDS = 600.0
TSAN_WARNING = "WARNING: ThreadSanitizer"
TSAN_SUMMARY = re.compile(r"ThreadSanitizer: reported (\d+) warning")
VALGRIND_LOST = re.compile(r"definitely lost:\s*([\d,]+) bytes")


def child_env(cpus: int) -> dict:
    """The environment of a stress process: the kernel's number of cores.

    The host port reads MICRO_OS_CPUS once, when the kernel is initialised
    (ADR 0012), so the two halves of the session run the same two-core
    scheduler; a build without KERNEL_SMP ignores the variable.
    """
    env = os.environ.copy()
    env["MICRO_OS_CPUS"] = str(cpus)
    return env


class Run:
    """One stress process and the lines it writes."""

    def __init__(self, command: list, seconds: float, hang: float, cpus: int) -> None:
        self.command = command
        self.seconds = seconds
        self.hang = hang
        self.cpus = cpus
        self.lines: list = []
        self.last_output = time.monotonic()
        self.lock = threading.Lock()
        self.process: subprocess.Popen | None = None
        self.watchdog: threading.Thread | None = None
        self.timed_out = False
        self.deadlock = False

    def start(self) -> None:
        self.process = subprocess.Popen(
            self.command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert self.process.stdout is not None
        self.last_output = time.monotonic()
        thread = threading.Thread(target=self._read, daemon=True)
        thread.start()

    def _read(self) -> None:
        assert self.process is not None
        assert self.process.stdout is not None
        for line in self.process.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))
            self.last_output = time.monotonic()

    def _watch(self, deadline: float) -> None:
        while time.monotonic() < deadline:
            if self.process is None or self.process.poll() is not None:
                return
            if time.monotonic() - self.last_output > self.hang:
                self.deadlock = True
                self.kill()
                return
            time.sleep(0.2)

        if self.process is not None and self.process.poll() is None:
            self.timed_out = True
            self.kill()

    def kill(self) -> None:
        process = self.process
        if process is None:
            return
        try:
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            try:
                process.kill()
            except ProcessLookupError:
                pass
            process.wait()

    def wait(self, timeout: float) -> int:
        assert self.process is not None
        deadline = time.monotonic() + timeout
        self.watchdog = threading.Thread(target=self._watch, args=(deadline,), daemon=True)
        self.watchdog.start()
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.timed_out = True
            self.kill()
            return 124

    def text(self) -> str:
        with self.lock:
            return "\n".join(self.lines)


def save_log(directory: str, name: str, text: str) -> str:
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"{name}.log")
    with open(path, "w", encoding="utf-8", errors="replace") as handle:
        handle.write(text)
    return path


def race_locations(text: str) -> list:
    """The globals and the sources TSan named, the first of each in order."""
    found: list = []
    for match in re.finditer(r"Location is global '([^']+)'", text):
        name = match.group(1)
        if name not in found:
            found.append(name)
    if not found:
        for match in re.finditer(r"^    #0 (\S+ \S+)", text, re.MULTILINE):
            if match.group(1) not in found:
                found.append(match.group(1))
    return found[:8]


def run_tsan(args, seconds: float) -> dict:
    if not os.path.isfile(args.tsan):
        print(f"soak-host: missing {args.tsan}", file=sys.stderr)
        return {"ok": False, "missing": True}

    command = [
        args.tsan,
        "--seconds",
        str(int(seconds)),
        "--workers",
        str(args.threads),
        "--seed",
        str(args.seed),
    ]
    run = Run(command, seconds, args.hang_seconds, args.cpus)
    print(
        f"soak-host: TSan, {seconds:.0f} s, {args.threads} task(s), {args.cpus} core(s)",
        flush=True,
    )
    started = time.monotonic()
    run.start()
    status = run.wait(seconds + args.hang_seconds + 30.0)
    elapsed = time.monotonic() - started
    text = run.text()

    warnings = text.count(TSAN_WARNING)
    summary = TSAN_SUMMARY.search(text)

    if summary is not None:
        warnings = max(warnings, int(summary.group(1)))

    ok = True
    detail = []

    if run.deadlock:
        ok = False
        detail.append("the stress stopped printing: a deadlock")
    elif run.timed_out:
        ok = False
        detail.append(f"the stress did not end in {seconds + args.hang_seconds + 30:.0f} s")

    if status != 0 and not run.deadlock and not run.timed_out:
        ok = False
        detail.append(f"the stress exited with {status}")

    if elapsed < seconds * 0.8 and not (run.deadlock or run.timed_out):
        ok = False
        detail.append(f"the stress ended early, after {elapsed:.1f} s")

    if warnings:
        ok = False
        detail.append(f"{warnings} TSan warning(s)")

    path = None
    if not ok or args.keep_logs:
        path = save_log(
            args.log_dir,
            f"soak-host-{datetime.datetime.now():%Y%m%d-%H%M%S}",
            text,
        )

    return {
        "ok": ok,
        "missing": False,
        "status": status,
        "elapsed": elapsed,
        "warnings": warnings,
        "locations": race_locations(text),
        "detail": detail,
        "path": path,
        "text": text,
    }


def run_valgrind(args) -> dict:
    if args.skip_valgrind:
        return {"ok": True, "skip": "asked for"}
    if shutil.which("valgrind") is None:
        return {"ok": True, "skip": "valgrind is not installed"}
    if not os.path.isfile(args.plain):
        print(f"soak-host: missing {args.plain}", file=sys.stderr)
        return {"ok": False, "missing": True}

    seconds = min(5.0, max(2.0, args.valgrind_seconds))
    command = [
        "valgrind",
        "--error-exitcode=9",
        "--leak-check=full",
        "--show-leak-kinds=definite",
        "--errors-for-leak-kinds=definite",
        "--quiet",
        args.plain,
        "--seconds",
        str(int(seconds)),
        "--workers",
        "4",
        "--seed",
        str(args.seed),
    ]
    print(f"soak-host: valgrind, {seconds:.0f} s", flush=True)

    process = subprocess.run(
        command,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=seconds + 180.0,
        check=False,
        env=child_env(args.cpus),
    )
    output = process.stdout + process.stderr
    lost = VALGRIND_LOST.search(output)
    ok = process.returncode != 9 and not (lost and int(lost.group(1).replace(",", "")) > 0)

    path = None
    if not ok:
        path = save_log(
            args.log_dir,
            f"soak-host-valgrind-{datetime.datetime.now():%Y%m%d-%H%M%S}",
            output,
        )

    return {
        "ok": ok,
        "skip": None,
        "status": process.returncode,
        "lost": lost.group(1) if lost else "0",
        "path": path,
        "text": output,
    }


def report(tsan: dict, valgrind: dict, args) -> int:
    print("Micro OS — the host soak")
    print("=" * 68)
    good = True

    if tsan.get("missing"):
        print("TSan        missing binary")
        return 2

    print(
        f"TSan        {tsan['elapsed']:.1f} s, {args.threads} task(s), {args.cpus} core(s)"
    )
    print(f"  warnings  {tsan['warnings']}")
    for location in tsan["locations"]:
        print(f"  race      {location}")
    for detail in tsan["detail"]:
        print(f"  FAIL      {detail}")
    if tsan["path"]:
        print(f"  log       {tsan['path']}")
    if not tsan["ok"]:
        good = False

    if valgrind.get("missing"):
        print("valgrind    missing binary")
        return 2

    if valgrind.get("skip"):
        print(f"valgrind    skip ({valgrind['skip']})")
    else:
        print(f"valgrind    definite lost: {valgrind['lost']} byte(s)")
        if not valgrind["ok"]:
            print(f"  FAIL      valgrind exit {valgrind['status']}")
            if valgrind["path"]:
                print(f"  log       {valgrind['path']}")
            good = False

    print("=" * 68)
    if good:
        print(f"RESULT: PASS {int(args.seconds)}s")
    else:
        print("RESULT: FAIL")

    return 0 if good else 1


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the host half of the session of 600 seconds")
    parser.add_argument(
        "--tsan", default=os.path.join(ROOT, "build-tsan", "tools", "microos_stress")
    )
    parser.add_argument(
        "--plain", default=os.path.join(ROOT, "build-host", "tools", "microos_stress")
    )
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    parser.add_argument("--minutes", type=float, default=None)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument(
        "--cpus",
        type=int,
        default=2,
        help="the cores the kernel is told about (MICRO_OS_CPUS, ADR 0012)",
    )
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--hang-seconds", type=float, default=20.0)
    parser.add_argument("--valgrind-seconds", type=float, default=5.0)
    parser.add_argument("--skip-valgrind", action="store_true")
    parser.add_argument(
        "--strict-valgrind",
        action="store_true",
        help="fail when valgrind is not installed (the default is a skip)",
    )
    parser.add_argument("--keep-logs", action="store_true")
    parser.add_argument("--log-dir", default=os.path.join(ROOT, "build-host", "soak-logs"))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    if args.minutes is not None:
        args.seconds = args.minutes * 60.0

    if args.seconds <= 0:
        print("soak-host: --seconds has to be positive", file=sys.stderr)
        return 2

    tsan = run_tsan(args, args.seconds)
    if tsan.get("missing"):
        return 2

    if args.verbose and tsan["text"]:
        print(tsan["text"])

    valgrind = run_valgrind(args)

    if valgrind.get("skip") and args.strict_valgrind:
        valgrind = {"ok": False, "skip": None, "status": -1, "lost": "?", "path": None,
                    "text": "valgrind is not installed"}

    return report(tsan, valgrind, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
