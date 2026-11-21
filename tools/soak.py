#!/usr/bin/env python3
"""Micro OS — the session of 600 seconds, one boot of the x86-64 port.

The gate of 1.0 (docs/releases/1.0.md, "The session of 600 seconds") is not
a loop of short boots: it is one machine that runs for ten minutes under a
mixed load, and a panic, a hang, a reboot, a failed command or a leak of a
descriptor or of memory is a failure of that one machine. This tool is that
machine.

    tools/soak.py [--build DIR] [--image PATH] [--minutes N | --seconds N]
                  [--iterations N] [--seed S] [--keep-logs] [--verbose]

The default is 600 seconds in one boot. `--minutes` shortens the session for
a smoke test of the tool itself; `--iterations` is the other mode, the one
of the campaign before the higher half: N short boots, each one pass of the
load, with the average boot reported. The two modes are exclusive.

What the load does, in a cycle that repeats until the clock says stop:

  * the shell: `uname`, `uptime`, `free`, `df`, `ulimit`, `ps`, `mount`,
    `tree`, `fsck`, `kill` of a pid nobody has, `sleep`;
  * the programs of /bin through `exec`: hello, cat, echo, ls, wc, grep,
    head, mkdir, rm, pwd, spawn, fork, signals, nx, badptr, crash;
  * pipelines and redirections: `ls /bin | wc -l`, `cat < file | wc -l`,
    `echo x > f`, `echo y >> f`, and a read of the file back;
  * the filesystem: a directory made, a program's `rm -r` of it, files
    written, appended, read and removed; once per boot the volume starts
    full (tools/soak_image.py), a write is refused with ENOSPC, the filler
    is removed, `fsck -y` repairs, and the volume is used again;
  * signals: `exec /bin/signals`, and Ctrl+C typed into the console while
    the program is running;
  * the network, when the image was built with MICRO_OS_X86_NET: ping of the
    gateway, the HTTP client against a page of this process, the `httpd`
    page reached through `hostfwd`, and the resolver when this host has one.
    NTP is skipped unless this process can bind UDP 123 (it needs a
    privilege a campaign on a laptop does not have), and the report says so.

A session that fails leaves its transcript next to the image; `--keep-logs`
keeps the transcript of a passing session as well.

The exit status is 0 for PASS, 1 for FAIL and 2 when a tool or a file is
missing.
"""

from __future__ import annotations

import argparse
import datetime
import http.server
import os
import random
import re
import shutil
import socket
import struct
import sys
import threading
import time

from qemu_session import Machine, MachineClosed, MachineTimeout, MachineError

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "build-x86-net")
DEFAULT_SECONDS = 600.0
PROMPT = "microos> "

# The words that mean the machine died or restarted. "reboot" alone is not
# one of them: the help of the shell lists the command, and a help line is
# not a reboot.
FATAL = [
    "Panic",
    "PANIC",
    "panic:",
    "Kernel panic",
    "Guru Meditation",
    "triple fault",
    "Rebooting",
    "assert failed",
    "Assertion failed",
    "*** ERROR",
]

# The lines of the boot that have to be in the banner. A session that starts
# the load on a machine without its filesystem or its shell would exercise
# nothing.
BOOT_EXPECT = [
    "board: qemu_x86_64",
    "the kernel is up",
    "vfs: '/' is mounted",
    "x86-64: the root volume is ata0p1",
    "MicroOS shell ready",
]

PS_ROW = re.compile(r"^\s*(\d+)\s+\d+\s+\w+\s+(\S+)\s*$", re.MULTILINE)
FD_ROW = re.compile(r"^fds\s+(\d+)\s*$", re.MULTILINE)
MEMORY_USED = re.compile(r"memory\s+\d+ bytes \((\d+) used\)")
HEAP_FREE = re.compile(r"heap:\s*(\d+) KB free")
DF_FREE = re.compile(r"blocks:\s*(\d+) of\s*(\d+) free")
UPTIME = re.compile(r"up\s+(\d+)\s+s")
COUNT = re.compile(r"^\s*(\d+)\s*$", re.MULTILINE)


