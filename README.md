# Micro OS

A Linux-like **kernel** for small machines: one portable core, and a port per
kind of machine. The kernel is plain C11 and knows nothing about hardware: a
scheduler, a syscall ABI, a VFS with `/proc` and `/dev`, MicroFS, a memory
manager with slabs, an init process, a shell, HTTP, NTP and telnet.

Everything platform specific lives behind the contract of the port
(`kernel/port/*.h`), and the same sources are built for several machines. Each
port has a directory of its own with its notes, its plan and its quirks:

| Port | What it is | Where |
|---|---|---|
| ESP32 | the M5Stack StickC Plus2, on ESP-IDF and FreeRTOS | [docs/ports/esp32](docs/ports/esp32/README.md) |
| x86-64 | bare metal in QEMU, with a loader of its own and no libc | [docs/ports/x86_64](docs/ports/x86_64/README.md) |
| host | a Linux process: the unit tests and the simulator | [docs/ports/host](docs/ports/host/README.md) |
| null | no machine at all: linked by the checks | [ports/port_null](ports/port_null/CMakeLists.txt) |

The board is one port, not the project: a kernel is portable when a second
machine runs it, and the x86-64 port is the proof that this one does.

> Russian version: [README_ru.md](README_ru.md)

## Status

The plan of the kernel is [docs/roadmap.md](docs/roadmap.md). The board has a
plan of its own, with the port: [docs/ports/esp32/roadmap.md](docs/ports/esp32/roadmap.md).
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
of the contract unimplemented. The contract has a version (1.3 today), and the
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
* **a memory manager**: slabs for the small blocks, the heap of the port for
  the large ones;
* **services**: `init` and the scripts of `/etc/init.d`, the configuration of
  `/etc` in the store of the port, the shell, HTTP, NTP, telnet;
* **a console** that is the UART of the board, the stdin of a process and
  COM1 of a PC, with a copy on a screen when the machine has one.

The shell speaks a small subset of Linux: `ls`, `cat`, `write`, `mkdir`, `rm`,
`df`, `mount`, `mkfs`, `ps`, `free`, `dmesg`, `uname`, `uptime`, `help`,
`echo`, `sleep`, `clear`, `reboot`, and the commands the port registers.

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
  there: no memory protection, no address-space isolation. A port with an MMU
  (x86-64) has one and does not use it yet - the page tables per task are the
  next step of the kernel roadmap.
* **`fork()` is a placeholder on those ports.** Without an MMU the child
  shares the memory of the parent and re-enters its program; the answer (pid
  in the parent, 0 in the child) is the POSIX one. A port with an MMU will copy
  the pages ([ADR 0005](docs/adr/0005-programs-and-protection.md)).
* **One user, no file owners at the edge yet.** A file has a mode and an owner
  in its inode, and the mode is checked at the edge of the system; there is a
  single user (uid 0) on the board today.
* **Programs are young.** The format (MOX1), the `execve` call and the window
  of the x86-64 port exist; the first program in `/bin` is the next step.
* **The board has limits of its own**: no PMIC, 8 MB of flash with one
  application slot and no OTA, a 200 mAh battery. They are in
  [docs/ports/esp32](docs/ports/esp32/README.md).

## License

MIT — see [LICENSE](LICENSE).
