# ADR 0036 - A server of files, and an update that can return

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0018 (the programs of the board), ADR 0020 (the signed image),
  ADR 0022 (the layout of 1.1 and the update), ADR 0024 (the stream and the
  installer), ADR 0032 (a buffer a device can read), `docs/plan-1.6.md` M2

## Context

1.5.0 made a program a process and the network a client: `wget` fills a file,
`http` reads a line, `pkg` installs from a repository, and the only server of
the system is the page of the state - two paths of plain text for a monitor,
one session at a time. M2 of 1.6 asks for the other direction: a program that
serves the files of the volume, a server of TLS beside it, and an update that
brings a signed image over the network and **returns** when the image does
not work.

The lane had to answer two questions before a line of code. The first is what
"a program" means for a server: the plan calls it `/bin/httpd`, and the tree
has a syscall ABI with no socket call in it (the next free number is 48),
because the network of this system reaches the kernel through
`kernel/net.h` - a contract of a port, not of a program. The second is what
the board can carry: its slot is 1.125 MiB with 169 KiB of reserve, its
internal RAM is about 100 KB, and the plan forbids growing the layout.

## Decision

### 1. The server is a program of the component, not of ring 3

`httpd` lives in `components/net` with the clients, is tested on the host,
and runs as a task of the machine on both ports: the same sources over the
sockets of the host, of x86-64 and of lwIP. It is written as a program - the
VFS for its files, one task per session, its own log and its own ceiling -
and it is not a `MOX1` image because the ABI has no way to give a ring-3 task
a socket. This is the same sense in which `pkg` is "a program of x86-64"
(ADR 0024 §5): the portable layer of the system and not the kernel.

The debt is recorded with the shape of the work: a socket call (or a `/dev`
session the VFS can open) and a shell that hands a program its descriptors,
then the engine moves into `/bin/httpd` unchanged.

### 2. What it serves

GET and HEAD, HTTP/1.1, keep-alive with an idle timeout and a bound on the
requests of one session; `Content-Length`, `Content-Type` from the extension,
`ETag` of size and mtime, `304`, a single `Range` with `206` and `416`, a
listing of a directory as a chunked page of links, and a `301` for a
directory named without its slash. Every request is one line of
`/var/log/httpd.log` or of the console.

A target is percent-decoded and walked segment by segment under the root: a
`..` that would climb out of it is `403` and a directory is never served as
the bytes of a file. Nothing is written, so the server needs no policy of
methods beyond `405`.

The ceiling of the sessions is the machine's: the task table is 32 slots, the
stack of x86-64 lends four TCP sessions (`NET_TCP_SOCKETS`), and a session of
the board is a task of 8 KiB of its hundred kilobytes of internal RAM. The
build names the number (3 on x86-64, 3 on the board, 16 on the host) and a
session over it is answered with `503` and closed - a client reads the
refusal, which a dropped SYN is not.

### 3. TLS on the serving side is the same library

`MBEDTLS_SSL_SRV_C` is on in the one configuration of the vendored mbedTLS:
the stack, the X.509 and the platform layer are the ones the client has used
since 1.2.0, and the server adds a certificate and its key read from
`/etc/ssl/server.crt` and `/etc/ssl/server.key` through the VFS. The door is
`httpd tls [port]`, HTTPS on 443 by default; the context of a session is
allocated with `kmm` at the priority of a user and freed with it, and a
handshake has a deadline of six seconds so a client that says half a hello
does not keep a task.

The board does not carry this: its slot has 169 KiB of reserve and the library
is not built for it (ADR 0018). `net_tls_server_supported()` answers "no" and
the command says so instead of pretending.

### 4. The update checks the envelope before the slot

An image is a payload and the 120-byte trailer of ADR 0020. The update
downloads the image into a staging file of the volume, reads the trailer from
the tail of that file, hashes the payload **from the file** (not from the
stream), compares the digest and verifies the Ed25519 signature of the
header with the key of the machine. Only then does a byte go into the
inactive slot.

The state machine has the three states the bootloader of the board keeps in
otadata:

```
NEW      an update wrote the slot; the boot that finds it makes it PENDING
PENDING  the running slot has not confirmed itself
VALID    the system is up and `ota_confirm` kept the slot
```

A boot that finds a PENDING slot from the boot before rolls back to the other
one. On x86-64 and on the host the slots are files of `/var/ota` and the
record is a small text file; the second call of `ota_boot()` is the "reboot".
On the board the slots are the `ota_0`/`ota_1` partitions: the payload goes
through `esp_ota_begin`/`esp_ota_write`/`esp_ota_end`, the trailer to the last
4 KiB of the slot, `esp_ota_set_boot_partition` selects it, and the rollback
is the bootloader's own (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), which the
board's `app_main` and shell already confirm.

The key is the compiled-in public key of the image on the board, and a file
of `/etc/ota/image.pub` in hexadecimal on x86-64, where the key of the build
is a development one.

### 5. The board's update is over HTTP, and the signature is why

The board has no client of TLS either (ADR 0024 §3 records it), and the flash
of its slot cannot take mbedTLS: the reserve is 169 KiB and the client of the
library is more than that. The update over the network of the board therefore
downloads over HTTP and trusts the signature, which is the rule the tree
already keeps - "a package is trusted by its signature, not by its transport"
(ADR 0024 §4). A hostile mirror can refuse the image or serve another one;
it cannot make it boot. The URL of an https server is accepted on a machine
that has the client of TLS, and the day the board has its client the same
command takes it with no change.

`ota fetch` writes the slot and says that a reboot is needed; it does not
reboot the machine. A reboot is a decision of the caller (the console, a
script of init, a monitor), and a command that restarts the board under a
person's hands is not one this system takes.

## What was built

| | |
|---|---|
| `components/net/src/httpd.c` | the server of files: sessions, ranges, tags, listing, log, ceiling |
| `components/net/src/tls_server.c` | the serving side of mbedTLS, the pair of `/etc/ssl` |
| `components/net/src/ota.c`, `ota_file.c` | the update: download, check, slot, state machine |
| `components/port_esp32/src/ota_slot.c` | the slots of the board: `app_update`, the trailer, otadata |
| `components/net/src/net.c` | `httpd serve`, `httpd tls`, `ota`, `wget --sha256` |
| `tests/cases/test_httpd.c`, `test_ota.c` | the loopback of the host, and the update on files |
| `tools/x86_net_smoke.py` | a file served and compared, a 100 MiB stream with its digest, `curl -k` over TLS, the ceiling, the install, the refusal and the rollback |

## The gates it met

`make test`, `test-asan`, `test-smp`, `x86-smoke`, `x86-net-smoke` 87 checks,
`x86-disk-smoke`, `x86-root-smoke`, `x86-smp-smoke`, `smoke`, `idf-build`,
`integrity`.

## What is not here

* the server is not a `MOX1` program (above);
* the board serves plain HTTP and installs over plain HTTP (above);
* the x86-64 loader does not boot a slot of `/var/ota`: the proof of the
  rollback there is the state machine, and the boot of a slot is the board's
  bootloader;
* a file of 100 MiB is served nowhere: the volume of x86-64 is 16 MiB and the
  board's is 5.56 MiB. The smoke proves the 100 MiB in the direction that
  matters for a machine of this size - a client that downloads it and hashes
  it - and the server's large-file path is proven on the host over a file of
  64 KiB with ranges and a head.