class SessionFailure(RuntimeError):
    """The machine or the load broke the rules of the session."""


class HostPage:
    """The page the guest fetches with `http get` over the user network."""

    BODY = "the soak page answers"

    def __init__(self) -> None:
        body = (self.BODY + "\n").encode("ascii")
        handler = type(
            "SoakHandler",
            (http.server.BaseHTTPRequestHandler,),
            {
                "do_GET": lambda self: (self.send_response(200),
                                        self.send_header("Content-Type", "text/plain"),
                                        self.send_header("Content-Length", str(len(body))),
                                        self.end_headers(),
                                        self.wfile.write(body)),
                "log_message": lambda self, *args: None,
            },
        )
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        # The guest reaches the host through 10.0.2.2 and not through its own
        # loopback: the address of QEMU user-net is the gateway of the guest.
        self.url = f"http://10.0.2.2:{self.server.server_address[1]}/"

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


class NtpServer:
    """A one-packet NTP server on 127.0.0.1:123, when the port can be bound.

    The kernel asks the address 10.0.2.2, which QEMU user-net maps to the
    loopback of this process, so the NTP client of components/net is
    exercised for real. The port is privileged; a laptop without
    CAP_NET_BIND_SERVICE cannot take it, and the session says so instead of
    calling the missing check a failure.
    """

    def __init__(self) -> None:
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stop = threading.Event()
        self.thread: threading.Thread | None = None

    def start(self) -> bool:
        try:
            self.socket.bind(("127.0.0.1", 123))
        except OSError:
            self.socket.close()
            return False

        self.socket.settimeout(0.5)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        return True

    def _serve(self) -> None:
        while not self.stop.is_set():
            try:
                data, address = self.socket.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                return

            if len(data) < 48:
                continue

            answer = bytearray(48)
            answer[0] = 0x24  # leap 0, version 4, mode 4 (server)
            answer[1] = 1     # stratum 1
            answer[2] = 4     # poll
            seconds = int(time.time()) + 2_208_988_800
            struct.pack_into(">I", answer, 40, seconds)
            struct.pack_into(">I", answer, 44, 0)
            try:
                self.socket.sendto(bytes(answer), address)
            except OSError:
                return

    def close(self) -> None:
        self.stop.set()
        if self.thread is not None:
            self.thread.join(timeout=2)
        self.socket.close()


class Command:
    """One line of the load, and what its answer has to hold."""

    __slots__ = ("text", "expect", "forbid", "soft", "interrupt", "timeout", "host")

    def __init__(
        self,
        text: str,
        expect=(),
        forbid=(),
        *,
        soft: bool = False,
        interrupt: bool = False,
        timeout: float = 20.0,
        host=None,
    ) -> None:
        self.text = text
        self.expect = list(expect)
        self.forbid = list(forbid)
        self.soft = soft
        self.interrupt = interrupt
        self.timeout = timeout
        # A callable of the host side that runs after the command: the page
        # of `httpd` is fetched by this process, not typed into the shell.
        self.host = host


def matches(block: str, expectation) -> bool:
    if isinstance(expectation, re.Pattern):
        return expectation.search(block) is not None
    return expectation in block


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def host_get(port: int, path: str = "/", tries: int = 3, timeout: float = 3.0) -> str:
    """An HTTP request through hostfwd, as a person of the host would make it."""
    for _ in range(tries):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
                client.sendall(f"GET {path} HTTP/1.0\r\nHost: soak\r\n\r\n".encode("ascii"))
                chunks = []
                while True:
                    data = client.recv(4096)
                    if not data:
                        break
                    chunks.append(data)
                answer = b"".join(chunks).decode("utf-8", errors="replace")
                if answer:
                    return answer
        except OSError:
            time.sleep(0.1)
    return ""


