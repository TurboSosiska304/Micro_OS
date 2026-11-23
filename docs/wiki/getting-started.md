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
with a version (`kernel/port/*.h`, 1.8 in this tree), checked at the boot and
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
port: host, contract 1.9
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
microos> tree /
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
make x86-build       # the loader, the kernel image and the programs, in build-x86/
make x86-disk        # build-x86/programs.img: /bin and /etc of MicroFS
make x86-run         # a QEMU window: the framebuffer of 1280x800 and the keyboard
make x86-run-serial  # the same machine on the serial console (-nographic)
make x86-smoke       # boot it and check the transcript, without a window
```

The machine boots from the disk image: `/bin` (thirty-one programs) and `/etc`
are objects of a MicroFS volume that `make x86-disk` writes with the host
tool `tools/mkfs_mfs.c`, and the boot mounts the volume and runs `/bin/hello`
through `execve`. The tree is the tree of the other ports - `/bin`, `/etc`,
`/proc` and `/dev` - so `tree /` shows the same shape here as on the board
and in the simulator.

The window needs a QEMU built with a display backend (the GTK one is a package
of its own in some distributions); [build.md](build.md) has the package names
and the fallbacks. `make x86-run-disk` keeps a disk of its own between runs
(`build-x86/disk.img`), so a file written at the console is there after a
reset; `make x86-run` and `make x86-smoke` start from a fresh build of the
image every time.

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
  A `.mox` on the board runs as a task in the kernel's address space, and a
  bad pointer reboots the machine.
* **Programs need a machine that can enter them.** x86-64 does it with ring 3
  and page tables; the board runs an Xtensa `.mox` without protection; the
  simulator and the null port answer `ENOSYS`, because jumping into an
  arbitrary image inside the test process is not a promise the host can keep.
* **A uid and a gid, but no login.** A file has a mode and an owner, the mode
  is checked against the uid of the task and root passes it, and a task
  inherits its credentials from the one that started it. Nothing starts a
  program as another user yet, and no filesystem stores a group.
* **No TLS, and the network is the LAN.** HTTP, MQTT and telnet are plain
  text; there is no HTTPS, no certificate store, no IPv6 and no raw sockets,
  and `httpd` and `telnet` take no password. Keep the machine on the network
  of its owner ([SECURITY.md](../../SECURITY.md)).
* **FAT32 is read and written, ext2 is read-only.** MicroFS is the root of
  the tree; a build can also carry FAT12/16/32 ([ADR 0009](../adr/0009-filesystem-types.md))
  and mount it beside MicroFS, and it can read an ext2 volume with its
  modes, its owner and its moments ([ADR 0019](../adr/0019-ext2.md)).
  MicroFS keeps the two moments of a change and no journal; FAT keeps no
  time yet, and a writable ext2 and a second FAT volume are still ahead.

[`docs/debts.md`](../debts.md) keeps the full register with the next step of
each row. [`docs/roadmap.md`](../roadmap.md) is the plan.

[Back to the wiki index](README.md)
