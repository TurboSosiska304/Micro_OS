# Micro OS roadmap

> Russian version: [ru/roadmap_ru.md](ru/roadmap_ru.md) (it may lag).

This is the plan of **the kernel**: one portable core, the contract of the
port, the programs, the protection, the machines it runs on, and the quality
of all of it. The plan of the board (the ESP32 port) is in
[ports/esp32/roadmap.md](ports/esp32/roadmap.md); the board is one port of
several, and its build is `make idf-build`. What each section leaves open is
collected, with its status, in [debts.md](debts.md).

## The builds of the kernel

The same sources are built three ways; each one is a check of the portability
as much as a product:

| Build | What it produces | Command | Check |
|---|---|---|---|
| host | the unit tests and the simulator, as a Linux process | `make test`, `make sim` | `make ci`, `make smoke` |
| x86-64 | bare metal in QEMU: its own loader, no libc under the kernel | `make x86-build` | `make x86-smoke` |
| ESP32 | the firmware of the board (ESP-IDF v5.3) | `make idf-build` | `tools/device-test.sh` |

A port that does not fit one of them yet is a directory under `ports/`; the
contract of the port is versioned in `kernel/port/info.h` and checked at the
boot. The plan of the board (the ESP32 port) lives with the port, in
[ports/esp32/roadmap.md](ports/esp32/roadmap.md) - it is not part of this
document.

The reports of the old numbered phases are in [phases/](phases/). They were
written when the project was an operating system for one board; the numbers
0-9 map onto the sections below, and the reports stay as history.

## K0 - Portable core (done)

* [x] Boot levels, log with a ring buffer, panic, version, build identity
* [x] Task API and scheduler on the substrate of the port, PIDs, parents
* [x] The syscall ABI (`kernel/ksys.h`), frozen numbers, one door
* [x] VFS with mount points, `/proc`, `/dev`, a descriptor that remembers its access
* [x] `kmm`: slabs for the small blocks, the heap of the port for the large ones
* [x] MicroFS as a consumer of `kernel/blockdev.h`, not of a flash chip
* [x] The libc subset over the ABI

## K1 - The contract of the port (done)

* [x] One header per subject: console, time, memory, locking, power, store,
      sockets, programs; version 1.3, checked at the boot
* [x] Doors for the upper layers: `k_sysinfo`, `k_console`, `k_kv`, `k_net`,
      `k_clock`, `k_render`, `k_mutex`
* [x] `[layers]` and `[port]` in `make integrity`: a leak fails the build
* [x] `ports/port_null` and a probe that links the whole portable system

## K2 - The host machine (done)

* [x] The stubs of a board for the host: the doctor, the commands, the clock,
      a renderer that draws into a file
* [x] `make sim` boots the whole system on a volume of a file, then the shell
* [x] `make smoke` drives it and checks the transcript
* [x] A second bare-metal machine: x86-64 in QEMU (`make x86-smoke`)

## K3 - Programs (done)

* [x] The format of a program: MOX1, a flat image linked for the window of the
      port; the parser checks the header against the image
* [x] The contract of the port for programs (1.3): a machine that cannot run a
      program says so, x86-64 names a window at 32 MiB
* [x] `tools/mkprog.sh`: an image out of an object
* [x] Mode and owner in an inode (9 + 15 bits in the flags), no format change
* [x] Every argument checked at the edge of the syscall table
* [x] The mode enforced at `open`, through `vfs_stat`, and the `x` bit at
      `execve`
* [x] `execve`, `chmod` and `chown` in the syscall table, and in the libc
* [x] A first program in `/bin`: the x86-64 port mounts the MicroFS volume of
      its disk and asks for `/bin/hello` with `execve`; the shell runs the
      same file with `exec`
* [x] Programs are files on a filesystem of the disk, not blobs of the
      kernel: `make x86-disk` writes `/bin` (fifteen programs) and `/etc` with
      `tools/mkfs_mfs.c`, and no boot installs an image
* [x] `PATH` for the shell
* [x] `argv` reaches the port, and the ABI of the entry is written down
* [x] An entry into the kernel from a program (`int $0x80`), with the status
      of a returned program and its arguments