class Load:
    """The commands of the campaign, made again for every cycle.

    The names and the words are seeded, so two sessions with the same
    `--seed` send the same lines in the same order. The line of a file is
    the number of the cycle, which makes a wrong answer a fact about this
    machine and not about an old file on the volume.
    """

    def __init__(self, rng: random.Random, *, net: bool, page: str, fwd: int,
                 ntp: bool, dns: bool) -> None:
        self.rng = rng
        self.net = net
        self.page = page
        self.fwd = fwd
        self.ntp = ntp
        self.dns = dns
        self.cycle = 0
        self.httpd_ready = False

    # --- the once-per-boot part ---------------------------------------

    def prelude(self) -> list:
        """The commands of the boot: the full volume first, then the network."""
        out = [Command("df", ["blocks:", "free"])]

        # /tmp is a directory of the image (tools/soak_image.py): a volume
        # with no free block cannot make one, and the load needs it before
        # the filler leaves.
        # The volume of tools/soak_image.py has no free block: one write has
        # to be refused, then the filler leaves and the repair has to say the
        # volume is consistent. This is the ENOSPC half of the gate.
        out += [
            Command("write /tmp/full hello", ["no space left on device"]),
            Command("df", [re.compile(r"blocks:\s*0\s+of")]),
            Command("rm /filler", forbid=("rm:",)),
            Command("df", [re.compile(r"blocks:\s*[1-9]\d*\s+of")]),
            Command("fsck -y", ["the volume is consistent"]),
            Command("rm -r /tmp", forbid=("rm:",)),
            Command("mkdir /tmp", forbid=("cannot create",)),
            Command("write /tmp/ready hello", ["byte(s) written"]),
            Command("cat /tmp/ready", ["hello"]),
            Command("rm /tmp/ready", forbid=("rm:",)),
            Command("fsck", ["the volume is consistent"]),
        ]

        if self.net:
            out += [
                Command("netinfo", ["virtio0", "net: 10.0.2.15/24"]),
                Command("ping 10.0.2.2", ["bytes from 10.0.2.2"], timeout=10.0),
                Command(
                    f"http get {self.page}",
                    ["HTTP 200", HostPage.BODY],
                    timeout=15.0,
                ),
                Command("httpd 8080", ["the page is on port 8080"]),
            ]
            self.httpd_ready = True
            out.append(
                Command(
                    "uname",
                    ["MicroOS"],
                    host=lambda: self._page_check(),
                )
            )

            if self.dns:
                out.append(
                    Command("dns example.com",
                            [re.compile(r"dns: \S+ is \d+\.\d+\.\d+\.\d+")], soft=True)
                )
                out.append(Command("dns example.com", ["from the cache"], soft=True))

            if self.ntp:
                out.append(Command("ntp 10.0.2.2", ["ntp: 20"], soft=True))

        return out

    # --- the cycle -----------------------------------------------------

    def cycle_commands(self) -> list:
        n = self.cycle
        self.cycle += 1
        tag = f"c{n}-{self.rng.randrange(1000):03d}"
        line = f"line-{n}"
        more = f"more-{n}"
        file = "/tmp/soak.txt"
        directory = "/tmp/soak.d"

        out = [
            Command("uname", ["MicroOS"]),
            Command("uptime", [re.compile(r"up \d+ s")]),
            Command("free", ["heap:"]),
            Command("df", ["blocks:", "free"]),
            Command("exec /bin/hello",
                    ["PROGRAM: hello from /bin", "the program exited with 0"]),
            Command(f"exec /bin/echo {tag}", [tag, "the program exited with 0"]),
            Command("exec /bin/cat /etc/setting", ["hello", "the program exited with 0"]),
            Command("exec /bin/ls /bin", ["hello", "the program exited with 0"]),
            Command("exec /bin/wc -l /etc/setting",
                    ["3 /etc/setting", "the program exited with 0"]),
            Command("exec /bin/grep -i HELLO /etc/setting",
                    ["hello", "the program exited with 0"]),
            Command("exec /bin/head -n 1 /etc/setting",
                    ["hello", "the program exited with 0"]),
            Command("exec /bin/pwd", [re.compile(r"(?m)^/\s*$"), "the program exited with 0"]),
            Command("exec /bin/spawn",
                    ["PROGRAM: envp 2, PATH=/custom", "the program exited with 0"]),
            # The tree through the calls: a directory, a listing, and the
            # `-r` the shell program needs to take it down.
            Command("exec /bin/mkdir " + directory, ["the program exited with 0"]),
            Command("exec /bin/ls /tmp", ["soak.d", "the program exited with 0"]),
            Command("exec /bin/rm -r " + directory, ["the program exited with 0"]),
            # A file made by a redirect, appended, read back through a pipe
            # and removed by the program of the library.
            Command(f"echo {line} > {file}", forbid=("cannot",)),
            Command(f"cat {file}", [line]),
            Command(f"echo {more} >> {file}", forbid=("cannot",)),
            Command(f"cat < {file} | wc -l", [re.compile(r"(?m)^\s*2\s*$")]),
            Command("cat < /etc/setting | wc -l", [re.compile(r"(?m)^\s*3\s*$")]),
            Command("ls /bin | wc -l", [re.compile(r"(?m)^\s*[1-9]\d*\s*$")]),
            Command(f"exec /bin/rm {file}", ["the program exited with 0"]),
            Command("exec /bin/fork",
                    ["PROGRAM: parent sees value 42", "PROGRAM: waited, status 7",
                     "the program exited with 0"]),
            Command("exec /bin/signals",
                    ["PROGRAM: signals done", "the program exited with 0"]),
            # Ctrl+C while the program runs: the port hands it to the
            # foreground task. Its answer is not compared (the program has
            # its own SIGINT story), only the prompt afterwards.
            Command("exec /bin/signals", interrupt=True, soft=True),
            Command("kill 9999", ["no such process"]),
            Command("sleep 50", ["slept 50 ms"], forbid=("usage", "not a number")),
            Command("write /tmp/soak.one hello", ["byte(s) written"]),
            Command("cat /tmp/soak.one", ["hello"]),
            Command("chmod 0644 /tmp/soak.one", forbid=("chmod:",)),
            Command("chown 0 /tmp/soak.one", forbid=("chown:",)),
            Command("rm /tmp/soak.one", forbid=("rm:",)),
            Command("exec /bin/hello one two",
                    ["PROGRAM: argc 3, argv[0] /bin/hello", "the program exited with 0"]),
        ]

        if n % 2 == 0:
            out += [
                Command("exec /bin/badptr",
                        ["PROGRAM: write far 14", "the program exited with 0"]),
                Command("exec /bin/nx",
                        ["PROGRAM: the three faults left the machine alive",
                         "the program exited with 0"], timeout=30.0),
                Command("exec /bin/crash",
                        ["the program faulted: vector 14", "the program exited with 255"]),
            ]

        if n % 5 == 0:
            out.append(Command("ps", [re.compile(r"pid\s+prio\s+state"), "shell"]))
            out.append(Command("ulimit", [FD_ROW]))

        if n % 10 == 0:
            out.append(Command("fsck", ["the volume is consistent"]))
            out.append(Command("mount", ["/proc", "/dev"]))
            out.append(Command("tree /", ["|--", "`--"]))
            out.append(Command("ls -la /bin", ["hello", "rwxr-xr-x"]))
            out.append(Command("cat /proc/meminfo", ["kernel heap:", "filesystem:"]))

        if self.net:
            if n % 3 == 0:
                out.append(Command("ping 10.0.2.2", ["bytes from 10.0.2.2"], timeout=10.0))
                out.append(Command(f"http get {self.page}", ["HTTP 200", HostPage.BODY],
                                   timeout=15.0))
            if n % 5 == 0 and self.httpd_ready:
                out.append(Command("uname", ["MicroOS"], host=lambda: self._page_check()))

        return out

    def _page_check(self) -> tuple:
        page = host_get(self.fwd, "/", tries=10, timeout=2.0)
        if "HTTP/1.0 200 OK" not in page:
            return False, "the page of the guest did not answer through hostfwd"
        for word in ("MicroOS", "uptime:", "memory:"):
            if word not in page:
                return False, f"the page of the guest has no '{word}'"
        return True, ""


