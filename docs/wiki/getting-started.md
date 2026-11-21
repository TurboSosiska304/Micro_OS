# Start here

Micro OS is a Linux-like kernel for small machines. One portable core lives in
`components/kernel/`; every machine is a port under it, and a board is a
description of a machine plus the drivers of its chips. The same sources build
as a Linux process (the unit tests and the simulator), as a bare-metal x86-64
machine in QEMU, and as the firmware of the M5Stack StickC Plus2.

It is Linux-*like*, not Linux: the system calls are the same shape, the
filesystem and the shell are the same shape, but there is no MMU on the board,
no multi-user, and no dynamic linker. The x86-64 port has an MMU and uses it:
a program runs in ring 3 in its own address space. The honest list of the
differences is in [`docs/architecture.md`](../architecture.md), section 2.

## One kernel, four ports

| Port | What it is | Documentation |
|---|---|---|
| ESP32 | the StickC Plus2: ESP-IDF, FreeRTOS, lwIP, NVS | [`docs/ports/esp32`](../ports/esp32/README.md) |
| x86-64 | bare metal in QEMU: its own loader, ring 3 | [`docs/ports/x86_64`](../ports/x86_64/README.md) |
| host | a Linux process: the tests and the simulator | [`docs/ports/host`](../ports/host/README.md) |
| null | no machine at all: linked by the checks | [`docs/ports/null`](../ports/null/README.md) |

The board is one port of several, not the project. The boundary is a contract
with a version (`kernel/port/*.h`, 1.6 in this tree), checked at the boot and
by `make integrity`; [ADR 0004](../adr/0004-portable-boundary.md) explains why.

## What you need

| Need | For | Note |
|---|---|---|
| `cmake`, `ninja`, `gcc`, `python3` | every host check | any current Linux distribution |
| `qemu-system-x86_64` | the x86-64 port | optional; without it the machine is only built |
| ESP-IDF v5.3 | the firmware of the board | only for `make idf-*`; see [build.md](build.md) |

`clang-format` is needed by `make format` and by the format check of `make
integrity`; `valgrind` and `pyserial` are optional. The whole list with the
packages of Arch is in [`commands.md`](../../commands.md).

## The first ten minutes

From the root of the repository:

```bash
make test        # build the host and run the unit tests (and the null probe)
make sim         # boot the whole system as a process, on a volume in a file
```

`make sim` is interactive: it boots the kernel, brings up MicroFS on a file
volume, `/proc`, `/dev`, the configuration of `/etc`, the init scripts and the
shell. The transcript starts like this (the version and the build date are the
ones of your tree):

```text
MicroOS 0.5.0-rc1 (build ...)
board: host_sim - Micro OS host simulator
arch: x86_64, compiler: gcc ...
port: host, contract 1.6
[ ok ] sched
file0: 512 sector(s) of 1024 bytes on /tmp/microos-sim.img
sim: the volume is /tmp/microos-sim.img (512 KB)
[ ok ] storage
...
MicroOS shell ready. Type 'help' for the list of commands.
microos>
```

Then type a few commands; leaving the input (Ctrl+D in a terminal, or the end
of a pipe) ends the run:

```text
microos> uname
MicroOS 0.5.0-rc1 x86_64 (build ...)
microos> ls /
/
|-- etc/
|   `-- init.d/
|-- proc/
...
microos> fsck
fsck: 0 file(s), 3 directory(ies), 484 of 487 block(s) free
fsck: the volume is consistent
```

`make smoke` drives the same simulator with a scripted list of commands and
checks the transcript; it is the end-to-end test of the host build.

## The other two machines

The x86-64 port is the one that proves the kernel is portable, and the only
machine that runs a program out of a file:

```bash
make x86-build       # the loader and the kernel image, in build-x86/
make x86-run         # a QEMU window: VGA text and the PS/2 keyboard
make x86-run-serial  # the same machine on the serial console (-nographic)
make x86-smoke       # boot it and check the transcript, without a window
```

The window needs a QEMU built with a display backend (the GTK one is a package
of its own in some distributions); [build.md](build.md) has the package names
and the fallbacks. A disk is optional: `make x86-run-disk` gives the machine
an 8 MB raw image, and what it writes survives a reset.

The board:

```bash
make idf-build                 # the firmware (ESP-IDF v5.3)
make idf-flash                 # over USB; IDF_PORT=/dev/ttyACM0 when it is not found
tools/device-test.sh           # build, flash, read the console, print RESULT: PASS
```

The board comes up with the self test, the hardware doctor, the shell on the
UART and a copy of it on the TFT, `telnet` on port 23, and the commands of the
drivers (`battery`, `display`, `input`, `mic`, `wifi`, ...).

## Where to go next

* [build.md](build.md) - the three builds in detail, and the errors that stop them.
* [shell.md](shell.md) - the command line, the commands and what they print.
* [files.md](files.md) - where `/etc` lives, how `mkfs` and `fsck` behave.
* [programs.md](programs.md) - what `exec` needs and which machine can run it.
* [ports.md](ports.md) - the differences between the four machines at a glance.

## What this is not yet

A short list, so that nothing here reads as a promise:

* **No MMU on the board, no memory protection.** The user/kernel split is
  logical; `fork()` shares the memory of the parent and re-enters the program.
* **Programs run on x86-64 only.** The board's flash is not executable for a
  file and the simulator must not jump into an arbitrary image, so `execve`
  answers `ENOSYS` there.
* **One user, uid 0.** A file has a mode and an owner, and the mode is checked,
  but there is no group and no second user.
* **FAT32 is read and written, ext2 is not there.** MicroFS is the root of
  the tree; a build can also carry FAT12/16/32 ([ADR 0009](../adr/0009-filesystem-types.md))
  and mount it beside MicroFS. MicroFS keeps the two moments of a change and
  no journal; FAT keeps no time yet, and ext2 and a second FAT volume are
  still ahead.

[`docs/debts.md`](../debts.md) keeps the full register with the next step of
each row. [`docs/roadmap.md`](../roadmap.md) is the plan.

[Back to the wiki index](README.md)
