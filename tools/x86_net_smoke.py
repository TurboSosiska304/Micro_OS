#!/usr/bin/env python3
"""Micro OS — the network smoke test of the x86-64 port.

The machine of `tools/x86_smoke.py` has no network: the kernel boots, runs a
program and answers the shell, and the contract of the port says "no" to a
socket. This test gives QEMU a virtio-net device on its user-mode network and
a few services on the loopback of the host, and then asks the guest for the
things a stack has to do:

  * the device comes up (the driver finds it on the PCI bus, reads the MAC
    and the link from the config space);
  * ARP and ICMP: `ping 10.0.2.2` - the gateway of QEMU user-net answers the
    echo, and the request found the MAC of the gateway first;
  * UDP: `netudp 10.0.2.2 <port> hello` against an echo server of the host
    started by this file (QEMU maps 10.0.2.2 to the loopback);
  * TCP and HTTP: `http get http://10.0.2.2:<port>/` through the HTTP client
    of components/net, which is the proof of the handshake, the sequence
    numbers, the acknowledgement of the request and the reading of a body;
  * the server half of the contract: `httpd 8080` opens the page of the state
    on the guest, QEMU forwards a port of the host to it (`hostfwd`), and a
    client of this file reads `/` and `/health` through it - the listen, the
    SYN/ACK, the request of the socket and the close;
  * the resolver: `dns <name>` asks the resolver of user-net, and the second
    `dns` of the same name comes from the cache of the port (the name is one
    the host itself can resolve, and the check is skipped when it cannot);
  * the counters of `netinfo` move, so the frames are counted where they
    crossed and not only in the transcript of the shell.

    tools/x86_net_smoke.py [--timeout SECONDS] [--verbose]

The alignment of a block of the console with the command that made it is
checked by the labels of the steps; `X86_NET_DEBUG_BLOCKS=1` prints the first
line of every block when a run is one block off and the reason is not obvious.

The image is the one of `make x86-net-smoke` (build-x86-net, configured with
MICRO_OS_X86_NET=ON so that components/net is part of it). The test is not a
part of `make ci`: it needs the user-mode network of QEMU and a port on the
loopback of the host, and a build farm grants neither.

The exit status is 0 when every check passed, 1 when one did not, and 2 when
the machine or QEMU is not there to run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

# The prompt of the shell. It is written in one call, so a line of another
# task never splits it and the answers can be separated by it.
PROMPT = "microos> "

# What the kernel has to say before the shell exists, and the one line that
# proves the driver found the card.
EXPECTED = [
    "board: qemu_x86_64",
    "virtio0:",
    "link up",
    "net: 10.0.2.15/24, gateway 10.0.2.2, dns 10.0.2.3",
]

BODY = "hello from the host network"

# The bodies of the stream: a file of a length, the chunks of the same idea,
# and the body that is cut in the middle for the resume.
FILE_BODY = bytes((i * 7 + 13) & 0xFF for i in range(1500))
HALF_BODY = bytes((i * 3 + 1) & 0xFF for i in range(1200))

# The repository of the test: the index and the program, both signed with the
# key of the image of this build (ADR 0024 §4).
REPO_NAME = "microos-progs"
PROGRAM = "hello"
PROGRAM_VERSION = "1.0.0"
PROGRAM_FILE = f"{PROGRAM}-{PROGRAM_VERSION}-x86_64.mox"
PROGRAM_ABI = 2  # K_EXEC_VERSION, the ABI of MOX1 the kernel reads

# The page of the state of the guest is on this port of the guest and on a
# port of the host that QEMU forwards to it.
GUEST_HTTPD_PORT = 8080
GUEST_DNS_NAME = "example.com"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get("X86_BUILD", os.path.join(ROOT, "build-x86-net"))


class UdpEcho(threading.Thread):
    """A UDP echo server on the loopback, which is 10.0.2.2 for the guest."""

    def __init__(self) -> None:
        super().__init__(daemon=True)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.5)
        self.port = self.socket.getsockname()[1]
        self.stop = threading.Event()

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                data, address = self.socket.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            self.socket.sendto(b"echo:" + data, address)

    def close(self) -> None:
        self.stop.set()
        self.socket.close()


class HttpHandler(BaseHTTPRequestHandler):
    """The pages of the test and the repository of programs (ADR 0024).

    The guest asks for a body of a length, a body of chunks, a redirect, a
    body that is cut in the middle (the file of a download that drops) and
    the index and packages of a repository. Every answer says
    `Connection: close`, because the client of the machine opens one session
    per request.
    """

    protocol_version = "HTTP/1.1"
    repository = ""

    def do_GET(self) -> None:  # noqa: N802 (the name is the protocol)
        self.close_connection = True

        if self.path == "/":
            self.reply(200, BODY.encode("ascii"))
            return

        if self.path == "/file.bin":
            self.reply(200, FILE_BODY)
            return

        if self.path == "/go":
            self.send_response(302)
            self.send_header("Location", "/file.bin")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return

        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"6\r\nchunk1\r\n")
            self.wfile.write(b"6\r\nchunk2\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
            return

        if self.path == "/half.bin":
            # The body the index promises is HALF_BODY twice; the first
            # answer carries the first half and the session ends, which is a
            # link that dropped. The second answer is the rest, asked for
            # with a Range.
            start = 0
            header = self.headers.get("Range")
            if header is not None and header.startswith("bytes=") and header.endswith("-"):
                try:
                    start = int(header[len("bytes=") : -1], 10)
                except ValueError:
                    start = 0
            rest = HALF_BODY[start:]
            if start > 0:
                self.send_response(206)
                self.send_header(
                    "Content-Range", f"bytes {start}-{len(HALF_BODY) - 1}/{len(HALF_BODY)}"
                )
            else:
                self.send_response(200)
                rest = HALF_BODY[: len(HALF_BODY) // 2]
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(HALF_BODY) if start == 0 else len(rest)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(rest)
            self.wfile.flush()
            return

        if self.path in ("/index.json", "/index.json.sig") or self.path.endswith(".mox") or \
                self.path.endswith(".mox.sig"):
            name = self.path.lstrip("/")
            path = os.path.join(self.repository, name)

            if os.path.isfile(path):
                with open(path, "rb") as handle:
                    self.reply(200, handle.read())
                return

        self.reply(404, b"not found\n")

    def reply(self, status: int, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def log_message(self, *args) -> None:
        """The console of the test is the transcript of the guest."""


class HttpServer(threading.Thread):
    """A TCP server of the host: a port of the user-mode network, 10.0.2.2."""

    def __init__(self, repository: str = "") -> None:
        super().__init__(daemon=True)
        HttpHandler.repository = repository
        self.server = HTTPServer(("127.0.0.1", 0), HttpHandler)
        self.port = self.server.server_address[1]
        self.stop = threading.Event()
        self.server.timeout = 0.5

    def run(self) -> None:
        while not self.stop.is_set():
            self.server.handle_request()

    def close(self) -> None:
        self.stop.set()
        self.server.server_close()


def free_port() -> int:
    """A port of the loopback nobody holds, for `hostfwd` to take."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def build_repository(directory: str, build: str) -> bool:
    """Write the index of a repository and sign it with the key of the build.

    The program is the `hello.mox` of this build, renamed as a repository
    names it. The key is the private half of the image: the pkg of the
    machine has the public half compiled in, because the release signs the
    kernel and the index with the same key (ADR 0020, ADR 0024 §4).
    """
    sign = os.path.join(build, "microos_sign")
    key = os.path.join(build, "signing.key")
    image = os.path.join(build, "hello.mox")

    for path in (sign, key, image):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-net-build", file=sys.stderr)
            return False

    os.makedirs(directory, exist_ok=True)

    package = os.path.join(directory, PROGRAM_FILE)
    shutil.copyfile(image, package)

    signed = subprocess.run(
        [sign, "sign-file", "--key", key, "--in", package, "--sig", package + ".sig"],
        check=False,
        capture_output=True,
    )
    if signed.returncode != 0:
        print(signed.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        return False

    data = open(package, "rb").read()
    index = {
        "repository": REPO_NAME,
        "machine": "x86_64",
        "abi": PROGRAM_ABI,
        "packages": [
            {
                "name": PROGRAM,
                "version": PROGRAM_VERSION,
                "machine": "x86_64",
                "abi": PROGRAM_ABI,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "file": PROGRAM_FILE,
                "summary": "says hello, from the repository",
            }
        ],
    }
    index_path = os.path.join(directory, "index.json")
    with open(index_path, "w", encoding="utf-8") as handle:
        json.dump(index, handle, indent=2, sort_keys=True)
        handle.write("\n")

    signed = subprocess.run(
        [sign, "sign-file", "--key", key, "--in", index_path, "--sig", index_path + ".sig"],
        check=False,
        capture_output=True,
    )
    if signed.returncode != 0:
        print(signed.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        return False

    return True


def host_get(port: int, path: str, tries: int = 15) -> str:
    """One HTTP request through `hostfwd`, retried until the guest answers.

    QEMU takes the connection as soon as its listener is up and connects to
    the guest behind it, so a refusal here is a guest that has not started
    the door yet; the retries are the seconds the task of the server needs.
    """
    for _ in range(tries):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.sendall(f"GET {path} HTTP/1.0\r\nHost: microos\r\n\r\n".encode("ascii"))
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
            pass
        time.sleep(0.2)
    return ""


def run_machine(loader: str, kernel: str, steps: list, fwd_port: int, timeout: float) -> str:
    """Boot the machine, run the steps and return the transcript.

    A step is ("cmd", text) - a line for the shell - or ("host", callable) -
    something this process does while the guest waits (a request through
    `hostfwd`, a DNS name it prepares). The output is read while the machine
    runs, and a step goes out only when the prompt of the one before is on the
    console: the FIFO of the serial port is small and a line typed too early
    is a line the shell of the port reads as two.
    """
    command = [
        "qemu-system-x86_64",
        "-kernel",
        loader,
        "-initrd",
        kernel,
        "-m",
        "256",
        "-nographic",
        "-no-reboot",
        "-netdev",
        # The port of the host that reaches the page of the state of the
        # guest: a person or a monitor of the host, as in a real machine.
        f"user,id=n0,hostfwd=tcp:127.0.0.1:{fwd_port}-:{GUEST_HTTPD_PORT}",
        # The legacy interface of virtio: the driver programs the I/O BAR and
        # the split virtqueue, and a modern-only device would leave it with no
        # registers to write.
        "-device",
        "virtio-net-pci,netdev=n0,disable-modern=on",
    ]

    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    assert process.stdin is not None

    descriptor = process.stdout.fileno()
    transcript = b""
    index = 0
    commands_before = 0
    deadline = time.monotonic() + timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([descriptor], [], [], 0.1)

            if not ready:
                if index >= len(steps):
                    break

                continue

            chunk = os.read(descriptor, 4096)
            if not chunk:
                break  # the machine went away on its own

            transcript += chunk
            text = transcript.decode("utf-8", errors="replace")

            while index < len(steps):
                kind, payload = steps[index]

                if text.count(PROMPT) <= commands_before:
                    break

                if kind == "cmd":
                    process.stdin.write((payload + "\n").encode("ascii"))
                    process.stdin.flush()
                    commands_before += 1
                else:
                    payload()

                index += 1

            # Every step is done, but the answer of the last command is the
            # line after its prompt: the machine is read until it is there.
            if index >= len(steps) and text.count(PROMPT) > commands_before:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

        while True:
            chunk = os.read(descriptor, 4096)
            if not chunk:
                break
            transcript += chunk

        process.stdout.close()
        process.stdin.close()

    return transcript.decode("utf-8", errors="replace")


def check(output: str, answers: dict, host: dict, dns_name: str | None, commands: int) -> tuple:
    """The expectations about the device, the protocols and the server."""
    checks = []
    notes = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, bool(condition), detail))

    for line in EXPECTED:
        expect(line, line in output)

    # The MAC is the one QEMU gives a card when nobody named another: the
    # driver read it from the config space of the device and not from a
    # constant of its own.
    expect(
        "the driver reads the MAC of the device",
        re.search(r"mac 52:54:00:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}", output) is not None,
    )

    expect(
        "every command is answered",
        len(answers) == commands,
        f"{len(answers)} of {commands} block(s)",
    )

    # --- the device
    info = answers.get("netinfo", "")
    expect("netinfo knows the device", "netinfo: virtio0" in info)
    expect("netinfo prints the address of the guest", "ip 10.0.2.15/24" in info)
    expect("netinfo prints the gateway and the resolver", "gateway 10.0.2.2" in info)

    # --- ARP and ICMP
    ping = answers.get("ping", "")
    expect(
        "the gateway answers the echo request",
        re.search(r"ping: 40 bytes from 10\.0\.2\.2, \d+ ms", ping) is not None,
        ping.strip(),
    )
    expect("the ping is not a failure", "no answer from" not in ping)

    # --- UDP: the echo server of the host, through 10.0.2.2
    udp = answers.get("udp", "")
    expect(
        "the UDP exchange comes back",
        "netudp: 10 byte(s): echo:hello" in udp,
        udp.strip(),
    )

    # --- TCP and HTTP: handshake, request, answer and body
    http = answers.get("http", "")
    expect("the HTTP answer has a status", "HTTP 200" in http, http.strip())
    expect("the body of the answer is the page", BODY in http)
    expect("the HTTP client does not report a failure", "the connection failed" not in http)

    # --- the stream: a file, a redirect, chunks, a body that was cut and a
    # resume (ADR 0024 §2)
    wget = answers.get("wget", "")
    expect(
        "wget downloads a body of a length",
        f"wget: HTTP 200, {len(FILE_BODY)} byte(s) to /file.bin" in wget,
        wget.strip(),
    )

    redirect = answers.get("wget-redirect", "")
    expect(
        "wget follows a redirect and says so",
        f"wget: HTTP 200, {len(FILE_BODY)} byte(s) to /redirect.bin (1 redirect(s))" in redirect,
        redirect.strip(),
    )

    chunked = answers.get("wget-chunked", "")
    expect(
        "wget reads a chunked body",
        "wget: HTTP 200, 12 byte(s) to /chunked.bin" in chunked,
        chunked.strip(),
    )

    cut = answers.get("wget-cut", "")
    expect("a body that ended early is reported", "ended before the body did" in cut, cut.strip())

    resume = answers.get("wget-resume", "")
    expect(
        "a second wget continues the file with a Range",
        f"wget: HTTP 206, {len(HALF_BODY) // 2} byte(s) to /part.bin (resumed at "
        f"{len(HALF_BODY) // 2})" in resume,
        resume.strip(),
    )

    package = answers.get("wget-package", "")
    expect(
        "wget downloads a file larger than the window of the session",
        "wget: HTTP 200" in package and "/prog.mox" in package,
        package.strip(),
    )

    # --- the repository and the installer (ADR 0024 §4 and §5)
    repo = answers.get("pkg-repo", "")
    expect("pkg takes the address of a repository", "repository microos-progs is" in repo,
           repo.strip())

    sync = answers.get("pkg-sync", "")
    expect("pkg -Sy refreshes the index", "refreshing microos-progs" in sync, sync.strip())
    expect("the index is the one of the repository", "1 package(s) in the index" in sync)

    install = answers.get("pkg-install", "")
    expect("pkg plans the install", f"install {PROGRAM} {PROGRAM_VERSION}" in install,
           install.strip())
    expect("the signature of the package is checked", "signature ok" in install)
    expect("the package is installed in /usr/bin",
           f"installed /usr/bin/{PROGRAM} ({PROGRAM_VERSION})" in install)
    expect("a package that did not pass is not installed", "error:" not in install, install.strip())

    query = answers.get("pkg-query", "")
    expect("pkg -Q lists the installed program", f"{PROGRAM} {PROGRAM_VERSION}" in query,
           query.strip())

    run = answers.get("exec", "")
    expect("the installed program runs from /usr/bin", "PROGRAM: hello from /bin" in run,
           run.strip())

    # --- the server half: the guest serves the page of the state, and the
    # host reaches it through QEMU's forward of a port
    started = answers.get("httpd", "")
    expect(
        f"the page of the state is on port {GUEST_HTTPD_PORT}",
        f"httpd: the page is on port {GUEST_HTTPD_PORT}" in started,
        started.strip(),
    )

    page = host.get("page", "")
    expect("the guest accepts a session of the host", "HTTP/1.0 200 OK" in page, page[:80])
    expect("the page names the kernel", "MicroOS" in page)
    expect("the page reports the uptime", "uptime:" in page)
    expect("the page reports the memory", "memory:" in page)

    health = host.get("health", "")
    expect("the page answers /health", health.endswith("\r\n\r\nok\n"), health[:80])

    stopped = answers.get("httpd-off", "")
    expect("the door closes", "httpd: stopped" in stopped, stopped.strip())

    after = host.get("after", "")
    expect("a closed door answers nothing", after == "", after[:80])

    # --- the resolver: one query and one answer of the cache
    if dns_name is None:
        notes.append("the resolver of the guest is not asked: this host cannot resolve a name")
    else:
        first = answers.get("dns", "")
        second = answers.get("dns-again", "")
        expect(
            "the resolver answers a name",
            re.search(r"dns: \S+ is \d+\.\d+\.\d+\.\d+", first) is not None,
            first.strip(),
        )
        expect("the first answer is not from the cache", "from the cache" not in first)
        expect(
            "the second answer is from the cache",
            "from the cache" in second,
            second.strip(),
        )

    # --- the counters of the driver and of the stack moved
    counters = answers.get("netinfo-after", "")
    match = re.search(r"(\d+) frame\(s\) in, (\d+) out", counters)
    expect("the counters of the card are printed", match is not None, counters.strip())
    if match is not None:
        expect(
            "the card took and gave frames",
            int(match.group(1)) >= 8 and int(match.group(2)) >= 8,
            f"{match.group(1)} in, {match.group(2)} out",
        )

    match = re.search(r"(\d+) ip in, (\d+) out", counters)
    if match is not None:
        expect(
            "the stack counted the packets of the protocols",
            int(match.group(1)) >= 8 and int(match.group(2)) >= 8,
            f"{match.group(1)} in, {match.group(2)} out",
        )

    if dns_name is not None:
        match = re.search(r"(\d+) name\(s\) asked, (\d+) from the cache", counters)
        expect("the resolver counted the query and the hit", match is not None, counters.strip())
        if match is not None:
            expect(
                "one query and one hit",
                int(match.group(1)) == 1 and int(match.group(2)) == 1,
                f"{match.group(1)} asked, {match.group(2)} hit",
            )

    return checks, notes


