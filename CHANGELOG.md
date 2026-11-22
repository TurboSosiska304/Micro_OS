# Changelog

The changes of Micro OS, one section per release, newest first. The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), the versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) under the rule of
[ADR 0011](docs/adr/0011-versioning-and-1.0.md): before 1.0 the minor number
was a phase and could break a format; from 1.0 the syscall numbers, MOX1 and
the MicroFS volume change by adding only.

A release is the commit a tag names (`vX.Y.Z`); a tag with a dash
(`vX.Y.Z-pre`) is a pre-release and must match `kernel_version()`.

## [Unreleased]

### Added

- `make x86-user`: the complete machine and the x86-64 tar of the release,
  unpacked and started from the tar itself, so the artifact a person
  downloads is the one that runs. `make x86-user-serial` is its console.
  The x86-64 tar now carries `x86-all.img`, the disk of `/bin` and `/etc`,
  and a `run.sh` with the disk and the network; the release workflow builds
  the complete machine before the bundle.
- TLS on x86-64 (ADR 0024 §3): Mbed TLS 3.6.2 vendored in
  `third_party/mbedtls` with a client-only configuration, and a freestanding
  platform layer - `kmm` for the allocator, the formatter of the kernel,
  `RDRAND` with the timer as the fallback for entropy, the clock of NTP for
  the dates, and the roots of `/etc/ssl/certs.pem` read through the VFS.
  `http_parse_url()` takes `https`, the stream wraps the plain transport, and
  `wget`, `http` and `pkg` speak HTTPS with the chain and the name checked; a
  clock that was never set is a refusal with a sentence, never a silent
  success.
- `ntp <server>[:port]` takes a port other than 123, for a home server and
  for the clock of a test.
- The x86-64 port registers a software clock, so `ntp` has a clock to set on
  a machine without a driver of the CMOS chip yet.
- `tests/cases/test_tls.c`: the policy of the clock, a store of roots of one
  and of two certificates, a broken store, and the layer that has no
  transport under it; `make x86-net-smoke` generates a test authority and
  three certificates with `openssl` and checks TLS 1.3 and 1.2, the refusal
  of a wrong name and of a wrong authority, and `pkg` over HTTPS.

### Changed

- `struct http_transport::open` receives the transport itself, so a layer can
  wrap the transport below it; a plain transport ignores the argument.
- `struct http_url` names the scheme: `tls` says the URL is https, and a
  redirect keeps the scheme of the hop.
- The first port of a program of somebody else: zlib 1.3.1 as a static
  archive built by the compiler and the flags of the guest, and `/bin/zcat`
  on a `.gz` of MicroFS. The recipe is `docs/porting-programs.md`; the
  library is in `third_party/zlib` with its port layer and license, the
  image carries `/etc/greeting.gz` and `/etc/licenses/zlib`, and
  `make x86-smoke` and `make x86-disk-smoke` check every inflated byte.
- The library of a program learned the three families a portable source
  expects: an arena of its own in the bss (`malloc`, `calloc`, `realloc`,
  `free`), `errno`/`strerror`, and the streams of `fopen`; the POSIX names of
  the headers are thin shims over `ulibc.h`, and `snprintf`/`vsnprintf` share
  the formatter with `printf`. The host test `tests/cases/test_zlib.c` covers
  the checksums, the three stream wrappers and a `gz` file.

## [1.1.1] - 2026-11-22

A patch of `1.1.0` for the data loss that the cleanroom found: a directory
that grew past its first block made every path of the volume answer an I/O
error, and a full directory corrupted the tree. The notes are in
[`docs/releases/1.1.1.md`](docs/releases/1.1.1.md); the volume, the syscall
ABI and MOX1 do not change, so a machine of 1.1.0 takes the new image as it
is.

### Fixed

- `mfs`: the new block of a directory that grows is written empty with the
  end of its chain in its header before it is linked, as the growth of a
  file always did. A stale header there made a loop; the lookup followed it
  and the whole volume answered `input/output error` at about the two
  hundredth file of one directory. The regression test fills a directory
  with three hundred files and asks the check for a sound volume; on the old
  code it fails at 172 files.
- `mfs`: a directory that is full answers `no space left on device`
  (`MFS_ENOSPC`) and leaves the volume sound, instead of `E2BIG` and an I/O
  error.
- `kshell`: `umount /` is refused; the root of the tree stays and `mkfs -y`
  formats the volume itself.
- `kshell`: `kill 1` is refused; the init of the machine is not killed from
  the console, and `reboot` is the way to leave.
- `free` prints the memory of the machine and not the uptime; the clock has
  its own command.
- x86-64: the small `printf` of the kernel prints octal, so `stat` says
  `mode: 0644` instead of the literal `0%o`.

### Changed

- `x86-smoke` checks the mode of `stat`, the refusal of `umount /` and that
  `free` has no clock: 165 checks.

## [1.1.0] - 2026-11-22

The release of the board's last major changes: the settings of `/etc`, the
partition of the programs, an update with a rollback, and the first phase of
many cores in the kernel. After `1.1.0` the ESP32 port is a compatibility port
(ADR 0018); the notes are in
[`docs/releases/1.1.md`](docs/releases/1.1.md).

### Added

- The systems of `/etc` (ADR 0021): `/etc/display`, `/etc/font` and
  `/etc/console`, one file of `key=value` lines each, parsed without an
  allocation and applied at the boot - the rotation, the inversion, the theme
  and the scale of the font on the board, the attribute of the VGA cells on
  x86-64. `config get`, `config set`, `config list` and `config sync` read and
  write them, and the store keeps them through a format.
- The layout of the flash of 1.1 (ADR 0022): two application slots of
  1.125 MiB, `progs` of 132 KiB and MicroFS of 5.56 MiB for the user. A board
  of 1.0 loses its volume once, at the move of the partition.
