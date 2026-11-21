# Security policy

Micro OS is a small system for machines of its owner. This document says what
1.0 protects, what it does not, and how to tell the project about a problem.
It is a description of the tree, not a promise of a certification.

## Supported versions

| Version | Supported |
|---|---|
| `1.0.x` | yes: fixes land in the candidate and then in `1.0.x` |
| `1.0.0-rc*` | yes, until `1.0.0` is cut |
| `0.x` | no: a development line; see [docs/migration.md](docs/migration.md) |

## Reporting a vulnerability

**Do not open a public issue with the details.** Use the private security
advisory of the repository:

<https://github.com/TurboSosiska304/Micro_OS/security/advisories/new>

If that page is not available to you, open a minimal public issue that asks
for a private channel and contains no exploit and no detail of the problem.
A direct message to the maintainer of the repository is equally fine.

Please include:

* the version (`cat /proc/version`), the port (ESP32, x86-64, host) and how
  the machine was built (`make idf-build`, `make x86-disk`, the bundle);
* what you expected and what happened, with the console lines, the build id
  and a program or a transcript that repeats it;
* whether the problem needs a local program, a network peer, the serial
  console or physical access.

The maintainers answer on a best-effort basis: there is no bounty fund and no
contractual response time. A report is acknowledged in the advisory, fixed on
a branch, and named in [CHANGELOG.md](CHANGELOG.md) when the fix is released.
Credit is given in the advisory unless you ask otherwise.

## What 1.0 protects

### x86-64: ring 3 and page tables

The x86-64 port has an MMU and uses it ([ADR 0007](docs/adr/0007-paging-and-user-mode.md)):

* a program is a task of ring 3 with page tables of its own; the kernel map is
  supervisor-only, the null page is not mapped and the stack has a guard page;
* the arguments of a system call are checked at the edge, and
  `copy_from_user`/`copy_to_user` answer `EFAULT` for a range that is not the
  program's memory;
* NX/W^X: the text of the kernel and of a program is the only executable
  memory, and no page is writable and executable at once;
* `fork()` copies the page tables and both copies are read-only until the
  first write.

### ESP32: no MMU, no sandbox

The board's processor has no MMU. A `.mox` there is a task **in the kernel's
address space**, with 8 KiB of text and 8 KiB of data ([ADR 0018](docs/adr/0018-esp32-programs.md)):

* a bad pointer of a program can read or write kernel memory, and a fault of
  the machine reboots the board;
* one program runs at a time; it cannot start another;
* the image is refused before a byte runs when its header or its machine is
  wrong, but a valid image is trusted like the kernel itself.

A program on the board is not a security boundary. Do not run code you would
not flash as firmware.

### The host and the simulator

The host port is a development machine, not a security boundary: it is a
Linux process with pthreads, files and sockets, and its kernel shares the
address space of the tests. It does not enter programs
(`k_port_exec_supported()` is false) precisely because jumping into an image
inside the test process would run it with the libc of the host. The null port
is not a machine at all.

### The network is the LAN

1.0 has **no TLS and no authentication** ([ADR 0011](docs/adr/0011-versioning-and-1.0.md)):

* HTTP is plain `http://`; there is no HTTPS, no certificate store, no
  server name check and no STARTTLS;
* `httpd` (port 80, or what you choose), `telnet` (port 23) and the MQTT
  client (1883) speak clear text; `telnet` and `httpd` take no password;
* NTP is unauthenticated UDP; there is no IPv6, no raw socket and no packet
  filter;
* the configuration store (NVS on the board) and the files of the volume are
  not encrypted.

The machine belongs on the network of its owner. A port opened to the
Internet is a service anyone can reach, and the traffic can be read and
changed on the way. Terminate TLS on another machine, or use a tunnel, if a
network you do not own is in the path.

### Files and users

A file has a mode and an owner, and the mode is checked at `open`, on the
directories of a path and at `execve`. There is **one user**, uid 0; the bits
of the group and of the others are stored and checked, but there is no second
user and no privilege separation. The board reads and writes its own flash;
the serial console gives a shell without a password.

### The image and the update path

In the tree of 0.8.0-rc1 the firmware image and a program image are checked
for their shape (magic, version, machine, sizes), not for a cryptographic
signature. A checksum and a signature that the boot checks are a gate of 1.0
and are not in this tree yet ([docs/releases/1.0.md](docs/releases/1.0.md)
§7). The firmware in this tree does not enable secure boot or flash
encryption; physical access to the board is the boundary. Flash the board
from a source you trust.

## Out of scope

The following are known and documented, and a report that confirms them is
not a vulnerability:

* a program on the board that reboots the machine by touching memory that is
  not its own — the price of a machine without an MMU (ADR 0018);
* a plain-text service on the LAN, or its lack of a password — the network
  model of 1.0;
* a local program that exhausts memory, descriptors or CPU — the limits per
  task are still an open gate of 1.0;
* physical access: the serial console, the USB port, reading or rewriting the
  flash;
* defects in ESP-IDF, FreeRTOS, lwIP, QEMU or the Linux host — report those to
  their projects.

## Where the model is written down

* [ADR 0004](docs/adr/0004-portable-boundary.md) — the portable boundary.
* [ADR 0007](docs/adr/0007-paging-and-user-mode.md) — paging, ring 3, the
  failure of a program, NX/W^X and the decision about SMAP/SMEP.
* [ADR 0011](docs/adr/0011-versioning-and-1.0.md) — versioning and what 1.0
  does not require.
* [ADR 0018](docs/adr/0018-esp32-programs.md) — a program on the board.
* [docs/architecture.md](docs/architecture.md) — the design of the kernel.
* [README.md](README.md) — the short list of limitations.