* [x] `mkprog.sh` checks the address a program is linked for against the base
      of the window of the port

## K4 - Protection (done)

* [x] Mode bits and an owner in the file, set by `chmod`/`chown`
* [x] The arguments of a system call checked at the edge
* [x] The permissions enforced at the edge, for the one user of the system, at
      `open` and `execve`
* [x] The `x` bit of the directories of a path is walked and checked
* [x] A real `fork()`: an address space per task, copy on write - the
      milestone of the x86-64 paging. The page tables per task, the copy of
      the tables, the fault of the first write and `k_port_fork_copy()` are
      done on x86-64 (contract 1.5, ADR 0005 and ADR 0007); the board keeps
      the placeholder of a machine without an MMU
* [x] `pipe` (27) and `dup2` (28): an open file is a counted description, the
      two ends of a pipe are ordinary descriptors, and the calls are in the
      ABI and in the library (ADR 0014)
* [x] The shell pipelines and redirections: `cmd | cmd`, `>`/`>>`/`<`/`2>`,
      a task per stage over `task_fork_shared()`, a program that inherits
      the three descriptors of its stage, and Ctrl+C for every stage of the
      job (ADR 0017; `test_kshell_pipe.c`, `make x86-smoke`)

## K5 - A processor with an MMU (in progress)

* [x] Page tables per program on x86-64: the kernel identity map is
      supervisor-only, the window at 32 MiB is user pages over a physical
      arena at 64 MiB, the null page is not mapped and the stack has a guard
      page (ADR 0007)
* [x] Ring 3: a GDT with user code and data, a TSS with `RSP0` and `IST1`, an
      `int $0x80` gate with `DPL 3` and an `iretq` entry; the program prints
      `cpl 3` and `make x86-smoke` checks it
* [x] A fault handler that means something: a page fault, `#UD` or `#GP` of
      ring 3 is printed as `the program faulted: vector N at 0x...`, the
      program is taken down and the shell stays alive
* [x] `exit` of a program ends the task it runs in, and the caller of
      `execve` waits for it; `execve` answers 0 (contract 1.5)
* [x] An errno per task: the TLS block is copied per task and `IA32_FS_BASE`
      follows the task and the privilege boundary
* [x] Page tables per *task* and copy on write: a program is a task with its
      own CR3 and kernel stack, `fork()` copies the tables, both copies are
      read-only until the first write, and `k_port_fork_copy()` is the seam
      of the contract (K4, ADR 0005 and ADR 0007)
* [x] `copy_from_user`/`copy_to_user` at the edge of the system call: the
      port answers whether a range is memory of the caller, a path and `argv`
      are copied into buffers of the kernel, and a bad pointer is `EFAULT`
      instead of a fault of ring 0 (the call of contract 1.7, ADR 0007; `/bin/badptr` in
      `make x86-smoke`)
* [x] NX/W^X: `EFER.NXE` is on from the loader, the kernel text is read-only
      and its rodata, data, bss, stacks and the first gigabyte of devices are
      NX; a program gets a text of its own (read-only) and data, bss, the
      stack and the restorer page with the protection each one needs. The
      port audits its tables at the boot and before every program, and
      `/bin/nx` proves the faults of a fetch, a write and a call into the
      text in `make x86-smoke` (ADR 0007, sections 9 and 10)
* [x] SMAP/SMEP: a decision, not a feature of 1.0 - the default QEMU
      processor does not advertise either, and SMAP needs `STAC`/`CLAC` in
      the portable copy edge. The reasons are in ADR 0007 section 10 and the
      debt in `docs/debts.md`
* [x] Basic signals: `SIGINT`, `SIGTERM`, `SIGCHLD` and `SIGPIPE`, the
      dispositions `SIG_DFL`/`SIG_IGN`, a handler that runs in ring 3 through
      a frame of the port and returns through `sigreturn`, `waitpid()` cut by
      a signal with `EINTR`, `SIGCHLD` on the exit of a child, `kill` in the
      shell and Ctrl+C of the foreground program (`/bin/signals` in
      `make x86-smoke`, ADR 0015). No job control: there are no groups and
      the shell says so.