class Session:
    """One boot, one load, one verdict."""

    def __init__(self, args, load: Load, label: str) -> None:
        self.args = args
        self.load = load
        self.label = label
        self.errors: list = []
        self.warnings: list = []
        self.commands = 0
        self.cycles = 0
        self.started = 0.0
        self.ended = 0.0
        self.boot_seconds = 0.0
        self.baseline = {}
        self.final = {}

    # --- the checks of one answer --------------------------------------

    def fatal(self, text: str) -> str | None:
        for marker in FATAL:
            if marker in text:
                return marker
        return None

    def fail(self, command: str, reason: str, block: str = "") -> None:
        self.errors.append(
            {
                "command": command,
                "reason": reason,
                "answer": block[-300:].replace("\r", "").strip(),
            }
        )

    def warn(self, command: str, reason: str) -> None:
        self.warnings.append({"command": command, "reason": reason})

    def execute(self, machine: Machine, command: Command) -> str:
        try:
            if command.interrupt:
                machine.send_line(command.text)
                if machine.wait_for("PROGRAM: signals starts", timeout=2.0):
                    machine.send_raw(b"\x03")
                block = machine.wait_prompt(command.timeout, self.args.hang_seconds)
            else:
                machine.send_line(command.text)
                block = machine.wait_prompt(command.timeout, self.args.hang_seconds)
        except (MachineClosed, MachineTimeout) as error:
            raise SessionFailure(str(error)) from error

        self.commands += 1
        marker = self.fatal(block)
        if marker is not None:
            raise SessionFailure(f"'{marker}' in the answer of {command.text!r}")

        for expectation in command.expect:
            if not matches(block, expectation):
                message = f"no {expectation!r} in the answer"
                if command.soft:
                    self.warn(command.text, message)
                else:
                    self.fail(command.text, message, block)
                break

        for forbidden in command.forbid:
            if matches(block, forbidden):
                self.fail(command.text, f"the answer holds {forbidden!r}", block)
                break

        if command.host is not None:
            ok, detail = command.host()
            if not ok:
                self.fail(command.text, detail, block)

        return block

    # --- the session ----------------------------------------------------

    def run(self, machine: Machine) -> dict:
        self.started = time.monotonic()
        banner = machine.open(self.args.boot_timeout)
        self.boot_seconds = machine.boot_seconds

        marker = self.fatal(banner)
        if marker is not None:
            raise SessionFailure(f"'{marker}' in the boot")

        for expectation in BOOT_EXPECT:
            if not matches(banner, expectation):
                self.fail("<boot>", f"no {expectation!r} in the banner", banner)

        # The samples before the load: the memory, the descriptors, the
        # tasks and the volume the session has to leave as it found them.
        self.baseline = self.sample(machine)

        for command in self.load.prelude():
            self.execute(machine, command)

        self.baseline = self.sample(machine, merge=True)
        single_pass = self.args.iterations is not None

        while True:
            if not self._deadline_left():
                break
            for command in self.load.cycle_commands():
                if not self._deadline_left():
                    break
                self.execute(machine, command)
            self.cycles += 1

            if single_pass:
                break

            # A session that has stopped answering is not a session that is
            # merely slow: the prompt of the last command was the last byte.
            if not machine.alive():
                raise SessionClosed("QEMU exited during the load")

        self.final = self.sample(machine)
        self.ended = time.monotonic()
        return self.verdict()

    def sample(self, machine: Machine, merge: bool = False) -> dict:
        """Read free, ulimit, ps and df as four ordinary commands."""
        sample = dict(self.baseline) if merge else {}
        for text, expect, parse in (
            ("free", ["heap:"], self._parse_free),
            ("ulimit", [FD_ROW], self._parse_ulimit),
            ("ps", [re.compile(r"pid\s+prio\s+state")], self._parse_ps),
            ("df", ["blocks:"], self._parse_df),
        ):
            block = self.execute(machine, Command(text, expect))
            sample.update(parse(block))
        return sample

    @staticmethod
    def _parse_free(block: str) -> dict:
        match = HEAP_FREE.search(block)
        return {"heap_free_kb": int(match.group(1))} if match else {}

    @staticmethod
    def _parse_ulimit(block: str) -> dict:
        fds = FD_ROW.search(block)
        used = MEMORY_USED.search(block)
        out = {}
        if fds:
            out["fds"] = int(fds.group(1))
        if used:
            out["memory_used"] = int(used.group(1))
        return out

    @staticmethod
    def _parse_ps(block: str) -> dict:
        rows = PS_ROW.findall(block)
        return {"tasks": len(rows), "task_names": [name for _, name in rows]}

    @staticmethod
    def _parse_df(block: str) -> dict:
        match = DF_FREE.search(block)
        return {"free_blocks": int(match.group(1))} if match else {}

    def _deadline_left(self) -> bool:
        return time.monotonic() < self.started + self.args.seconds

    # --- the verdict ----------------------------------------------------

    def verdict(self) -> dict:
        baseline, final = self.baseline, self.final

        if "heap_free_kb" in baseline and "heap_free_kb" in final:
            drift = baseline["heap_free_kb"] - final["heap_free_kb"]
            if drift > self.args.mem_drift_kb:
                self.fail(
                    "<memory>",
                    f"the heap lost {drift} KB (the limit is {self.args.mem_drift_kb} KB)",
                )

        if baseline.get("memory_used") not in (None, 0) or final.get("memory_used") not in (None, 0):
            self.fail(
                "<memory>",
                "the shell holds memory at rest: "
                f"{baseline.get('memory_used')} -> {final.get('memory_used')} byte(s)",
            )

        if "fds" in baseline and "fds" in final and baseline["fds"] != final["fds"]:
            self.fail(
                "<descriptors>",
                f"the descriptor limit moved: {baseline['fds']} -> {final['fds']}",
            )

        if "free_blocks" in baseline and "free_blocks" in final:
            drift = baseline["free_blocks"] - final["free_blocks"]
            if drift > self.args.blocks_drift:
                self.fail(
                    "<volume>",
                    f"the volume lost {drift} block(s) (the limit is {self.args.blocks_drift})",
                )

        if "tasks" in baseline and "tasks" in final and baseline["tasks"] != final["tasks"]:
            self.fail(
                "<tasks>",
                f"the task table grew: {baseline['tasks']} -> {final['tasks']} "
                f"({baseline.get('task_names')} -> {final.get('task_names')})",
            )

        return {
            "label": self.label,
            "seconds": round(self.ended - self.started, 1),
            "boot_seconds": round(self.boot_seconds, 2),
            "commands": self.commands,
            "cycles": self.cycles,
            "errors": self.errors,
            "warnings": self.warnings,
            "baseline": baseline,
            "final": final,
        }


