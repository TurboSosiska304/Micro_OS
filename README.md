# Micro OS

A Linux-like **kernel** for small machines: one portable core, and a port per
kind of machine. The kernel is plain C11 and knows nothing about hardware: a
scheduler, a syscall ABI, a VFS with `/proc` and `/dev`, MicroFS, a memory
manager with slabs, an init process, a shell, HTTP, NTP and telnet.

Everything platform specific lives behind the contract of the port
(`kernel/port/*.h`), and the same sources are built for several machines. Each
port has a directory of its own with its notes, its plan and its quirks:

| Port or substrate | What it is | Where |
|---|---|---|
| ESP32 | the M5Stack StickC Plus2, on ESP-IDF and FreeRTOS | [docs/ports/esp32](docs/ports/esp32/README.md) |
| x86-64 | bare metal in QEMU, with a loader of its own and no libc | [docs/ports/x86_64](docs/ports/x86_64/README.md) |
| host (substrate) | a Linux process: the unit tests and the simulator | [docs/ports/host](docs/ports/host/README.md) |
| null (substrate) | no machine at all: linked by the checks | [docs/ports/null](docs/ports/null/README.md) |

The board is one port, not the project: a kernel is portable when a second
machine runs it, and the x86-64 port is the proof that this one does.

> Russian version: [README_ru.md](README_ru.md)

## Status

The plan of the kernel is [docs/roadmap.md](docs/roadmap.md). The board has a
plan of its own, with the port: [docs/ports/esp32/roadmap.md](docs/ports/esp32/roadmap.md).
The user guide is the wiki in [docs/wiki](docs/wiki/README.md); the same pages
build a site with `mkdocs build` and `mkdocs.yml`. To put the system on a
laptop, from a USB stick to a file that survives the power:
[docs/install.md](docs/install.md).
The changes of every release are in [CHANGELOG.md](CHANGELOG.md); a machine
from 0.x moves with [docs/migration.md](docs/migration.md), and the security
model, with the way to report a problem, is in [SECURITY.md](SECURITY.md).
The reports of the old numbered phases are in [docs/phases/](docs/phases/) -
history, kept by name.

## The key idea: a portable kernel

```
components/kernel/    plain C11, zero ESP-IDF dependencies
kernel/port/*.h       the contract: console, time, memory, locks, power, store,
                      sockets, programs, and the execution substrate
components/port_esp32 the board's port: ESP-IDF, FreeRTOS, lwIP, NVS, the flash
ports/port_host/      a Linux process: the simulator and the unit tests
ports/port_x86_64/    bare metal x86-64 in QEMU: its own loader, no libc
ports/port_null/      no machine at all: linked by the checks
```

The boundary is not a promise, it is a check: `make integrity` refuses a
platform header in the kernel, a `k_port_*` call above it, a portable
component the host build does not compile, and a port that leaves a function
of the contract unimplemented. The contract has a version (1.8 today), and the
boot compares it with the one the port reports ([ADR 0004](docs/adr/0004-portable-boundary.md)).
A new machine is a new port directory - see [docs/porting.md](docs/porting.md).

## Builds

```bash
make test        # the host: build and run the unit tests
make sim         # the host: the simulator, the whole system on a file volume
make x86-build   # x86-64: the kernel and its loader
make x86-smoke   # x86-64: boot it in QEMU and check what it says
make idf-build   # the board: the firmware (needs ESP-IDF, see its port)
```

`make ci` runs the host checks (build, tests, sanitizers, the smoke test of
the simulator, the integrity of the repository); `make test-tsan` and
`make x86-smoke` are one command each, because they need a tool of their own.
The checks of the board are in [its port](docs/ports/esp32/README.md).

## What the kernel provides

* **Tasks and a scheduler** on the substrate of the port: priorities, a time
  slice, sleep, parents and children, waiting;
* **a syscall ABI** (`kernel/ksys.h`, frozen numbers) as the only door, with
  every argument checked at the edge;
* **a VFS** with mount points, `/proc`, `/dev`, and a descriptor that
  remembers the access it was opened with;
* **MicroFS**, a filesystem on a block device (`kernel/blockdev.h`) - the
  partition of the flash on the board, a file in the simulator;
* **filesystems of the world** behind the VFS types: FAT12/16/32 read and
  written, ext2 read-only (modes, owners and the moments of `statx`);
* **a memory manager**: slabs for the small blocks, the heap of the port for
  the large ones;
* **services**: `init` and the scripts of `/etc/init.d`, the configuration of
  `/etc` in the store of the port, the shell, HTTP, NTP, telnet;
* **a console** that is the UART of the board, the stdin of a process and
  COM1 of a PC, with a copy on a screen when the machine has one.

The shell speaks a small subset of Linux: `ls`, `cat`, `write`, `mkdir`, `rm`,
`df`, `mount`, `mkfs`, `fsck`, `chmod`, `chown`, `exec`, `ps`, `free`, `dmesg`,
`uname`, `uptime`, `help`, `echo`, `sleep`, `clear`, `reboot`, and the
commands the port registers.

## Repository layout