* [x] The kernel in a higher half: the image is linked at
      `0xFFFFFFFF80000000` and the loader copies it to 16 MiB and maps the
      same bytes at the top, with `-mcmodel=kernel` for the C, `page_phys()`
      as `virtual - KERNEL_BASE + KERNEL_PHYS`, the GDT, the TSS, the IDT and
      the stacks high, and a program's space naming the kernel's high half as
      supervisor-only. The W^X audit walks both halves and `make x86-smoke`
      checks the link address against the ELF (ADR 0007, "The higher half:
      taken")
* [ ] A port for a real ARM with an MMU: the same contract, a second `arch/`
* [ ] `KERNEL_NOMMU` becomes 0 and the concessions become features

## K6 - The filesystem and the network (in progress)

* [x] A second filesystem behind the VFS: the block devices and the MBR/GPT
      partitions are there (ADR 0008), the types are registered and
      mounted by name (`vfs_mount_type`), and FAT12/16/32 is read and
      written with long names (ADR 0009); a volume of `/mnt/...` is mounted
      next to MicroFS in the tests
* [x] FAT32 write: create, write, append, truncate, unlink, mkdir and the
      free-cluster map a write needs; both copies of the table, long names
      made with their checksum, `statfs`; no journal, so a power cut is a
      volume the check can only describe
* [x] `fsck` of FAT, read-only: the boot sector and the BPB, both copies of
      the table, the chains (a loop, a cross-link, a cluster outside the
      volume), the root and the subdirectories, the checksum of the long
      names, the lost clusters and the free count; `fsck fat <point|device>`
      at the console, and a volume beyond 65536 clusters refused with a
      message
* [x] A repair of FAT under `-y`: the lost clusters go back to the free map
      in both copies of the table, a loop or a cluster outside the volume is
      cut, the second of two names of one cluster gives it up, a broken run
      of long names is removed and its 8.3 name kept, and a size that does
      not match its chain is put right; the check runs again afterwards
* [x] Crash consistency of MicroFS: `fsck -y` (or `--repair`) repairs what the
      format itself proves - an orphan block goes back to the free map, an
      entry that names a free record or a name no path can reach is removed,
      an entry whose record disagrees with it about the type takes the type
      of the record, a chain that loops or leaves the data section is cut,
      the second of two objects that share a block gives it up (the first
      owner of the walk of the tree keeps it), a size that does not match its
      chain is put right in both directions, the counter of the entries of a
      directory is counted again, and a record that no name reaches - the
      half-written create or unlink - is freed with its blocks; the check runs
      again afterwards, and the header, the data of a file and a shared
      directory stay `(not repaired)` for a person
* [ ] A journal or a cache of the changes under a write
* [x] ext2, the reader with inodes and a superblock a checker can walk
      (ADR 0019): the superblock, the group descriptors, the inode table,
      the direct blocks and the single, double and triple indirection, the
      directories with names up to 255 bytes, and `stat`/`statx` with the
      mode, the uid and the moments of the inode; read-only, so a write, a
      new name, a mode or an owner is `KERR_NOTSUP`
* [x] `/etc/fstab` and the `mount` command over the type registry: the boot
      mounts the table after the root, `mount <device> <point> <type>` and
      `umount <point>` work at the console, and a line that does not mount is
      reported and skipped
* [x] Timestamps of a file, fed by the clock of the kernel: the record of
      MicroFS has `mtime` and `ctime` in version 2, an old volume keeps its
      16 bytes, and `statx` (26) reports the moments (ADR 0013)
* [x] A page of the state over HTTP: `httpd` on the sockets, `/` and `/health`
* [x] MQTT over the sockets: a 3.1.1 client at QoS 0 and 1, with a fake broker in the tests
* [x] The network on the x86-64 machine: a virtio-net driver (the legacy I/O
      interface, polled, ADR 0008 for the shape of a device of the port) and a
      stack of its own - ARP, IPv4, ICMP, UDP, a client of TCP with one
      segment in flight and a timeout retransmission, and the server half of
      the contract: a table of doors, SYN/ACK, a reset for a port nobody
      listens on and a session that `accept` hands out. `netinfo`, `ping`,
      `netudp` and `dns` come from the port, `http get`, `httpd` and `ntp`
      from `components/net` behind `-DMICRO_OS_X86_NET=ON`, and
      `make x86-net-smoke` checks the echo of the gateway, a UDP exchange, an
      HTTP GET against a server of the host, the page of the state served by
      the guest through `hostfwd`, and a name the resolver answers once
      (10.0.2.15/24 over QEMU user-net, no DHCP)
* [x] DNS with a cache: the resolver of `components/net` (`net/dns.h`) builds
      the query, reads the first A record with its time to live and keeps
      eight names in a cache, and the x86-64 stack asks it for every name, so
      `http`, `httpd` and `mqtt` get the cache behind `k_socket_connect`; the
      board still resolves through lwIP, which has one of its own

## K7 - Quality (done)

* [x] The races of the host port under TSan, in the cloud CI and in `make ci`
* [x] A fuzz of the syscall ABI and of the parsers of the filesystem: MOX1,
      the edge of `exec` and the records of MicroFS have a deterministic seed
* [x] The budget of the image checked at every release (`make integrity`)
* [x] The budget of the internal SRAM checked as well: 128 KiB of static
      DRAM, read from the map of the ESP-IDF build
* [x] The optional features behind compile-time flags (ADR 0010):
      `KERNEL_PSRAM`, `KERNEL_EXEC` and the `MICRO_OS_*` switches of the
      network components, the defaults per build, `make test-minimal` and
      the check of the map
* [x] The shell of the cleanroom (1.1.2): `ls` and `tree` stream a directory
      without a table (300 names, not 64 or 16), `tree -d` counts every entry,
      a path is normalized (`.`/`..`, `//`, a trailing `/`), `memtest`
      repeats an operation and never divides by a still clock, `config` on a
      machine without a store says so, and the words of `write`, `mkdir`,
      `df`, `umount` and the clock are one style

## K8 - Many cores (in progress, after 1.0)

> Not a gate of 1.0 (ADR 0012, updated): the release promises a correct
> single-core kernel; the second core is the first project after it. The
> first phase is in the tree (`KERNEL_SMP`, `struct k_cpu`, the host gate
> and the x86-64 processor list); the AP bring-up is the blocker, named in
> ADR 0012 "The pilot of 1.x".

* [x] A `struct k_cpu` per core and a run queue per core; the scheduler never
      runs one task on two cores (ADR 0012) - `kernel/cpu.h`,
      `components/kernel/src/sched.c`, `tests/cases/test_smp.c`
* [ ] The allocator, the VFS, the console and the block layer safe under two
      cores; the locks that sleep and the locks that spin are named - the
      sleeping locks are audited in [debts.md](debts.md) under K8; the port
      critical section of x86-64 is still `cli` and has to become a machine
      lock before an AP enters the kernel
* [ ] x86-64: AP bring-up, local APIC timer, I/O APIC and the IPI of a TLB
      and a reschedule; `make x86-smoke` with `-smp 2` - the processor list
      (MADT) and `-smp 2` are done; the boot says `2 cpu(s) reported by the
      firmware, 1 in use`; `make x86-smp-build` builds the two-core kernel
* [ ] ESP32: the kernel tasks on both cores of the Pico, no driver assuming
      one core - not started (ADR 0018, the compatibility port)
* [x] `KERNEL_SMP` (ADR 0010) and the host TSan run as the gate - the flag is
      off by default; `make test-smp`, `make test-tsan` and `make soak-host`
      run two cores under TSan, and the plugin into `make ci` is `test-smp`

## K9 - A userland (in progress)

* [x] The entry ABI of a program is `argc`, `argv` and `envp` in `rdi`, `rsi`
      and `rdx`, written down and checked (`make x86-smoke` prints the
      environment of `/bin/hello`) - ADR 0016
* [x] `execve(path, argv, envp)` with the third argument of POSIX; a NULL
      environment is the one of the system (`k_exec_environ()`), and the
      arguments and the environment are copied and bounded apart
* [x] A library of a program, not of the kernel: `components/libc/user` with
      `_start`, the door of `int $0x80`, `open`, `close`, `read`, `write`,
      `lseek`, `readdir`, `getpid`, `uptime`, `exit`, `printf`, `puts` and
      the strings; the numbers come from the header of the kernel
* [x] The programs of `/bin` built from C against the library - `cat`,
      `echo`, `ls`, `spawn` and `wc` - and `make x86-smoke` runs them,
      including a program that starts another with an environment of its own
* [x] Descriptors inherited by `execve`, and a `cat` that reads its stdin:
      the table of the stage is copied into the program before it runs
* [x] `pipe`, `dup2` and a redirection without the help of the kernel: the
      shell of ADR 0017 uses the calls of ADR 0014 (`test_kshell_pipe.c`)
* [x] Basic signals: `SIGINT`, `SIGTERM`, `SIGCHLD` and an interrupted wait,
      and Ctrl+C for every stage of a pipeline (the job of ADR 0017)
* [x] `ls` lists the names of a directory (`-l` with the mode, owner, size and
      date; `-a` with `.` and `..`), and `tree` draws the levels below it
* [ ] A shell `export` and the environment of the shell in the programs,
      instead of the four words of the kernel
* [x] Programs are files on a filesystem of the disk, and a program is added
      by writing a file of the volume (`make x86-disk`), not by embedding it
      in the kernel

## The last major changes of the board (1.1.0)

> The settings of `/etc`, the programs in a partition of the board mapped as
> instructions, and the update with an A/B slot and a rollback are the last
> major changes of the ESP32 port. From `1.2.0` the port is a compatibility
> port (ADR 0018): it builds and passes its test, and receives only the
> changes that are cheap on the board. The notes are
> [`docs/releases/1.1.md`](releases/1.1.md).

* [x] The layout of 1.1 ([ADR 0022](adr/0022-layout-of-1.1.md)): two equal
      application slots of 1.125 MiB, `progs` of 132 KiB and 5.56 MiB of
      MicroFS; the one-time move of a board of 1.0 and its warning in the
      release notes
* [x] A safe update over USB (`tools/ota_usb.py`) with the rollback of a boot
      that never confirms itself, and `DEVICE_OTA=1 tools/device-test.sh` on
      the board
* [x] The programs are files of `progs`, mounted read-only at `/bin`, with no
      image of a program in the firmware (`tools/progs_image.py`)
* [ ] An update over the network into the inactive slot (ADR 0022, open
      item; the USB path is the safe update of 1.1)

## The road to 2.0

> [`docs/releases/2.0.md`](releases/2.0.md) is the direction of the first
> number that may break a contract of 1.0: a machine a person changes from
> the machine itself - settings, programs, services and the system - without
> rebuilding an image. The flags of the build stay build-time. It is a
> direction, not a gate.

## K10 - The settings of a machine (done in 1.1.0)

> The board, the x86-64 machine and the host already kept a few settings as
> files of `/etc`; the screen and the font were the next ones, in a file per
> system (ADR 0021). The feature was not a gate of 1.0.

* [x] A file per system in `/etc` (`display`, `font`, `console`; `time` and
      `net` have room in the table): `key=value`, comments, a bounded parser
      in `components/settings` (12 pairs, 127 characters, keys of
      `[a-z0-9_.-]`), a key the system does not know kept and reported once,
      a value that does not parse replaced by the default and a warning
* [x] The store keeps the text of a system file, so a format brings the
      settings back as it brings the other files of `/etc`; a system file is
      at most 127 characters because it is one value of the store
      (`tests/cases/test_settings.c` proves the round trip)
* [x] An applier per system, called at the boot in the order of the machine:
      the display and the font before the first frame, the console before the
      shell; the board loads them in the init task, x86-64 in `kmain()`
* [x] `config set`, `config get`, `config list` and `config sync` over a
      system file, and a setting a machine cannot honour kept and ignored with
      a warning (`SETTINGS_KEPT`)
* [x] The screen and the font: rotation, brightness, inversion, the scale of
      the 5x7 font and the theme; the VGA of x86-64 where it can (the
      attribute of the cells), the panel of the board everywhere
* [x] A table of the keys per machine in the wiki
      ([settings.md](wiki/settings.md)), with a host test that every key of a
      default file is a key of its system, so a key is applied or named

The next steps that stay open are the ones of the machine and not of the
format: a backlight with a duty cycle for `brightness`, an applier on the host
when its screen file is a real screen, and a rewrite of a file that is atomic
on a VFS without `rename` (see [debts.md](debts.md)).

## K11 - The screen of x86-64 (1.2)

> The port keeps the text mode of the firmware: 80 columns by 25 rows at
> `0xB8000`, sixteen colours, no font and no picture. The next screen is a
> linear framebuffer, not a GPU (ADR 0023); a 3D driver of a real card is
> not promised, and UEFI GOP is the later door to a physical machine.

* [ ] A linear framebuffer through Bochs VBE in QEMU: 1280x800, 16 bits per
      pixel, RGB565 so `k_renderer` draws into it unchanged; the PCI device
      is found over the configuration space the network port already reads,
      and `paging.c` maps its window
* [ ] The console of the screen: cells of the 5x7 font scaled twice (about
      128 columns by 57 rows), the control characters, the cursor, the
      scrolling and the two sequences of `clear`; the colours of
      `/etc/console` (ADR 0021); the keyboard of IRQ1 unchanged
* [ ] The text mode stays the fallback, improved once to 80x50 with the 8x8
      font of the VGA; the port chooses the screen at the boot
* [ ] `make x86-vga-smoke` reads the framebuffer with `screendump` (a
      picture has no text memory for `pmemsave`) and checks cells and
      colours; `make x86-run` opens it for a person
* [ ] Later: a 2D surface for a program, and optionally virtio-gpu 2D in
      QEMU; UEFI GOP for a real machine, which is a loader of its own

The desktop is not a step of this list. A picture, a mouse and a compositor
are the parts of one, and the compositor has to be a program of the system
and not the kernel (see [releases/2.0.md](releases/2.0.md)); the first thing
to build is the screen a program can draw on.

## K12 - The stream and the installer (1.2, in parallel with K11)

> The programs are a repository of their own (`microos-progs`); the machine
> that installs one needs a stream, TLS and a signature (ADR 0024). The board
> has no installer - its `progs` belongs to the firmware (ADR 0018) - so this
> is the work of x86-64 and of the host substrate.

* [ ] A streaming HTTP client: `Content-Length` and a chunked body, a
      bounded number of redirects, a timeout, and a sink that is a
      `struct vfs_file` or the inactive slot of an update; `wget URL FILE`
      at the console and a `Range` resume for a file
* [ ] mbedTLS on x86-64 with a freestanding platform layer: `kmm` for the
      allocations, `RDRAND` for entropy (the timer as the fallback), NTP for
      the clock and `/etc/ssl/certs.pem` through the VFS; the board keeps the
      copy of ESP-IDF
* [ ] The signed index and a signed `.mox`: `index.json` with the name, the
      version, the machine, the ABI, the size and the digest; the verifier
      reuses the Ed25519 of ADR 0020
* [ ] `pkg update/install/remove/list/info` over `/var/pkg` and `/usr/bin`,
      atomic through a new `vfs_rename` (MicroFS and FAT implement it; a
      volume without it installs with a warning)
* [ ] The interface of the console is pacman's, the build is not: `-S`, `-R`,
      `-Q`, `-Sy`, `-Su`, `-Syu`, `-U`, `--noconfirm`, `--needed`,
      `--download-only`, `--dry-run`, `--allow-downgrade`, `--offline`,
      `--root`, `--repo`, `--color`, `NAME=VERSION` (ADR 0024 §6)
* [ ] The repository `microos-progs`: CI builds a program per machine, signs
      it and publishes the index with the artifacts; `make` in the kernel
      tree keeps only the programs of the tests
* [ ] Tests: the fake transport of `components/net` serves a chunked body, a
      redirect and a dropped connection; the installer is tested on a
      temporary volume with a bad signature, a wrong machine and a downgrade;
      `make x86-net-smoke` fetches a file from a local server
