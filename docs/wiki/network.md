# The network

Micro OS speaks the network with the sockets of its port: the board has WiFi
and lwIP, x86-64 has a virtio-net card and a small stack of its own, and the
host substrate uses the sockets of Linux. The kernel does not know which:
`kernel/port/net.h` is a socket, a name and a byte, and the client of HTTP and
the resolver of names live above it.

## What a machine has today

| | ESP32 (board) | x86-64 (QEMU) | host substrate |
|---|---|---|---|
| link | WiFi station, lwIP | virtio-net | the sockets of Linux |
| address | DHCP from the access point | static `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3` (the user network of QEMU) | whatever Linux gives |
| names | lwIP (`getaddrinfo`) | a resolver of its own with a cache: `dns <name>` | the resolver of Linux |
| TCP and UDP | yes | yes | yes |
| ICMP | the stack of lwIP | `ping <address>` | no |
| HTTP client | `http get <url>`, `wget <url> <file>` | `http get <url>`, `wget <url> <file>` | tests only |
| HTTP server | `httpd [port\|off]`, the page of the state | `httpd [port\|off]` | no |
| NTP | `ntp [server]` | `ntp [server[:port]]` | no |
| MQTT | `mqtt <host> <port> <topic> <message>`, QoS 0 | not built | no |
| telnet | `telnet [on\|off]`, the shell on port 23 | no | no |
| TLS | the copy of ESP-IDF, not wired to a command | yes, mbedTLS 3.6.2: `https://` in `wget`, `http` and `pkg` | for the tests, through the x86-64 layer |

The commands of x86-64 are `netinfo` (the device, the address, the
counters), `ping`, `netudp` (one exchange), `dns`, `http`, `httpd`, `ntp` and
`wget`. `make x86-net-smoke` boots the machine with a card of QEMU and checks
the stack: ARP, ICMP, UDP, a client and a server of TCP, the cache of the
resolver, a download into a file, a download over TLS and an install from a
repository served over TLS.

## The two clients

`http get` is a client of a console, not of a file:

```text
microos> http get http://example.com/
HTTP 200, 1256 byte(s)
<!doctype html>...
```

It speaks HTTP/1.0, reads the answer into a buffer, and cuts a body that is
longer than the buffer (`components/net/src/http.c`). It is the client of a
page for a person.

`wget` is the client of a file (ADR 0024 §2):

```text
microos> wget http://mirror.example/hello-1.0.0-x86_64.mox /usr/bin/hello.new
wget: HTTP 200, 4288 byte(s) to /usr/bin/hello.new
```

The body goes into the sink as it arrives, so a program of a hundred kilobytes
costs a 256-byte buffer and not a buffer of its size. The client knows the two
shapes of a body (`Content-Length` and the chunked framing of HTTP/1.1),
follows up to five redirects (an absolute URL or a path on the same host),
gives up after a budget of two minutes for a file, and throws away the body of
an answer that is not a 2xx: a mirror's 404 is a status and not a file.

A second run of the same command on a file that is already there sends
`Range: bytes=N-` and continues it:

```text
microos> wget http://mirror.example/big.bin /big.bin
wget: http://mirror.example/big.bin: the answer ended before the body did
microos> wget http://mirror.example/big.bin /big.bin
wget: HTTP 206, 940 byte(s) to /big.bin (resumed at 300)
```

The file keeps the bytes of the run that was cut - a download that stops in
the middle is exactly the file a resume wants - and a server that ignores the
range and answers `200` makes the client start the file again from zero
instead of writing a whole object behind half of an old one.

## The stream under an install

`net_http_download_to()` is the same stream with the transport as a parameter:
`pkg` passes the sockets of the port on a machine and a fake transport in the
tests. `make x86-net-smoke` is a repository on the loopback of the host, a
signed index and a signed program; the machine runs `pkg -Sy`, `pkg -S hello`
and then `exec hello` of what it installed.

## The server

`httpd` publishes the page of the state on port 80, without authentication;
`telnet` puts the shell on port 23 and is off until a person turns it on. Both
are described in [shell.md](shell.md).

## TLS