def resolvable_name() -> str | None:
    """The name the guest will ask for, when this host can resolve it too.

    The guest asks the resolver of QEMU user-net (10.0.2.3), which asks the
    resolver of the host; an offline machine has nobody to ask, and the DNS
    checks are then skipped instead of failing the run.
    """
    try:
        socket.getaddrinfo(GUEST_DNS_NAME, 80, socket.AF_INET)
    except OSError:
        return None

    return GUEST_DNS_NAME


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the network smoke test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=90.0, help="the seconds the machine gets")
    parser.add_argument("--verbose", action="store_true", help="print the console of the machine")
    args = parser.parse_args(argv)

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-net-build", file=sys.stderr)
            return 2

    udp = UdpEcho()
    repository = tempfile.mkdtemp(prefix="microos-repo-")

    if not build_repository(repository, BUILD):
        return 2

    http = HttpServer(repository)
    udp.start()
    http.start()

    fwd_port = free_port()
    dns_name = resolvable_name()
    host: dict = {}

    def fetch_page() -> None:
        host["page"] = host_get(fwd_port, "/")
        host["health"] = host_get(fwd_port, "/health")

    def fetch_after_close() -> None:
        host["after"] = host_get(fwd_port, "/", tries=5)

    steps = [
        ("cmd", "netinfo"),
        ("cmd", "ping 10.0.2.2"),
        ("cmd", f"netudp 10.0.2.2 {udp.port} hello"),
        ("cmd", f"http get http://10.0.2.2:{http.port}/"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/file.bin /file.bin"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/go /redirect.bin"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/chunked /chunked.bin"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/half.bin /part.bin"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/half.bin /part.bin"),
        ("cmd", f"pkg repo add http://10.0.2.2:{http.port}/"),
        ("cmd", f"wget http://10.0.2.2:{http.port}/{PROGRAM_FILE} /prog.mox"),
        ("cmd", "pkg -Sy"),
        ("cmd", f"pkg -S {PROGRAM}"),
        ("cmd", "pkg -Q"),
        ("cmd", f"exec {PROGRAM}"),
        ("cmd", f"httpd {GUEST_HTTPD_PORT}"),
        ("host", fetch_page),
    ]

    if dns_name is not None:
        steps.append(("cmd", f"dns {dns_name}"))
        steps.append(("cmd", f"dns {dns_name}"))

    steps += [
        ("cmd", "httpd off"),
        ("host", fetch_after_close),
        ("cmd", "netinfo"),
    ]

    labels = [
        "netinfo",
        "ping",
        "udp",
        "http",
        "wget",
        "wget-redirect",
        "wget-chunked",
        "wget-cut",
        "wget-resume",
        "pkg-repo",
        "wget-package",
        "pkg-sync",
        "pkg-install",
        "pkg-query",
        "exec",
        "httpd",
        None,
    ]

    if dns_name is not None:
        labels += ["dns", "dns-again"]

    labels += ["httpd-off", None, "netinfo-after"]

    try:
        output = run_machine(loader, kernel, steps, fwd_port, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        udp.close()
        http.close()
        shutil.rmtree(repository, ignore_errors=True)

    if args.verbose:
        print(output)

    blocks = output.split(PROMPT)[1:]

    if os.environ.get("X86_NET_DEBUG_BLOCKS"):
        for i, block in enumerate(blocks):
            print(f"block {i}: {block.strip().splitlines()[0][:60] if block.strip() else '(empty)'}")
    answers = {}
    position = 0

    for label in labels:
        if label is None:
            continue
        if position < len(blocks):
            answers[label] = blocks[position]
            position += 1

    checks, notes = check(output, answers, host, dns_name, commands=sum(1 for k, _ in steps if k == "cmd"))
    failed = [item for item in checks if not item[1]]

    print("Micro OS — the network smoke test of the x86-64 port")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    for note in notes:
        print(f"  skip  {note}")

    print("=" * 60)

    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        if "http: command not found" in output:
            print("the image has no HTTP command: configure with -DMICRO_OS_X86_NET=ON")
        print("--- the console of the machine ---")
        print(output)
        return 1

    print(f"OK: {len(checks)} check(s) passed")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