class SessionClosed(SessionFailure):
    """QEMU went away before the clock of the session ran out."""


def report(results: list, args) -> int:
    good = True
    print("Micro OS — the soak")
    print("=" * 68)

    for result in results:
        baseline, final = result["baseline"], result["final"]
        print(f"{result['label']}:")
        print(f"  duration      {result['seconds']:.1f} s")
        print(f"  boot          {result['boot_seconds']:.2f} s")
        print(f"  cycles        {result['cycles']}")
        print(f"  commands      {result['commands']}")
        print(
            "  memory        heap free "
            f"{baseline.get('heap_free_kb', '?')} KB -> {final.get('heap_free_kb', '?')} KB"
        )
        print(
            "  descriptors   fds "
            f"{baseline.get('fds', '?')} -> {final.get('fds', '?')}, "
            f"shell memory {baseline.get('memory_used', '?')} -> "
            f"{final.get('memory_used', '?')} byte(s)"
        )
        print(
            "  tasks         "
            f"{baseline.get('tasks', '?')} -> {final.get('tasks', '?')}"
        )
        print(
            "  volume        free blocks "
            f"{baseline.get('free_blocks', '?')} -> {final.get('free_blocks', '?')}"
        )
        print(f"  errors        {len(result['errors'])}")
        print(f"  warnings      {len(result['warnings'])}")

        for warning in result["warnings"][:6]:
            print(f"    hint: {warning['command']}: {warning['reason']}")
        for error in result["errors"][:10]:
            print(f"    FAIL: {error['command']}: {error['reason']}")
            if error["answer"]:
                first = error["answer"].splitlines()[-1] if error["answer"].splitlines() else ""
                print(f"          {first[:120]}")

        if result["errors"]:
            good = False

    print("=" * 68)
    planned = int(args.seconds)

    if args.iterations is not None:
        average = sum(item["boot_seconds"] for item in results) / max(1, len(results))
        print(f"average boot  {average:.2f} s")
        if good:
            print(f"RESULT: PASS ({len(results)} short iteration(s))")
        else:
            print(f"RESULT: FAIL ({sum(len(item['errors']) for item in results)} error(s))")
    elif good:
        print(f"RESULT: PASS {planned}s")
    else:
        print(f"RESULT: FAIL ({sum(len(item['errors']) for item in results)} error(s))")

    return 0 if good else 1


