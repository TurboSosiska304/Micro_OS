# Changelog

The changes of Micro OS, one section per release, newest first. The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) under the rule of
[ADR 0011](docs/adr/0011-versioning-and-1.0.md): before 1.0 the minor number
was a phase and could break a format; from 1.0 the syscall numbers, MOX1 and
the MicroFS volume change by adding only.

A release is the commit a tag names (`vX.Y.Z`); a tag with a dash
(`vX.Y.Z-pre`) is a pre-release and must match `kernel_version()`.

## [1.0.0] - unreleased

The gate of this number is [`docs/releases/1.0.md`](docs/releases/1.0.md), and
a line of it is met only when the code, the test and the run on the machine
agree. Nothing is released under 1.0.0 yet: this section is the place for the
commits that close the gate, and it is deliberately empty until the first
release candidate is cut.

## [0.8.0] - 2026-11-21

The userland of x86-64 boots from the disk, the kernel and its programs are
W^X, and a `.mox` runs on the board.

### Added

- x86-64 boots its userland from a MicroFS volume on the disk: `make
  x86-disk` writes `/bin` and `/etc` with a host tool, the boot mounts the
  volume and `execve` reads the program as a file; `/proc`, `/dev` and `/etc`
  are part of the machine, and no boot installs a blob.
- MOX1 version 2 carries the machine of the image in the header (`arch`, 0
  x86-64, 1 Xtensa); an image of another machine is `ENOEXEC` before a byte
  runs (ADR 0018).
- ESP32 runs a `.mox`: Xtensa code in an 8 KiB instruction window and data in
  an 8 KiB DRAM window, entered through the door of the port, without an MMU
  and without protection; `/bin/hello`, `/bin/cat` and a foreign image that
  the boot refuses are in the firmware.
- NX/W^X on x86-64: `EFER.NXE`, read-only kernel text and program text, and
  every data page, stack, heap and the screen NX; the port audits its tables
  at the boot and `/bin/nx` proves the faults (ADR 0007).
- ext2, read-only: the superblock, group descriptors, inode table, direct and
  indirect blocks, directories with 255-byte names, and `stat`/`statx` with
  the mode, owner and moments (ADR 0019).
- `fsck fat <point|device> -y` repairs lost clusters in both copies of the
  table, a loop, a cross-link, a broken long name and a size against its
  chain; what the medium cannot explain is named as not repaired.

### Changed

- The port contract is 1.8: `k_port_exec_arch()` names the machine of the
  format, and a port that cannot run a program still names the processor.
- The mode and owner of a file are checked through the path, at `open` and at
  `execve`; NX is not a boot flag.

## [0.7.0] - 2026-11-21

Pipes, signals and a real userland on x86-64; the board's program is a
decision and a plan.

### Added

- `pipe()` (27) and `dup2()` (28): an open file is a counted description and
  the two ends of a pipe are ordinary descriptors (ADR 0014).
- Basic signals: `SIGINT`, `SIGTERM`, `SIGCHLD` and `SIGPIPE`, `kill` (29),
  `signal` (30) and the restorer (31); a handler runs in ring 3 through a
  frame of the port, `waitpid()` is cut by a signal with `EINTR`, Ctrl+C
  reaches the foreground program (ADR 0015).
- `argc`, `argv` and `envp` at the entry, and a library of a program
  (`components/libc/user`) with `_start`, the door, `open`/`read`/`write`,
  `printf` and the strings (ADR 0016).
- `wc`, `head`, `grep`, `mkdir`, `rm` and `pwd` in `/bin`, beside `cat`,
  `echo`, `ls` and `spawn`.
- The shell runs pipelines and redirections (`|`, `>`, `>>`, `<`, `2>`) with a
  task per stage over the inherited descriptors; Ctrl+C reaches every stage
  (ADR 0017). `ls` and `tree` are two commands.
- `fork()` keeps a zombie on the CPU until its exit runs, so a parent that
  waits sees the status it expects.

### Changed

- The wiki site lives on the `wiki` branch; the pages and the link check stay
  in `docs/wiki`.
- The ESP32 program is decided in ADR 0018: MOX1 carries the machine, and the
  board runs a program as a compatibility port after 1.0.

## [0.6.0] - 2026-11-21

The moments of a file, a second writable filesystem, and a network on the
bare machine.

### Added

- MicroFS version 2: 32-byte records with `mtime` and `ctime`, and `statx`
  (26) reports them; a version 1 volume mounts unchanged and reports zero
  (ADR 0013).
- FAT12/16/32 is read and written: create, write, append, truncate, unlink,
  mkdir, long names with their checksum, both copies of the table and
  `statfs`; no journal yet (ADR 0009).
- `mount` and `umount` at runtime over the type registry, several filesystems
  at once, and `/etc/fstab` mounted by the boot after the root (ADR 0009).
- x86-64 network: virtio-net (legacy I/O, polled), ARP, IPv4, ICMP, UDP, a
  TCP client and the server half of the contract, DNS with a cache; `httpd`,
  `http get`, `ntp` and `ping` behind it (`make x86-net-smoke`).
- `fsck fat <point|device>`, read-only: the boot sector and BPB, both copies
  of the table, chains, entries and long names, lost clusters and the free
  count.

## [0.5.0] - 2026-11-21

Protection on x86-64, the first second filesystem, and the gate of 1.0 in the
documents. The 0.5 line was published as `v0.5.0-rc2`; it has no final tag.

### Added

- Ring 3 with page tables of its own, a GDT/TSS and an `int $0x80` gate; a
  fault of a program is a line on the console and the shell survives; `exit`
  of a program returns to its caller; on x86-64 every task has its own errno
  (ADR 0007).
- `fork()` with copy on write on x86-64; the board keeps the placeholder of a
  machine without an MMU.
- `copy_from_user`/`copy_to_user` and `EFAULT` at the edge of a call
  (contract 1.7).
- VGA text and the PS/2 keyboard on x86-64, and a disk on the IDE channel.
- Filesystem types with a registry, and a read-only FAT reader (ADR 0009).
- MQTT 3.1.1 at QoS 0 and 1 over the kernel sockets.
- The board's RTC alarm, deep sleep and a factory reset.
- The optional features behind compile-time flags (ADR 0010), the budget of
  the static DRAM, and a fuzz of the MicroFS parsers.
- The first wiki pages with the link check, and the gate of 1.0
  (`docs/releases/1.0.md`, ADR 0011).

## [0.4.0] - 2026-11-21

Programs as files and the permissions of a file.

### Added

- MOX1, a flat image with a header and a parser that checks it, and
  `execve` (25) with `argv` (ADR 0005, ADR 0006).
- A mode and an owner in the record of a file; `chmod` (23), `chown` (24),
  the `x` bit checked at `execve`, and the path walked with its directories.
- x86-64 runs a program at the boot and from the shell, and a program calls
  the kernel through `int $0x80`.
- `fsck` of MicroFS, read-only: the header, the records and the free map.
- The page of the state over HTTP, and `mkprog.sh` with a check of the
  window a program is linked for.
- The SPM1423 microphone, `PATH` and `stat` in the shell, and the libc
  wrappers of the phase; `execve` answers 0 when the program returns.
