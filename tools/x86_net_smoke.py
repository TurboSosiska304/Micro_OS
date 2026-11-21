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

The image is the one of `make x86-net-smoke` (build-x86-net, configured with
MICRO_OS_X86_NET=ON so that components/net is part of it). The test is not a
part of `make ci`: it needs the user-mode network of QEMU and a port on the
loopback of the host, and a build farm grants neither.

The exit status is 0 when every check passed, 1 when one did not, and 2 when
the machine or QEMU is not there to run.
"""

from __future__ import annotations

import argparse
import os
import re
import select
import socket
import subprocess
import sys
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
    """One page, with a body the test compares byte for byte."""

    def do_GET(self) -> None:  # noqa: N802 (the name is the protocol)
        body = BODY.encode("ascii")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args) -> None:
        """The console of the test is the transcript of the guest."""


class HttpServer(threading.Thread):
    """A TCP server of the host: a port of the user-mode network, 10.0.2.2."""

    def __init__(self) -> None:
        super().__init__(daemon=True)
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
    http = HttpServer()
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

    if args.verbose:
        print(output)

    blocks = output.split(PROMPT)[1:]
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