def log_transcript(args, records: list) -> str | None:
    """Write the transcript of the last session next to the image."""
    if not records:
        return None

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    directory = args.log_dir or os.path.join(args.build, "soak-logs")
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, f"soak-{stamp}.log")

    with open(path, "w", encoding="utf-8", errors="replace") as handle:
        for label, transcript in records:
            handle.write(f"===== {label} =====\n")
            handle.write(transcript)
            handle.write("\n")

    return path


def make_load(args, seed: int, services: dict) -> Load:
    return Load(
        random.Random(seed),
        net=args.net,
        page=services["page"],
        fwd=services["fwd"],
        ntp=services["ntp"],
        dns=services["dns"],
    )


def prepare_image(args) -> bool:
    """Build the full image when the caller gave none (the usual case)."""
    if args.no_image:
        return True

    command = [
        sys.executable,
        os.path.join(ROOT, "tools", "soak_image.py"),
        "--build",
        args.build,
        "--image",
        args.image,
        "--mkfs",
        args.mkfs,
        "--if-needed",
    ]
    if args.verbose:
        command.append("--verbose")

    import subprocess

    return subprocess.run(command, check=False).returncode == 0


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the session of 600 seconds of the x86-64 port")
    parser.add_argument("--build", default=DEFAULT_BUILD, help="the build of the port")
    parser.add_argument(
        "--mkfs",
        default=os.path.join(ROOT, "build-host", "tools", "microos_mkfs"),
        help="the host tool that writes the image",
    )
    parser.add_argument("--image", default=None, help="the full image of the session")
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS,
                        help="the length of one session (600 by default)")
    parser.add_argument("--minutes", type=float, default=None,
                        help="the length in minutes, for a short run")
    parser.add_argument("--iterations", type=int, default=None,
                        help="short mode: N boots, each one pass of the load")
    parser.add_argument("--seed", type=int, default=1, help="the seed of the load")
    parser.add_argument("--hang-seconds", type=float, default=30.0,
                        help="a long silence of the console is a hang")
    parser.add_argument("--boot-timeout", type=float, default=60.0)
    parser.add_argument("--mem-drift-kb", type=int, default=16,
                        help="the free heap a session may lose (16 KB by default)")
    parser.add_argument("--blocks-drift", type=int, default=4,
                        help="the free blocks a session may lose (4 by default)")
    parser.add_argument("--keep-logs", action="store_true",
                        help="keep the transcript of a passing session too")
    parser.add_argument("--log-dir", default=None)
    parser.add_argument("--no-image", action="store_true",
                        help="use the image as it is (it may not start full)")
    parser.add_argument("--no-net", action="store_true", help="no network even if the build has it")
    parser.add_argument("--verbose", action="store_true")

    args = parser.parse_args(argv)

    if args.image is None:
        args.image = os.path.join(args.build, "soak.img")

    if args.iterations is not None and args.minutes is not None:
        print("soak: --iterations and --minutes are different modes", file=sys.stderr)
        return 2

    if args.minutes is not None:
        args.seconds = args.minutes * 60.0

    if args.iterations is not None and args.iterations < 1:
        print("soak: --iterations has to be at least 1", file=sys.stderr)
        return 2

    for path in (os.path.join(args.build, "loader32.elf"),
                 os.path.join(args.build, "microos64.bin")):
        if not os.path.isfile(path):
            print(f"soak: missing {path}: run make {args.build}", file=sys.stderr)
            return 2

    args.net = not args.no_net and net_enabled(args.build)

    if not prepare_image(args):
        return 2

    if not os.path.isfile(args.image):
        print(f"soak: missing {args.image}", file=sys.stderr)
        return 2

    services = {"page": "", "fwd": None, "ntp": False, "dns": False}
    page = HostPage() if args.net else None
    ntp = NtpServer() if args.net else None
    services["page"] = page.url if page is not None else ""
    services["fwd"] = free_port() if args.net else None
    if ntp is not None:
        services["ntp"] = ntp.start()
    services["dns"] = can_resolve() if args.net else False

    results = []
    records = []
    status = 0

    try:
        rounds = args.iterations if args.iterations is not None else 1

        for index in range(rounds):
            label = (
                f"iteration {index + 1} of {rounds}"
                if args.iterations is not None
                else f"session of {int(args.seconds)} s"
            )
            workdir = os.path.join(args.build, "soak-run")
            os.makedirs(workdir, exist_ok=True)
            image = os.path.join(workdir, f"session-{index + 1}.img")
            shutil.copyfile(args.image, image)

            load = make_load(args, args.seed + index, services)
            session = Session(args, load, label)
            machine = Machine(
                args.build,
                image,
                net=args.net,
                hostfwd=services["fwd"],
            )

            try:
                result = session.run(machine)
                results.append(result)
                if args.verbose:
                    print(f"{label}: {result['cycles']} cycle(s), "
                          f"{len(result['errors'])} error(s)")
            except (SessionFailure, MachineError) as error:
                session.errors.append(
                    {"command": "<machine>", "reason": str(error), "answer": ""}
                )
                results.append(
                    {
                        "label": label,
                        "seconds": round(time.monotonic() - session.started, 1),
                        "boot_seconds": round(session.boot_seconds, 2),
                        "commands": session.commands,
                        "cycles": session.cycles,
                        "errors": session.errors,
                        "warnings": session.warnings,
                        "baseline": session.baseline,
                        "final": session.final,
                    }
                )
            finally:
                transcript = machine.close()
                records.append((label, transcript))

                if session.errors:
                    path = log_transcript(args, [(label, transcript)])
                    if path:
                        print(f"soak: the transcript is in {path}", file=sys.stderr)

            if session.errors:
                status = 1
                break

            if args.iterations is not None and index + 1 < rounds and not args.verbose:
                passed = len(results)
                print(f"  {label}: {len(results[-1]['errors'])} error(s), "
                      f"boot {results[-1]['boot_seconds']:.2f} s", flush=True)
    finally:
        if page is not None:
            page.close()
        if ntp is not None:
            ntp.close()

    status = report(results, args) or status

    if args.keep_logs and records:
        path = log_transcript(args, records)
        if path:
            print(f"soak: the transcript is in {path}")

    line = "soak: network " + ("on" if args.net else "skip (the build has no MICRO_OS_X86_NET)")
    if args.net:
        line += (
            f", ntp {'on' if services['ntp'] else 'skip (UDP 123 is privileged)'}"
            f", dns {'on' if services['dns'] else 'skip (this host cannot resolve a name)'}"
        )
    print(line)

    return status


def net_enabled(build: str) -> bool:
    """Did this build compile components/net into the port?"""
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return False
    with open(cache, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("MICRO_OS_X86_NET:BOOL="):
                return line.strip().endswith("=ON")
    return False


def can_resolve() -> bool:
    try:
        socket.getaddrinfo("example.com", 80)
        return True
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
