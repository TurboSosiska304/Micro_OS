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
  * TLS: a server of the host with a certificate of a test authority (and
    servers with another name and with another authority), the store of the
    guest in `/etc/ssl/certs.pem`, `ntp` to give the machine a clock, and
    `wget https://...` - the handshake, the chain, the name and the dates
    (ADR 0024 §3); the repository of `pkg` is served over TLS too;
  * the counters of `netinfo` move, so the frames are counted where they
    crossed and not only in the transcript of the shell.

    tools/x86_net_smoke.py [--timeout SECONDS] [--verbose]

The alignment of a block of the console with the command that made it is
checked by the labels of the steps; `X86_NET_DEBUG_BLOCKS=1` prints the first
line of every block when a run is one block off and the reason is not obvious.

The image is the one of `make x86-net-smoke` (build-x86-net, configured with
MICRO_OS_X86_NET=ON so that components/net is part of it). The test is not a
part of `make ci`: it needs the user-mode network of QEMU and a port on the
loopback of the host, and a build farm grants neither. The certificates of
the test are generated with `openssl`; a host without it keeps the checks of
the plain HTTP and says in a note that TLS was not asked.

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
import ssl
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

# The store the guest reads: the root of the test authority, written by a
# plain download before the first https request (ADR 0024 §3).
TLS_ROOTS_PATH = "/etc/ssl/certs.pem"

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
GUEST_FILES_PORT = 8081
GUEST_TLS_PORT = 8443
GUEST_DNS_NAME = "example.com"

# The file the server of the files of the guest serves: the bytes of the host
# are the bytes of the volume, compared by this process after the download.
SERVED_FILE = "/a.bin"
GUEST_SERVED_FILE = "/doc/a.bin"

# The size of the body the machine downloads in one stream: a hundred
# megabytes, checked by the SHA-256 the client computes as it reads. The
# volume of the guest is sixteen megabytes, so /dev/null is the sink and the
# digest is the comparison.
BIG_SIZE = 100 * 1024 * 1024
# The payload of the update: the volume of the guest has about four hundred
# kilobytes free after the image of the machine, and an install keeps two
# slots and one staging file - three payloads of this size fit with room.
UPDATE_PAYLOAD = 64 * 1024

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
    # The files that are not the pages of the test: the root of the test
    # authority, which the guest downloads before its first https request.
    extras: dict = {}

    def do_GET(self) -> None:  # noqa: N802 (the name is the protocol)
        self.close_connection = True

        if self.path in HttpHandler.extras:
            self.reply(200, HttpHandler.extras[self.path])
            return

        if self.path == "/":
            self.reply(200, BODY.encode("ascii"))
            return

        if self.path == "/file.bin":
            self.reply(200, FILE_BODY)
            return

        if self.path == "/big.bin":
            # A hundred megabytes that are not a buffer of a hundred
            # megabytes: the head promises the length and the body goes out in
            # blocks of the pattern the test hashes on both sides.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(BIG_SIZE))
            self.send_header("Connection", "close")
            self.end_headers()
            sent = 0
            while sent < BIG_SIZE:
                take = min(65536, BIG_SIZE - sent)
                self.wfile.write(big_block(sent, take))
                sent += take
            self.wfile.flush()
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


class TlsServer(threading.Thread):
    """A server of HTTPS of the host, 10.0.2.2 from the guest (ADR 0024 §3).

    The certificate and its key are a pair of the test authority, generated
    by this file: a server with the certificate of the store, a server whose
    certificate names another machine (the client must refuse the name) and a
    server whose authority is not in the store (the client must refuse the
    chain). The version of the protocol of every session is kept, so the test
    can say that TLS 1.3 was used and not only that a handshake happened.
    """

    def __init__(self, certificate: str, key: str, maximum: "ssl.TLSVersion | None" = None) -> None:
        super().__init__(daemon=True)
        outer = self
        self.versions: list = []

        class Handler(HttpHandler):
            def do_GET(self) -> None:  # noqa: N802 (the name is the protocol)
                try:
                    outer.versions.append(self.connection.version())
                except (AttributeError, OSError):
                    pass
                super().do_GET()

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.server.timeout = 0.5
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        if maximum is not None:
            context.maximum_version = maximum
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.port = self.server.server_address[1]
        self.stop = threading.Event()

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                self.server.handle_request()
            except (OSError, ssl.SSLError):
                # A refused certificate is the client stopping in the middle
                # of a handshake: the server waits for the next one.
                continue

    def close(self) -> None:
        self.stop.set()
        self.server.server_close()


