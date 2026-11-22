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
| HTTP client | `http get <url>` | no command yet | tests only |
| HTTP server | `httpd [port\|off]`, the page of the state | no command yet | no |
| NTP | `ntp [server]` | no command yet | no |
| MQTT | `mqtt <host> <port> <topic> <message>`, QoS 0 | not built | no |
| telnet | `telnet [on\|off]`, the shell on port 23 | no | no |

The commands of x86-64 are `netinfo` (the device, the address, the
counters), `ping`, `netudp` (one exchange) and `dns`. `make x86-net-smoke`
boots the machine with a card of QEMU and checks the stack: ARP, ICMP, UDP,
a client and a server of TCP, and the cache of the resolver.

## The HTTP client of the board

`http get` is a client of a console, not of a file:

```text
microos> http get http://example.com/
200, 1256 byte(s)
<!doctype html>...
```

It speaks HTTP/1.0, reads the answer into a buffer, and cuts a body that is
longer than the buffer (`components/net/src/http.c`). There is no chunked
body, no redirect and no TLS, and the answer of a server is a line of text
for a person, not a program to install.

The server is the other direction: `httpd` publishes the page of the state on
port 80, without authentication; `telnet` puts the shell on port 23 and is
off until a person turns it on. Both are described in
[shell.md](shell.md).

## The stream, and what it changes

K12 ([ADR 0024](../adr/0024-stream-and-installer.md)) turns the console
client into a stream: `Content-Length` and a chunked body, a bounded number of
redirects, a timeout, and a sink that is a file or the inactive slot of an
update. The console face is:

```text
microos> wget http://mirror.example/hello-x86_64.mox /var/cache/pkg/hello.mox
hello-x86_64.mox: 8 192 bytes, 2.1 KB/s, done
```

The same stream fills the inactive slot of the board, which is the update over
the network that 1.1 left open.

The public internet is HTTPS. TLS is **mbedTLS**: the board takes the copy of
ESP-IDF, and x86-64 vendors it with a freestanding platform layer - the
allocator of the kernel, entropy from `RDRAND`, the clock from NTP and the
roots of `/etc/ssl/certs.pem` through the VFS. A clock that NTP has not set is
a warning at the handshake, because a certificate has dates.

A package is trusted by its **signature**, not by its transport: TLS protects
the wire, and the Ed25519 trailer of the file protects the program. A hostile
mirror can refuse to serve; it cannot replace the bytes.

## What is not there yet

* no TLS on either machine: the handshake is the work of K12;
* no HTTP client and no NTP command on x86-64 (the network of the port is
  TCP, UDP, ICMP and DNS; the console client is the work of K12);
* no DHCP and no IPv6 on x86-64: the address of the port is the one of the
  user network of QEMU, and a real link is a later change;
* no congestion window and no retransmission timers beyond the simple ones of
  the port (`docs/ports/x86_64/README.md` says what the stack is).

The network of a machine without a link - the simulator and the host
substrate - is the sockets of Linux for the tests, and nothing for the
console.