```
Micro_OS/
├── components/
│   ├── kernel/         the portable kernel: boot, log, panic, task, sched,
│   │                   vfs, kmm, ksys, kmod, console, exec
│   ├── libc/           a libc subset over the syscall ABI
│   ├── kshell/         the command interpreter and its commands
│   ├── init/           the init process: the scripts of /etc/init.d
│   ├── mfs/            MicroFS and its VFS adapter
│   ├── fs_fat32/       FAT12/16/32, read and written
│   ├── fs_ext2/        ext2, read-only
│   ├── procfs/         the virtual /proc
│   ├── devfs/          the virtual /dev
│   ├── config/         the configuration of /etc, in the store of the port
│   ├── net/, telnet/   HTTP, NTP, the shell over the air
│   ├── graphics/, font/ the console on a screen, the 5x7 font
│   ├── wifi/           the portable core of the WiFi station
│   ├── board/          the description of the board
│   ├── drivers/        the drivers of the board
│   └── port_esp32/     the ESP32 port
├── ports/
│   ├── port_host/      a Linux process: the simulator and the tests
│   ├── port_x86_64/    bare metal x86-64 in QEMU
│   └── port_null/      no machine at all: linked by the checks
├── main/               the firmware entry point of the board
├── tests/              unit tests, own framework, no dependencies
├── tools/              console, bundle, the smoke tests, the integrity check
├── docs/               architecture, roadmap, porting, ADRs, the ports
├── CMakeLists.txt      the builds: ESP-IDF, host, x86-64
└── Makefile            the targets: test, ci, sim, x86-*, idf-*
```

## Development rules

* C11, `-Wall -Wextra -Werror`, 4 spaces, at most 100 columns (enforced by
  `make integrity`).
* Any new kernel behaviour is covered by a test in `tests/`.
* The kernel includes nothing but the kernel; the checks fail the build when
  a platform header or a `k_port_*` call appears above it.
* Before you push: `make ci`; before you touch a port: its smoke test.
* Details: [CONTRIBUTING.md](CONTRIBUTING.md), [docs/](docs/README.md).

## Known limitations

* **No MMU on the boards without one.** The kernel/user split is logical
  there: no memory protection, no address-space isolation. The x86-64 port
  has an MMU and uses it: a program is a task of ring 3 with page tables of
  its own, the text is read-only code and every data page is NX (ADR 0007).
  Since 2.0 the port also has a *policy* ([ADR 0052](docs/adr/0052-the-policy-of-a-page.md)):
  a writable and executable page is refused, the stack of a program grows to
  the limit of its task, the heap and the anonymous mappings stop at
  `ulimit -m`, the kernel is supervisor-only in every space, and SMEP/SMAP
  are turned on when the processor has them - with `make mmu-smoke` as the
  gate. The board keeps its logical split and the host has no space for a
  program, which the ADR says in its first table.
* **`fork()` is a placeholder on those ports.** Without an MMU the child
  shares the memory of the parent and re-enters its program; the answer (pid
  in the parent, 0 in the child) is the POSIX one. On x86-64 the pages are
  copied on write ([ADR 0005](docs/adr/0005-programs-and-protection.md)).
* **A user, a mode, an owner and a group.** A file has a mode, an owner and a
  group in its record, the mode is checked at the edge of the system against
  the effective uid and the gid of the task (owner triad, group triad,
  others), and a task inherits its credentials from the one that started it
  (`getuid`, `geteuid`, `setuid` and their siblings are calls of the ABI).
  Root passes the triads of read and write. `/etc/passwd`, `/etc/shadow` and
  `/etc/group` name the people and the groups, `login`/`su`/`id`/`groups` are
  commands of the shell, and an image may ask for the identity of its owner
  with the setuid flag of `MOX1`
  ([ADR 0053](docs/adr/0053-the-setuid-bit-of-an-image.md)).
* **Programs are young.** The format (MOX1), `execve`, `chmod`/`chown`, the
  window of the x86-64 port and a library of a program exist: `/bin` has
  `hello`, `cat`, `echo`, `ls`, `spawn` and the programs of the tests, all of
  them ring 3 tasks with `argc`, `argv` and `envp` and an `exit` that the
  caller waits for ([ADR 0016](docs/adr/0016-userland.md)). The board runs
  `/bin/hello` and `/bin/cat` too, as tasks of its kernel without an address
  space: one program at a time, 8 KiB of text and 8 KiB of data, and a bad
  pointer can reboot the machine ([ADR 0018](docs/adr/0018-esp32-programs.md)).
  There is no shell `export` (the environment of a program is the four words
  of the kernel) and no job control: no groups, no `&`, no `fg`/`bg`.
* **No TLS, and the network is the LAN.** HTTP, MQTT and telnet speak plain
  text: there is no HTTPS, no certificate store, no IPv6 and no raw sockets.
  `telnet` asks for the account of the machine (`/etc/passwd` and
  `/etc/shadow`) on the socket of the session, and `httpd` takes no password,
  so the machine belongs on the network of its owner, not on the open
  Internet. This is a decision of 1.0, not a missing build
  ([ADR 0011](docs/adr/0011-versioning-and-1.0.md)); the security model is in
  [SECURITY.md](SECURITY.md).
* **A disk this system did not format is read-only.** The SATA controller of
  a chipset and the NVMe controller of a laptop find the disk of a machine,
  mount the volume of its first partition and write nothing: a volume is
  writable only when its superblock carries the identifier of this system
  **and** `/etc/volumes` names it. A volume that passes the two is written,
  and `make x86-disk-smoke` keeps a file across a reset on the IDE channel, on
  SATA and on NVMe (ADR 0041, ADR 0042). What is not there: the same rule for
  a medium of the USB that is written and taken out while it is mounted, and
  an NVMe driver with one pair of queues, one command in flight and no
  completion interrupt (`docs/plan-1.8.md`, M2).
* **The board has limits of its own**: no PMIC, 8 MB of flash with two
  application slots (the update is over USB), a 200 mAh battery. They are in
  [docs/ports/esp32](docs/ports/esp32/README.md).

## License

MIT — see [LICENSE](LICENSE). Third-party components (zlib, PCRE2, Mbed TLS,
Spleen) keep their own licenses; the list and the rules are in
[docs/licensing.md](docs/licensing.md).