class NtpServer(threading.Thread):
    """The clock of the test: one answer of NTP on a port of the loopback.

    The port of the protocol is 123 and the host of QEMU user-net cannot
    bind it without a privilege, so the guest is given the port with
    `ntp 10.0.2.2:<port>`. The answer is the moment of this process, which is
    what the certificate of the test is valid at.
    """

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
                _, address = self.socket.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return

            now = int(time.time()) + 2208988800
            packet = bytearray(48)
            packet[0] = 0x24  # version 4, mode 4 (a server)
            packet[1] = 1     # stratum 1
            packet[12:16] = b"LOCL"
            packet[40:44] = now.to_bytes(4, "big")
            packet[44:48] = b"\0\0\0\0"
            self.socket.sendto(bytes(packet), address)

    def close(self) -> None:
        self.stop.set()
        self.socket.close()


def generate_certificates(directory: str) -> bool:
    """The authority and the three certificates of the test, with openssl.

    `openssl` is a tool of the host, not of the machine; a host without it
    cannot make a certificate, and the test then runs without the checks of
    TLS and says so instead of failing a build of a machine that is fine.
    """
    if shutil.which("openssl") is None:
        return False

    def run(arguments: list) -> bool:
        result = subprocess.run(arguments, check=False, capture_output=True)
        if result.returncode != 0:
            print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        return result.returncode == 0

    def authority(name: str) -> bool:
        return run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", os.path.join(directory, name + ".key"),
            "-out", os.path.join(directory, name + ".pem"),
            "-days", "3650", "-subj", "/CN=MicroOS smoke " + name,
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        ])

    def server(name: str, san: str, signer: str) -> bool:
        csr = os.path.join(directory, name + ".csr")
        ext = os.path.join(directory, name + ".ext")
        with open(ext, "w", encoding="utf-8") as handle:
            handle.write(
                f"subjectAltName={san}\n"
                "basicConstraints=CA:FALSE\n"
                "keyUsage=digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\n"
            )
        if not run([
            "openssl", "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", os.path.join(directory, name + ".key"),
            "-out", csr, "-subj", "/CN=server",
        ]):
            return False

        return run([
            "openssl", "x509", "-req", "-in", csr,
            "-CA", os.path.join(directory, signer + ".pem"),
            "-CAkey", os.path.join(directory, signer + ".key"),
            "-CAcreateserial", "-out", os.path.join(directory, name + ".pem"),
            "-days", "825", "-extfile", ext,
        ])

    # The address the guest reaches the host at, and a name that is not it.
    return (
        authority("ca")
        and authority("other-ca")
        and server("server", "IP:10.0.2.2", "ca")
        and server("wrong-name", "DNS:not-this-machine.example", "ca")
        and server("other-authority", "IP:10.0.2.2", "other-ca")
    )


def big_block(offset: int, length: int) -> bytes:
    """A block of the body of /big.bin: the same bytes for the guest."""
    return bytes(((offset + i) * 37 + 11) & 0xFF for i in range(length))


def big_digest() -> str:
    """The SHA-256 of the whole body, computed without keeping it."""
    digest = hashlib.sha256()
    offset = 0
    while offset < BIG_SIZE:
        take = min(1 << 20, BIG_SIZE - offset)
        digest.update(big_block(offset, take))
        offset += take
    return digest.hexdigest()


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