Since `1.2.0` x86-64 speaks HTTPS (ADR 0024 §3). The stack is mbedTLS 3.6.2, vendored in
`third_party/mbedtls`; its configuration is a TLS client (1.2 and 1.3, X.509,
ECDHE with ECDSA or RSA, AES-GCM/CCM, ChaCha20-Poly1305, SHA-1/256/384/512),
and the platform under it is the kernel: `kmm` for the allocator, the
formatter of the kernel, `RDRAND` with the timer as the fallback for entropy,
the clock of NTP for the dates, and the roots of `/etc/ssl/certs.pem` read
through the VFS. The board keeps the copy that ESP-IDF carries and has no
command of TLS yet; it is a compatibility port (ADR 0018).

A URL with `https` is the same client with one more layer:

```text
microos> ntp 10.0.2.2:123
ntp: 2026-11-22 20:12:37 UTC
microos> wget https://mirror.example/hello-1.0.0-x86_64.mox /usr/bin/hello.new
wget: HTTP 200, 4288 byte(s) to /usr/bin/hello.new
```

The store of roots is one file, `/etc/ssl/certs.pem`, with any number of PEM
certificates. The image of `make x86-disk` carries the bundle of the Mozilla
root program (`tools/ca/README.md`), so a fresh machine speaks HTTPS without a
step first; a person replaces the file with `wget`, and it is one file of the
filesystem and no reflash. A machine that has no store trusts nobody, and says
so.

A bundle is a store of every kind of authority, and this client implements RSA
and the NIST curves P-256 and P-384 (ECDHE with RSA or ECDSA, ecdsa-with-SHA256
and -SHA384). A certificate whose key or signature is something else - the
P-521 root `e-szigno TLS Root CA 2023` of the Mozilla bundle, or an Ed25519
authority - is **skipped**, once per boot, with one line in the log, and the
rest of the file is the store; a file with no usable certificate at all is
still "not a PEM store" and nothing is trusted. A chain that ends in a skipped
root is refused by name, as any untrusted chain is.

The clock is part of the trust. A certificate has dates, and a machine whose
clock NTP has not set cannot check them: the client refuses the connection
before it opens the socket and says which command fixes it.

```text
microos> wget https://mirror.example/x /x
wget: https://mirror.example/x: the clock is not set (1970): the date of a
certificate cannot be checked - run `ntp` first
```

`ntp [server[:port]]` takes the port when it is not 123 (a home server, or the
clock of a test, because the host of QEMU user-net cannot bind 123 without a
privilege). On x86-64 the moment is kept by the kernel over its uptime: this
port has no driver of the CMOS clock yet.

A handshake that fails is a sentence and never a success: no
`/etc/ssl/certs.pem`, the clock of 1970, a certificate of another authority
(`flags 0x8`), a certificate of another name (`flags 0x4`), the name of the
server not in the SAN, or the word of the library for anything else. The head
of an answer is read up to 1024 bytes, which is the 702 of the pages of
GitHub - the repository of `pkg` - with room for a CDN; a longer one is
`the head of the answer is longer than 1024 bytes` and not a lost download.

What it does not do: no revocation (CRL or OCSP), no client certificate, no
session resumption between two downloads, no ALPN. Every download is a
handshake, which on this machine costs a second or two and no more.

## Trust of a package

A `.mox` and the index are signed with Ed25519 over their own bytes, and the
installer checks the digest and the signature before a byte reaches `/usr`: a
hostile mirror can refuse to serve a package, and cannot put another program
in its place (ADR 0024 §4). TLS adds privacy and the name of the server; it
does not replace the signature, and the signature does not replace TLS.

## What is not there yet

* no TLS on the board (above);
* no DHCP and no IPv6 on x86-64: the address of the port is the one of the
  user network of QEMU, and a real link is a later change;
* no congestion window and no retransmission timers beyond the simple ones of
  the port (`docs/ports/x86_64/README.md` says what the stack is); the
  window of a session is one segment in flight and 4 KB of buffer, and the
  reader sends the acknowledgement with the new window as it drains;
* no HTTP/1.1 keep-alive: one session per request, and `Connection: close`.

The network of a machine without a link - the simulator and the host
substrate - is the sockets of Linux for the tests, and nothing for the
console.