- An update over USB with a rollback: `tools/ota_usb.py` writes the inactive
  slot, the bootloader keeps it in `PENDING_VERIFY` until the shell confirms
  the boot, and a boot that ends before that, or an image whose signature does
  not match, returns the board to the previous slot.
- The programs are files of the `progs` partition, mounted read-only at
  `/bin`; `tools/progs_image.py` builds `progs.bin` at build time and no image
  of a program is embedded in the firmware.
- The first phase of many cores (ADR 0012, `KERNEL_SMP`, off by default): a
  `struct k_cpu` per core, a run queue per core, the scheduler lock as the
  critical section of the port, `k_sched_migrate()` and the rule that a
  running task never moves. With the flag off the kernel, its tests and its
  behaviour are those of 1.0.
- The host port takes the number of cores from `MICRO_OS_CPUS` (1 by default);
  `make test-smp`, `make test-tsan` and `make soak-host` run two of them, and
  the two-core soak runs the thread sanitizer over a million operations.
- The x86-64 port reads the processor list of the machine (ACPI: the root
  pointer, the RSDT/XSDT and the MADT) and remembers the local APIC; the boot
  prints how many processors the firmware describes and how many it runs, and
  `make x86-smoke` boots with `-smp 2` and checks the line. `make
  x86-smp-build` builds the two-core kernel; the APs are not started yet
  (ADR 0012, "The pilot of 1.x").
- The port contract is version 1.9: `kernel/port/cpu.h` with
  `k_port_cpu_count` and `k_port_cpu_id`, and a weak default of one core for a
  port that predates them.

### Changed

- The partition `factory` is replaced by `ota_0` and `ota_1`, and the trailer
  of the signature is the last 4 KiB of the slot that runs (ADR 0020); the
  check of the board reads the running slot, not a fixed address.
- A board of 1.0 moves its volume once: the release notes say to back it up
  and to flash with `--erase-volume` (or to run `mkfs -y`); the NVS survives
  the flash and the update.
- The device test covers `/bin` from `progs` and, with `DEVICE_OTA=1`, an
  update to the other slot, the return and a refused trailer that rolls back.

### Fixed

- `task_signal_interrupts()` reads the mark and the handler of a signal with
  an atomic load, so the reader of another core sees the pair the writer made.
- The tick reads the current task of a core atomically, so the accounting of
  a core no longer races with a switch under `KERNEL_SMP`.

## [1.0.0] - 2026-11-22

The first major release: the contracts are frozen, two machines run the
system, and a session of 600 seconds under a mixed load passes on both of
them and on the host substrate. The gate is
[`docs/releases/1.0.md`](docs/releases/1.0.md); the page of the release is
[`docs/wiki/release-1.0.md`](docs/wiki/release-1.0.md).

### Added

- The syscall ABI, MOX1 version 2 and the MicroFS volume are frozen and
  recorded in `tests/corpus/` with tests that compare the record with the
  running kernel; the contract of the port is version 1.8 and the same test
  holds its list.
- x86-64: the kernel is linked in the higher half (`0xFFFFFFFF80000000`,
  physical `0x01000000`), with a low identity map for the screen and the
  arena; NX and W^X cover the kernel and the programs; `fork` with copy on
  write, ring 3, `EFAULT` at the edge of every call.
- The board runs a `.mox` of Xtensa code without protection: a fixed 8 KiB
  instruction window and a data window, the door of the port as the system
  call; the sleep wakes on the RTC alarm, the factory reset writes the
  defaults of `/etc` back, the IR emitter sends NEC and the sound detector
  listens over the microphone.
- Processes and userland: `pipe`, `dup2`, `kill`, `signal`, `sigreturn`,
  `getlimit`/`setlimit`, a shell with pipelines and redirections, Ctrl+C for
  a job, and ten programs built against `components/libc/user`.
- Filesystems: FAT12/16/32 is read and written, `fsck fat -y` repairs a
  damaged volume, MicroFS repairs its own (`fsck -y`), ext2 is read, and
  `mount`/`umount` and `/etc/fstab` are part of the machine.
- Network: the HTTP client, NTP, MQTT, telnet and the page of the state on
  the board; IPv4/TCP, a server half, `httpd` and a DNS cache on x86-64.
- Signed images: SHA-256 and Ed25519 over a trailer (`MOSIGN01`), the x86-64
  loader verifies the kernel before a byte runs, the board verifies its
  application at the boot, and a build flag decides whether an unsigned or an
  older image is refused or accepted with a warning.
- Quality: reproducible images, 82% of the lines of the kernel in the
  coverage, fuzzing of the ABI and of the parsers, ASan/TSan/valgrind, and
  the three tools of the 600-second campaign (`make soak`, `make soak-host`,
  `make board-soak`).

### Changed

- Programs are files of a MicroFS volume (`make x86-disk`), not blobs of the
  kernel; `/proc`, `/dev`, `/etc` and `/etc/fstab` are the tree of x86-64 as
  they are the tree of the board.
- `execve` carries the descriptors, the working directory and the job of the
  caller; the environment is the third argument of POSIX.
- `ls` lists names (`-l`, `-a`) and `tree` draws the levels.

### Known limitations

- The second core (SMP) and the safe update with a rollback are the first
  projects after 1.0 (ADR 0012, ADR 0011 updated).
- No TLS and no IPv6: 1.0 is a LAN system.
- ext2 is read-only and FAT is written without a journal.
- One user; an object has a mode and an owner, and there is no second party.
- A program of the board has no protection: a bad pointer takes the board
  down, and the documentation says so.
- A pipeline has at most three stages and a task four descriptors.

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