def build_update(directory: str, build: str) -> bool:
    """The signed image an update of the guest installs, and a broken one.

    The envelope is the one of ADR 0020: the payload, then the trailer the
    tool appends and signs with the key of the image. The payload is not a
    kernel - a slot of the file form is a file and the test checks the
    envelope and the state machine - and the broken copy is the same image
    with one byte of the payload changed.
    """
    sign = os.path.join(build, "microos_sign")
    key = os.path.join(build, "signing.key")
    public = os.path.join(build, "signing.pub")

    for path in (sign, key, public):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-net-build", file=sys.stderr)
            return False

    payload = os.path.join(directory, "update.payload")
    with open(payload, "wb") as handle:
        offset = 0
        while offset < UPDATE_PAYLOAD:
            take = min(65536, UPDATE_PAYLOAD - offset)
            handle.write(big_block(offset, take))
            offset += take

    signed = subprocess.run(
        [sign, "sign", "--key", key, "--in", payload,
         "--out", os.path.join(directory, "update.img")],
        check=False,
        capture_output=True,
    )
    if signed.returncode != 0:
        print(signed.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        return False

    with open(os.path.join(directory, "update.img"), "rb") as handle:
        image = bytearray(handle.read())

    image[UPDATE_PAYLOAD // 2] ^= 0x5A

    with open(os.path.join(directory, "update-bad.img"), "wb") as handle:
        handle.write(image)

    return True


def host_request(port: int, raw: bytes, timeout: float = 20.0) -> bytes:
    """One raw request through `hostfwd`, the answer read to the close."""
    chunks = []
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
            client.sendall(raw)
            while True:
                data = client.recv(65536)
                if not data:
                    break
                chunks.append(data)
    except OSError:
        pass
    return b"".join(chunks)


def host_fetch(port: int, path: str, headers: list | None = None,
               method: str = "GET") -> tuple:
    """Status, headers and body of one request of the guest's server."""
    lines = [f"{method} {path} HTTP/1.0", "Host: microos"]
    for header in headers or []:
        lines.append(header)
    answer = host_request(port, ("\r\n".join(lines) + "\r\n\r\n").encode("ascii"))
    head, _, body = answer.partition(b"\r\n\r\n")
    first = head.split(b"\r\n", 1)[0].decode("latin-1")
    status = int(first.split(" ")[1]) if len(first.split(" ")) > 1 else 0
    fields = {}
    for line in head.split(b"\r\n")[1:]:
        name, _, value = line.decode("latin-1").partition(":")
        fields[name.strip().lower()] = value.strip()
    return status, fields, body


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


def run_machine(loader: str, kernel: str, steps: list, forwards: list,
                timeout: float) -> str:
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
        # The ports of the host that reach the doors of the guest: the page
        # of the state, the server of the files of the volume and its TLS
        # door. A person or a monitor of the host, as in a real machine.
        "user,id=n0,"
        + ",".join(f"hostfwd=tcp:127.0.0.1:{host}-:{guest}" for host, guest in forwards),
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


def check(output: str, answers: dict, host: dict, dns_name: str | None, commands: int,
          tls_state: dict) -> tuple:
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

    # --- TLS: the store, the clock, the chain and the name (ADR 0024 §3)
    if not tls_state["enabled"]:
        notes.append("TLS is not asked: openssl is not installed, no certificate of a test")
    else:
        early = answers.get("wget-tls-early", "")
        expect(
            "https on a machine of 1970 is refused with the clock",
            "the clock is not set (1970)" in early and "HTTP 200" not in early,
            early.strip(),
        )

        stored = answers.get("wget-ca", "")
        expect(
            "the store of roots is a file of the guest",
            f"wget: HTTP 200," in stored and TLS_ROOTS_PATH in stored,
            stored.strip(),
        )

        clock = answers.get("ntp", "")
        expect(
            "the clock is set from NTP",
            re.search(r"ntp: 20\d\d-\d\d-\d\d \d\d:\d\d:\d\d UTC", clock) is not None,
            clock.strip(),
        )

        secure = answers.get("wget-tls", "")
        expect(
            "wget downloads a file over TLS",
            f"wget: HTTP 200, {len(FILE_BODY)} byte(s) to /tls.bin" in secure,
            secure.strip(),
        )
        expect(
            "the handshake of the test is TLS 1.3",
            "TLSv1.3" in tls_state["versions"],
            ", ".join(tls_state["versions"]) or "no session reached the server",
        )

        redirected = answers.get("wget-tls-redirect", "")
        expect(
            "a redirect over TLS keeps the scheme",
            f"wget: HTTP 200, {len(FILE_BODY)} byte(s) to /tls-redirect.bin (1 redirect(s))"
            in redirected,
            redirected.strip(),
        )

        buffered = answers.get("http-tls", "")
        expect(
            "http get reads the head of an https answer",
            "HTTP 200" in buffered and "not one we speak" not in buffered,
            buffered.strip(),
        )

        older = answers.get("wget-tls12", "")
        expect(
            "TLS 1.2 is spoken to a server that offers no 1.3",
            f"wget: HTTP 200, {len(FILE_BODY)} byte(s) to /tls12.bin" in older
            and "TLSv1.2" in tls_state["tls12_versions"],
            (older + " " + ", ".join(tls_state["tls12_versions"])).strip(),
        )

        wrong = answers.get("wget-wrong-name", "")
        expect(
            "the certificate of another name is refused",
            "certificate of the server is not trusted (flags 0x4)" in wrong,
            wrong.strip(),
        )

        other = answers.get("wget-other-authority", "")
        expect(
            "the certificate of another authority is refused",
            "certificate of the server is not trusted (flags 0x8)" in other,
            other.strip(),
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
    if tls_state["enabled"]:
        expect("the repository of the test is https", "https://" in repo, repo.strip())

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

    # --- the server of the files of the volume (net/httpd.h)
    serve = answers.get("httpd-serve", "")
    expect(
        "the server of files is on its port",
        f"httpd: the files of /doc are on port {GUEST_FILES_PORT}" in serve,
        serve.strip(),
    )

    expect("a file of the volume is served", host.get("served_status") == 200,
           str(host.get("served_status")))
    expect("the bytes of the file are the bytes of the host",
           host.get("served") == FILE_BODY, repr(host.get("served"))[:80])
    expect("the type of the answer comes from the extension",
           host.get("served_type") == "application/octet-stream", host.get("served_type", ""))
    expect("the answer has a tag", str(host.get("served_etag", "")).startswith('"'),
           host.get("served_etag", ""))
    expect("the length of the answer is the length of the file",
           host.get("served_length") == str(len(FILE_BODY)), host.get("served_length", ""))

    expect("a HEAD answers the head alone", host.get("head_status") == 200
           and host.get("head_body") == b""
           and host.get("head_length") == str(len(FILE_BODY)),
           f"{host.get('head_status')} {host.get('head_length')} {len(host.get('head_body') or b'')}")

    expect("a Range is a 206 of the bytes asked for",
           host.get("range_status") == 206 and host.get("range_body") == FILE_BODY[10:20]
           and host.get("range_field") == f"bytes 10-19/{len(FILE_BODY)}",
           f"{host.get('range_status')} {host.get('range_field')}")

    expect("a tag the client has is a 304 without a body",
           host.get("not_modified") == 304 and host.get("not_modified_body") == b"",
           str(host.get("not_modified")))

    expect("a path that climbs out of the root is refused",
           host.get("traversal") in (400, 403), str(host.get("traversal")))

    expect("a directory is a listing",
           host.get("listing_status") == 200
           and b"a.bin" in (host.get("listing") or b"")
           and "text/html" in str(host.get("listing_type", "")),
           f"{host.get('listing_status')} {host.get('listing_type')}")

    expect("the ceiling of the sessions answers 503 and closes",
           b"503 Service Unavailable" in (host.get("busy_answer") or b""),
           (host.get("busy_answer") or b"")[:60].decode("latin-1"))

    log = answers.get("httpd-log", "")
    expect("every request is a line of the log of the volume",
           f"GET {SERVED_FILE} 200 {len(FILE_BODY)}" in log and "HEAD " in log, log.strip()[:120])

    off = answers.get("httpd-serve-off", "")
    expect("the server of files closes its door", "httpd: stopped" in off, off.strip())

    # --- the stream of a hundred megabytes (wget --sha256)
    big = answers.get("wget-big", "")
    expect("the machine downloads a hundred megabytes",
           f"wget: HTTP 200, {BIG_SIZE} byte(s)" in big, big.strip()[:160])
    expect("the digest of the stream is the digest of the host",
           f"sha256: {big_digest()}" in big, big.strip()[-80:])

    # --- TLS on the serving side (net/tls_server.h)
    if tls_state["enabled"]:
        if host.get("tls_skipped"):
            notes.append("curl is not installed: the TLS door of the guest is not asked")
        else:
            started = answers.get("httpd-tls", "")
            expect(
                "the TLS door of the guest is on its port",
                f"httpd: the files of /doc are on port {GUEST_TLS_PORT} (TLS)" in started,
                started.strip(),
            )
            expect("curl -k reads a file of the guest over TLS 1.3",
                   host.get("tls_rc") == 0 and host.get("tls_body") == FILE_BODY,
                   (host.get("tls_error") or "") + " " + str(host.get("tls_rc")))
            expect("the listing of the guest over TLS is a 200",
                   host.get("tls_status") == "200", str(host.get("tls_status")))
            closed = answers.get("httpd-tls-off", "")
            expect("the TLS door closes", "httpd: stopped" in closed, closed.strip())

    # --- the update over the network (net/ota.h, ADR 0022 §7)
    key = answers.get("ota-key", "")
    expect("the guest takes the key of an image", "wget: HTTP 200" in key, key.strip())

    fetch = answers.get("ota-fetch", "")
    expect("a signed image is installed in the inactive slot",
           "is in slot 1 (new); a reboot is needed" in fetch, fetch.strip())

    boot = answers.get("ota-boot", "")
    expect("the boot of the new slot waits for its confirmation",
           "ota: slot 1 pending" in boot, boot.strip())

    confirm = answers.get("ota-confirm", "")
    expect("the confirmation keeps the slot", "ota: slot 1 is valid" in confirm,
           confirm.strip())

    bad = answers.get("ota-bad", "")
    expect("an image that is not the one the trailer hashes is refused",
           "the image is not the one the trailer hashes" in bad, bad.strip())

    good = answers.get("ota-status-good", "")
    expect("the refused image changed no slot", "ota: slot 1 valid" in good, good.strip())

    again = answers.get("ota-fetch-2", "")
    expect("a second image goes to the other slot",
           "is in slot 0 (new)" in again, again.strip())

    waiting = answers.get("ota-boot-2", "")
    expect("the boot of the second image is pending",
           "ota: slot 0 pending" in waiting, waiting.strip())

    rolled = answers.get("ota-boot-3", "")
    expect("a boot that did not confirm returns to the other slot",
           "ota: slot 1 valid" in rolled and "rolled back" in rolled, rolled.strip())

    status = answers.get("ota-status-rollback", "")
    expect("the machine runs the slot that confirmed itself",
           "ota: slot 1 valid" in status and "slot 0" not in status, status.strip())

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
    parser.add_argument("--timeout", type=float, default=150.0, help="the seconds the machine gets")
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
    certificates = tempfile.mkdtemp(prefix="microos-certs-")

    if not build_repository(repository, BUILD):
        return 2

    # The image of an update and the key of the build: the guest fetches the
    # public half into /etc/ota (net/ota.h) and installs the payload of the
    # signed envelope into a slot that is a file of its volume.
    if not build_update(repository, BUILD):
        return 2

    update_extras = {
        "/update.img": open(os.path.join(repository, "update.img"), "rb").read(),
        "/update-bad.img": open(os.path.join(repository, "update-bad.img"), "rb").read(),
        "/signing.pub": open(os.path.join(BUILD, "signing.pub"), "rb").read(),
    }

    http = HttpServer(repository)
    udp.start()
    http.start()

    # --- the servers of TLS (ADR 0024 §3), when the host can make a
    # certificate. Three certificates and one NTP answer: the store of the
    # guest is the root of the first authority, and the two others are the
    # mistakes a client has to refuse - another name, another authority.
    tls_state: dict = {"enabled": False, "versions": [], "tls12_versions": []}
    tls_servers: list = []
    ntp = None

    if generate_certificates(certificates):
        HttpHandler.extras = {
            "/ca.pem": open(os.path.join(certificates, "ca.pem"), "rb").read(),
            # The pair the guest's own server is given: the certificate of the
            # test authority and its key, as the files of a machine that
            # serves HTTPS (components/net/include/net/tls_server.h).
            "/server.pem": open(os.path.join(certificates, "server.pem"), "rb").read(),
            "/server.key": open(os.path.join(certificates, "server.key"), "rb").read(),
        }

        def certificate(name: str) -> tuple:
            return (
                os.path.join(certificates, name + ".pem"),
                os.path.join(certificates, name + ".key"),
            )

        good = TlsServer(*certificate("server"))
        tls12 = TlsServer(*certificate("server"), maximum=ssl.TLSVersion.TLSv1_2)
        wrong_name = TlsServer(*certificate("wrong-name"))
        other_authority = TlsServer(*certificate("other-authority"))
        ntp = NtpServer()
        tls_servers = [good, tls12, wrong_name, other_authority]
        for server in tls_servers:
            server.start()
        ntp.start()
        tls_state = {
            "enabled": True,
            "port": good.port,
            "tls12_port": tls12.port,
            "wrong_name_port": wrong_name.port,
            "other_authority_port": other_authority.port,
            "ntp_port": ntp.port,
            "versions": good.versions,
            "tls12_versions": tls12.versions,
        }

    HttpHandler.extras = {**HttpHandler.extras, **update_extras}

    fwd_port = free_port()
    files_port = free_port()
    tls_port = free_port()
    forwards = [
        (fwd_port, GUEST_HTTPD_PORT),
        (files_port, GUEST_FILES_PORT),
        (tls_port, GUEST_TLS_PORT),
    ]
    dns_name = resolvable_name()
    host: dict = {}

    def fetch_page() -> None:
        host["page"] = host_get(fwd_port, "/")
        host["health"] = host_get(fwd_port, "/health")

    def fetch_after_close() -> None:
        host["after"] = host_get(fwd_port, "/", tries=5)

    def fetch_files() -> None:
        """The requests of the host to the server of the files of the guest.

        The bytes of /doc/a.bin are the bytes of the host that wget wrote
        there a step before: the download, the head, a range, a tag the
        client already has, a path that climbs and a listing of a directory.
        """
        status, fields, body = host_fetch(files_port, SERVED_FILE)
        host["served_status"] = status
        host["served"] = body
        host["served_type"] = fields.get("content-type", "")
        host["served_etag"] = fields.get("etag", "")
        host["served_length"] = fields.get("content-length", "")

        status, fields, body = host_fetch(files_port, SERVED_FILE, method="HEAD")
        host["head_status"] = status
        host["head_body"] = body
        host["head_length"] = fields.get("content-length", "")

        status, fields, body = host_fetch(files_port, SERVED_FILE, ["Range: bytes=10-19"])
        host["range_status"] = status
        host["range_body"] = body
        host["range_field"] = fields.get("content-range", "")

        status, fields, body = host_fetch(
            files_port, SERVED_FILE, [f"If-None-Match: {host['served_etag']}"]
        )
        host["not_modified"] = status
        host["not_modified_body"] = body

        status, _, _ = host_fetch(files_port, "/../etc/passwd")
        host["traversal"] = status

        status, fields, body = host_fetch(files_port, "/")
        host["listing_status"] = status
        host["listing_type"] = fields.get("content-type", "")
        host["listing"] = body

    def fetch_tls() -> None:
        """curl -k against the TLS door of the guest, through `hostfwd`."""
        if shutil.which("curl") is None:
            host["tls_skipped"] = True
            return

        # --tlsv1.3 is the proof of the version: a server that offered only
        # 1.2 would refuse this handshake and the body would not arrive.
        result = subprocess.run(
            ["curl", "-k", "-s", "-S", "--max-time", "90", "--tlsv1.3", "--tls-max", "1.3",
             "-o", "-", f"https://127.0.0.1:{tls_port}{SERVED_FILE}"],
            check=False, capture_output=True,
        )
        host["tls_body"] = result.stdout
        host["tls_rc"] = result.returncode
        if result.returncode != 0:
            host["tls_error"] = result.stderr.decode("utf-8", errors="replace")[:200]

        # The server keeps a session alive for its idle timeout, so the
        # second request of this process waits for the first to end.
        time.sleep(0.5)

        status = subprocess.run(
            ["curl", "-k", "-s", "-S", "--max-time", "90", "-o", "/dev/null",
             "-w", "%{http_code}", f"https://127.0.0.1:{tls_port}/"],
            check=False, capture_output=True,
        )
        host["tls_status"] = status.stdout.decode("ascii", errors="replace").strip()
        host["tls_listing"] = status.returncode == 0

    def fetch_busy() -> None:
        """The ceiling of the sessions: the door is full and says 503.

        The stack of the port lends four sessions (four sockets), and the
        server of the files serves three: the sockets of this process hold
        the three and one more is answered with 503 and closed.
        """
        idle = []

        try:
            for _ in range(3):
                client = socket.create_connection(("127.0.0.1", files_port), timeout=5)
                idle.append(client)
                time.sleep(0.15)

            time.sleep(0.4)

            answer = host_request(
                files_port, b"GET /a.bin HTTP/1.0\r\nHost: microos\r\n\r\n", timeout=10
            )
            host["busy_answer"] = answer
        except OSError as error:
            host["busy_answer"] = b""
            host["busy_error"] = str(error)
        finally:
            for client in idle:
                client.close()

        time.sleep(0.5)

    steps: list = []
    labels: list = []

    def command(text: str, label: str) -> None:
        steps.append(("cmd", text))
        labels.append(label)

    def action(call) -> None:
        steps.append(("host", call))
        labels.append(None)

    command("netinfo", "netinfo")
    command("ping 10.0.2.2", "ping")
    command(f"netudp 10.0.2.2 {udp.port} hello", "udp")
    command(f"http get http://10.0.2.2:{http.port}/", "http")
    command(f"wget http://10.0.2.2:{http.port}/file.bin /file.bin", "wget")
    command(f"wget http://10.0.2.2:{http.port}/go /redirect.bin", "wget-redirect")
    command(f"wget http://10.0.2.2:{http.port}/chunked /chunked.bin", "wget-chunked")
    command(f"wget http://10.0.2.2:{http.port}/half.bin /part.bin", "wget-cut")
    command(f"wget http://10.0.2.2:{http.port}/half.bin /part.bin", "wget-resume")

    if tls_state["enabled"]:
        # The order matters: https before `ntp` is a clock of 1970 and a
        # refusal, which is the policy of ADR 0024 §3.
        command(f"wget https://10.0.2.2:{tls_state['port']}/file.bin /tls-early.bin",
                "wget-tls-early")
        command("mkdir /etc/ssl", "tls-directory")
        command(f"wget http://10.0.2.2:{http.port}/ca.pem {TLS_ROOTS_PATH}", "wget-ca")
        command(f"ntp 10.0.2.2:{tls_state['ntp_port']}", "ntp")
        command(f"wget https://10.0.2.2:{tls_state['port']}/file.bin /tls.bin", "wget-tls")
        command(f"wget https://10.0.2.2:{tls_state['port']}/go /tls-redirect.bin",
                "wget-tls-redirect")
        command(f"http get https://10.0.2.2:{tls_state['port']}/", "http-tls")
        command(f"wget https://10.0.2.2:{tls_state['tls12_port']}/file.bin /tls12.bin",
                "wget-tls12")
        command(f"wget https://10.0.2.2:{tls_state['wrong_name_port']}/file.bin /wrong.bin",
                "wget-wrong-name")
        command(
            f"wget https://10.0.2.2:{tls_state['other_authority_port']}/file.bin /other.bin",
            "wget-other-authority",
        )

    scheme = "https" if tls_state["enabled"] else "http"
    repository_port = tls_state["port"] if tls_state["enabled"] else http.port
    command(f"pkg repo add {scheme}://10.0.2.2:{repository_port}/", "pkg-repo")
    command(f"wget http://10.0.2.2:{http.port}/{PROGRAM_FILE} /prog.mox", "wget-package")
    command("pkg -Sy", "pkg-sync")
    command(f"pkg -S {PROGRAM}", "pkg-install")
    command("pkg -Q", "pkg-query")
    command(f"exec {PROGRAM}", "exec")

    # --- the server of files of the volume (components/net/src/httpd.c)
    command("mkdir /doc", "doc-directory")
    command(f"wget http://10.0.2.2:{http.port}/file.bin {GUEST_SERVED_FILE}", "doc-file")
    command("mkdir /var/log", "log-directory")
    command(f"httpd serve {GUEST_FILES_PORT} /doc", "httpd-serve")
    action(fetch_files)
    action(fetch_busy)
    command("cat /var/log/httpd.log", "httpd-log")

    # The hundred megabytes of a stream, checked by the digest the client
    # computes while it reads: the sink is /dev/null, the volume is sixteen
    # megabytes, and the comparison is the SHA-256 of the bytes on the wire.
    command(f"wget --sha256 http://10.0.2.2:{http.port}/big.bin /dev/null", "wget-big")
    command("httpd off", "httpd-serve-off")

    if tls_state["enabled"]:
        # --- the serving side of TLS on the same files
        command(f"wget http://10.0.2.2:{http.port}/server.pem /etc/ssl/server.crt",
                "tls-server-cert")
        command(f"wget http://10.0.2.2:{http.port}/server.key /etc/ssl/server.key",
                "tls-server-key")
        command(f"httpd tls {GUEST_TLS_PORT} /doc", "httpd-tls")
        action(fetch_tls)
        command("httpd off", "httpd-tls-off")

    # --- the update over the network (net/ota.h, ADR 0022 §7)
    command("mkdir /etc/ota", "ota-directory")
    command(f"wget http://10.0.2.2:{http.port}/signing.pub /etc/ota/image.pub", "ota-key")
    command(f"ota fetch http://10.0.2.2:{http.port}/update.img", "ota-fetch")
    command("ota boot", "ota-boot")
    command("ota confirm", "ota-confirm")
    command(f"ota fetch http://10.0.2.2:{http.port}/update-bad.img", "ota-bad")
    command("ota status", "ota-status-good")
    command(f"ota fetch http://10.0.2.2:{http.port}/update.img", "ota-fetch-2")
    command("ota boot", "ota-boot-2")
    command("ota boot", "ota-boot-3")
    command("ota status", "ota-status-rollback")

    command(f"httpd {GUEST_HTTPD_PORT}", "httpd")
    action(fetch_page)

    if dns_name is not None:
        command(f"dns {dns_name}", "dns")
        command(f"dns {dns_name}", "dns-again")

    command("httpd off", "httpd-off")
    action(fetch_after_close)
    command("netinfo", "netinfo-after")

    try:
        output = run_machine(loader, kernel, steps, forwards, args.timeout)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    finally:
        udp.close()
        http.close()
        for server in tls_servers:
            server.close()
        if ntp is not None:
            ntp.close()
        shutil.rmtree(repository, ignore_errors=True)
        shutil.rmtree(certificates, ignore_errors=True)

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

    checks, notes = check(output, answers, host, dns_name,
                          commands=sum(1 for k, _ in steps if k == "cmd"), tls_state=tls_state)
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
