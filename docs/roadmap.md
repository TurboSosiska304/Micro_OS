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
      kernel: `make x86-disk` writes `/bin` (thirty-one programs) and `/etc` with
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
* [x] The permissions enforced at the edge against the uid of the task, at
      `open` and `execve`; the credentials of a task (calls 36 to 40) and
      root that passes the triads of read and write
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
      job (ADR 0017; `test_kshell_pipe.c`, `make x86-smoke`). The stages are
      no longer three: the parser holds sixteen, the pipes are made one
      boundary at a time as the stages are forked, and `make x86-smoke`
      checks a line of ten stages.

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
      `execve` waits for it; `execve` answers 0 (contract 1.5). Since M1 of
      the 1.5 plan a program's `execve` replaces the image of the caller
      instead (POSIX), the child of a `fork` is a real process with its own
      status, and `waitpid(-1)` waits for any child; the caller without an
      address space (the shell) keeps the task-and-wait shape.
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
      (`docs/plan-arm64.md`; a **later release**, no date - the plan is written
      and the work is not started)
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
* [x] MicroFS 1.7 ([ADR 0038](adr/0038-a-table-of-records.md)): the `META`
      section is the map of the free blocks and the table of the records and
      nothing else, the table is sized by the records and grows into the data
      section when its reserve runs out, every record of a version 3 volume
      carries CRC32 of itself - so a write that a power cut stopped is named
      by `fsck` and repaired - and the record holds the group of the object,
      which `statx` answers; a volume of version 1 and 2 mounts and writes as
      it did
* [ ] A journal or a cache of the changes under a write
* [x] ext2, the reader with inodes and a superblock a checker can walk
      (ADR 0019): the superblock, the group descriptors, the inode table,
      the direct blocks and the single, double and triple indirection, the
      directories with names up to 255 bytes, and `stat`/`statx` with the
      mode, the uid and the moments of the inode; and the write side of 1.4
      (ADR 0029): the bitmaps of a group, `create`, `write` through the
      direct blocks and one table, `truncate`, `unlink`, `mkdir`, `chmod`,
      `chown`, and `ext2_check` for the counters and the chain of the
      directories
* [x] `/etc/fstab` and the `mount` command over the type registry: the boot
      mounts the table after the root, `mount <device> <point> <type>` and
      `umount <point>` work at the console, and a line that does not mount is
      reported and skipped
* [x] Timestamps of a file, fed by the clock of the kernel: the record of
      MicroFS has `mtime` and `ctime` in version 2, an old volume keeps its
      16 bytes, and `statx` (26) reports the moments (ADR 0013)
* [x] A page of the state over HTTP: `httpd` on the sockets, `/` and `/health`
* [x] A server of files (1.6, [ADR 0036](adr/0036-a-server-and-an-update.md)):
      `httpd serve [port] [root]` answers GET and HEAD over HTTP/1.1 with
      keep-alive, `Content-Length`, the type of the extension, `ETag` and
      `304`, one `Range` (`206`/`416`), a listing of a directory as a chunked
      page and a line of the log per request; a `..` that leaves the root is
      `403`, a directory is never a file, and a session over the ceiling of
      the machine is `503`. Three sessions on x86-64, three on the board,
      sixteen on the host
* [x] The serving side of TLS (`httpd tls`, port 443): the vendored mbedTLS
      with `MBEDTLS_SSL_SRV_C`, the certificate and the key of `/etc/ssl`
      read through the VFS, and `curl -k` against the guest in
      `make x86-net-smoke`; the board has no TLS on either side (the flash of
      its slot, [debts.md](debts.md))
* [x] An update over the network (1.6, ADR 0022 §7): `ota fetch URL`
      downloads a signed image, checks the trailer and the Ed25519 signature
      **before** a byte of the inactive slot is written, selects the slot and
      asks for a reboot; a slot that never confirms itself is rolled back, in
      the file form of x86-64 and in otadata on the board. `ota status`,
      `ota boot` and `ota confirm` are the state machine at the console
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

## K8 - Many cores (x86-64 and host done in 1.3, the guard page open)

> Not a gate of 1.0 (ADR 0012, updated), and not a part of `1.2.0` either:
> the screen, the stream and the installer were that release. `1.3.0` closes
> the pilot: the machine of QEMU starts both processors, runs tasks on each of
> them, passes `make x86-smp-smoke` (34 checks) and the 600-second
> `make soak-smp` session. `KERNEL_SMP` stays off by default. 1.6 took the
> I/O APIC and the wake IPI (ADR 0034); the open rows are the stack guard page
> and the board, and they are the K8 rows of [debts.md](debts.md). The
> floating point state of a task is saved and loaded at every switch since K9
> (below).

* [x] A `struct k_cpu` per core and a run queue per core; the scheduler never
      runs one task on two cores (ADR 0012) - `kernel/cpu.h`,
      `components/kernel/src/sched.c`, `tests/cases/test_smp.c`; the claim of
      the next task (`k_sched_claim_next()`) and the refusal of a task that
      is the current of two cores are tested
* [x] The big kernel lock is a spinlock of the machine, and the per-core
      state of the port (GDT/TSS/IST, RSP0, the thread pointer, the current
      context, `CR0.WP`) is in `ports/port_x86_64/src`
* [x] x86-64: the AP bring-up (the MADT list, the trampoline at `0x7000`,
      INIT-SIPI-SIPI, a stack and a table per core), the local APIC timer of
      every AP, the IPI vector; `make x86-smp-build`, `make x86-smp-run`,
      `make x86-smp-smoke` and `make soak-smp`
* [x] The console is one resource of the machine: `k_port_write()` holds the
      big kernel lock for the whole call
* [x] x86-64: the I/O APIC (the timer and the keyboard off the 8259, the
      MADT overrides, the LAPIC EOI) and the wake IPI of the scheduler, with
      the claim that preempts by priority - ADR 0034, `make x86-smp-smoke`
      (52 checks)
* [ ] A guard page under a task stack - the K8 row of
      [debts.md](debts.md)
* [ ] The allocator, the VFS, the console and the block layer under an
      aggressive migration: they are sleeping locks and the two-core session
      runs the full load for ten minutes, but no test migrates a task in the
      middle of a filesystem operation
* [ ] ESP32: the kernel tasks on both cores of the Pico, no driver assuming
      one core - not started (ADR 0018, the compatibility port)
* [x] `KERNEL_SMP` (ADR 0010) and the host TSan run as the gate - the flag is
      off by default; `make test-smp`, `make test-tsan` and `make soak-host`
      run two cores under TSan

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
* [x] A third-party library under a program: PCRE2 10.42 in
      `third_party/pcre2`, the 8-bit interpreter without JIT and Unicode,
      linked into `/bin/pcre2grep`; the image is 61,336 bytes of the 64 KiB cap and
      the library's own test is `tests/cases/test_pcre2.c` (the Unicode and
      the transfer limit are in `docs/debts.md`)
* [ ] A shell `export` and the environment of the shell in the programs,
      instead of the four words of the kernel
* [x] Programs are files on a filesystem of the disk, and a program is added
      by writing a file of the volume (`make x86-disk`), not by embedding it
      in the kernel
* [x] The library of a program learned the three families a portable source
      expects: an arena of its own (`malloc`/`free`, the bss of the image, not
      `KSYS_MALLOC`), `errno` and `strerror`, and the streams of `fopen`; the
      shims of `<string.h>`, `<stdlib.h>`, `<stdio.h>`, `<errno.h>`,
      `<unistd.h>`, `<fcntl.h>` and `<sys/types.h>` are in
      `components/libc/user/include`
* [x] The first port of a program of somebody else: zlib 1.3.1 as a static
      archive built by the compiler of the window (`cmake/userland.cmake`,
      `third_party/zlib/port/`) and `/bin/zcat` on a `.gz` of MicroFS. The
      recipe is [`porting-programs.md`](porting-programs.md), the library has
      `tests/cases/test_zlib.c`, and `make x86-smoke` and
      `make x86-disk-smoke` check the inflated bytes of a fixed fixture. Lua,
      sbase and SQLite are the next copies of the recipe
* [x] The second port of a program of somebody else: Lua 5.4.7 (MIT) as a
      static archive built by the compiler of the window and `/bin/lua`, with
      the script `/etc/hello.lua` and `/etc/licenses/lua` in the image. The
      library of a program learned the last pieces a language asks for -
      `%f`/`%e`/`%g` with a precision, `strtod`, `qsort`, `setjmp`/`longjmp`,
      `math.h` on the x87 unit, `time`/`localtime`, `getenv`, `signal` and
      `fprintf` - and the port sets `CR4.OSFXSR` for a program with a
      `double`. The host test is `tests/cases/test_lua.c`; `make x86-smoke`
      runs the interpreter, and `make x86-disk-smoke` runs it out of the
      volume in both boots
* [x] The floating point state of a task: the port saves and loads the XMM
      and x87 image of a context at every switch (`fxsave64`/`fxrstor64` in
      `ports/port_x86_64/src/port_x86_64_task.c`), a fork copies the live
      state of the parent, an `execve` resets the unit after `fninit`, and
      the trampoline of an AP sets the bits of CR4 and CR0 the core needs.
      `/bin/fpu2` is the gate: three tasks with a `double` at once keep their
      own registers, a fourth dirties the unit and `execve`s the clean one,
      and `make x86-smoke` and `make x86-smp-smoke` read every line
* [x] The image of a program larger than `KSYS_MAX_IO`: `execve` reads an
      image of up to 8 MiB (`K_EXEC_MAX_IMAGE`) in pieces of the transfer
      edge into one block of the allocator, and a larger file is `E2BIG`
      before a byte is read (`docs/debts.md`)
* [x] The third port, and the first set of commands: sbase with ten tools
      (`cat`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`, `xargs`,
      `sed`) under their canonical names in `/bin`, in place of the small
      programs they replace. It is the port that grew the library of a
      program the families a POSIX command needs (`opendir`/`readdir`,
      `struct stat`, `getpwuid`, `localtime`, `fnmatch`, a regular expression
      engine, `getopt`, `getline`, `fmemopen`), and the ten commands are in
      `make x86-smoke` and `make x86-disk-smoke`
* [x] The fourth port: bc 7.2.0 (BSD-2-Clause) as a static archive and the
      two names `/bin/bc` and `/bin/dc` of the same engine, with the scripts
      `/etc/bc.bc` and `/etc/dc.dc` and `/etc/licenses/bc` in the image. The
      release is configured by a force-included `port/bc_port.h` (no history,
      no NLS, 64-bit limbs) and its two POSIX calls are answered by the port
      layer (`fstat` of a descriptor and `sigaction`). The library of a
      program learned `strdup` and `srand`/`rand`; the host test is
      `tests/cases/test_bc.c` and the smoke tests run the calculator and the
      RPN one before and after a reset of the disk. The image is 115 KiB and
      its arena is 768 KiB of the bss
* [x] The eighth port: the One True Awk of 2024-07-28 (the Lucent permissive
      license) as `/bin/awk`, with the data `/etc/columns`, the script
      `/etc/hello.awk` and `/etc/licenses/awk` in the image. The grammar is
      generated once by byacc and committed with its header, the port layer
      (`third_party/awk/port/awk_port.h`, `awk_os.c`) answers the calls of the
      C library the release wants and the library does not have, and
      `system` is an honest refusal: this machine has no shell and no second
      process, and the `popen` of a pipe is the library's `ENOSYS`. `tests/cases/test_awk.c` runs the `main` of the release
      through thirty-two cases, and the smoke tests run the interpreter in
      the image and out of the volume after a reset
* [ ] The gaps of the next port: a `malloc` larger than the bss of an image
      (`brk`/`mmap` of a program), a `poll` for a library that waits on a
      pipe, and the package that ships `zcat`, `lua` and the tools of sbase
      with their licenses beside them (`docs/debts.md`)
* [x] The seventh port, a library with an operating system of its own:
      SQLite 3.54.1 with `SQLITE_OS_OTHER=1` and the VFS of the port on the
      calls of the system (`third_party/sqlite/port/sqlite_vfs.c`), and
      `/bin/sqlite3` (the shell of the release) over a database of MicroFS. It
      is the first program with a `REAL`: the `FLOAT` option of the recipe
      leaves `-mgeneral-regs-only` out and the loader turns the SSE and x87
      units on. The image is 846,080 bytes (826 KiB), `tests/cases/test_sqlite.c`
      is the
      host, and the smoke tests create, read and check a database across a
      reset of the disk
* [ ] A `chdir` and a wall clock in the ABI: the library works around the two,
      and the workaround of each is named in `docs/debts.md`
* [x] The POSIX frame of a fork child: the child of `fork()` comes back from
      the call with 0 in RAX and a stack page of its own, `execve` from a
      program replaces the image of the caller, and `waitpid` reaps it (1.5)
* [x] The fifth port: zstd 1.5.7 (BSD-3-Clause) as a static archive of
      lib/common, lib/compress and lib/decompress, and `/bin/zstd` as the
      program of the release itself (`programs/zstdcli.c`), with
      `/bin/zstdcat` as the same image under a second name and
      `/etc/setting.zst` as a fixture; `/etc/licenses/zstd` travels in the
      image. The port caps the memory of the context to the arena of a
      program (`third_party/zstd/README.md`), and the frames of the guest and
      of the host are read by one another in `make x86-smoke`; the host test
      is `tests/cases/test_zstd.c`
* [x] The sixth port, and the first database: file 5.45 with libmagic
      (`third_party/file`, BSD-2-Clause) as `/bin/file`, with a compact
      database of the Magdir rules (`tools/magic_subset.py`, the subset of
      `third_party/file/magic/subset.json`) compiled into `/etc/magic.mgc`
      and the text `/etc/magic` as the fallback. It is the port that asked
      for an image past the transfer edge (`K_EXEC_MAX_IMAGE`), the first
      whose data does not fit (`magic.mgc` is 359 KiB), and the first whose
      `-z` names the content of a gzip and of a zstd frame;
      `tests/cases/test_file.c`, `make x86-smoke` and `make x86-disk-smoke`
      cover the values, the program and the volume after a reset
* [ ] The port of 1.3.0 still open: awk (the One True Awk and its C library),
      whose branch is in progress and is not a promise before it is merged.
      SQLite landed in this release; a package of it needs `PKG_PACKAGE_MAX`
      above 1 MiB (`docs/debts.md`)

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
* [x] An update over the network into the inactive slot (ADR 0022 §7,
      ADR 0036): the signed image of an HTTPS server is checked before it
      reaches the slot, the slot is selected and the boot that does not
      confirm itself is rolled back. The board's flash path is built and
      waits for a session with the board; the USB path of 1.1 stays the safe
      update of a board in the field ([debts.md](debts.md))

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
      the 5x7 font and the theme; the panel of the board everywhere, and on
      x86-64 the pair of the framebuffer and the gap between its lines
      (`font.line_gap`)
* [x] A table of the keys per machine in the wiki
      ([settings.md](wiki/settings.md)), with a host test that every key of a
      default file is a key of its system, so a key is applied or named

The next steps that stay open are the ones of the machine and not of the
format: a backlight with a duty cycle for `brightness`, an applier on the host
when its screen file is a real screen, and a rewrite of a file that is atomic
on a VFS without `rename` (see [debts.md](debts.md)).

## K11 - The screen of x86-64 (done in 1.2.0)

> The port keeps the text mode of the firmware: 80 columns by 25 rows at
> `0xB8000`, sixteen colours, no font and no picture. The next screen is a
> linear framebuffer, not a GPU (ADR 0023); a 3D driver of a real card is
> not promised, and UEFI GOP is the later door to a physical machine.

* [x] A linear framebuffer through Bochs VBE in QEMU: 1280x800, 16 bits per
      pixel, RGB565 so `k_renderer` draws into it unchanged; the PCI device
      is found over the configuration space the network port already reads,
      and `paging.c` maps its window
* [x] The console of the screen: cells of the 8x16 font of the PC (Spleen
      8x16, 160 columns by 50 rows; `font.line_gap` of `/etc/font` widens a
      line to 18 or 20 pixels, 44 or 40 rows), the control characters, the
      cursor, the scrolling and the two sequences of `clear`; the colours of
      `/etc/console` (ADR 0021); the keyboard of IRQ1 unchanged
* [x] A colour per cell: the sixteen of the CGA in RGB565, `(paper << 4) |
      ink` on every cell of the grid and the attribute byte of the text mode,
      the SGR sequences of a terminal (`0`, `1`, `22`, `7`, `27`, `30`-`37`,
      `39`, `40`-`47`, `49`, `90`-`97`, `100`-`107`), the levels of the log
      (warn amber, error red, success green), `console.ink`/`console.paper`
      of `/etc/console` (a name or `0xRRRR`) and `ls --color`; the serial
      line stays plain
* [x] The text mode stays the fallback; the port chooses the screen at the
      boot. The 80x50 mode with the 8x8 font of the VGA is not built - the
      fallback is the screen of a machine without an interface, and the font
      load and the CRTC timings are a debt of their own
      ([debts.md](debts.md), K11)
* [x] `make x86-vga-smoke` reads the framebuffer with `screendump` (a
      picture has no text memory for `pmemsave`) and checks cells and
      colours, and keeps the `pmemsave` checks of the text mode on a card
      without the interface; `make x86-run` opens it for a person
* [x] A picture a program draws on (ADR 0028, ADR 0030): `/dev/fb0` and
      `/dev/input0`, the window model, the canvas and the keyboard of
      `components/ui`, the arrows of the E0 prefix as keys of the machine, and
      the programs `/bin/ui`, `/bin/edit` and `/bin/fm`
* [ ] Later: a 2D surface for a program is there and a mouse is not; a
      compositor, optionally virtio-gpu 2D in QEMU; UEFI GOP for a real
      machine, which is a loader of its own

A desktop is not a step of this list. A picture a program draws on arrived in
1.4.0, the programs of the screen in 1.5.0 and a mouse is the next device
(see [releases/2.0.md](releases/2.0.md)); the compositor stays a program of
the system and not the kernel.

## K12 - The stream and the installer (done in 1.2.0)

> The programs are a repository of their own (`microos-progs`); the machine
> that installs one needs a stream, TLS and a signature (ADR 0024). The board
> has no installer - its `progs` belongs to the firmware (ADR 0018) - so this
> is the work of x86-64 and of the host substrate.

* [x] A streaming HTTP client: `Content-Length` and a chunked body, a
      bounded number of redirects, a timeout, and a sink that is a
      `struct vfs_file` (a callback); `wget URL FILE` at the console and a
      `Range` resume for a file. The inactive slot of an update is the same
      stream and is not wired yet: the update of the board is over USB
      (ADR 0022) and there is no `esp_ota_*` writer in the tree
      ([debts.md](debts.md))
* [x] mbedTLS 3.6.2 on x86-64 with a freestanding platform layer:
      `third_party/mbedtls` with a client-only configuration, `kmm` for the
      allocations, `RDRAND` for entropy (the timer as the fallback), NTP for
      the clock and `/etc/ssl/certs.pem` through the VFS; `http_parse_url`
      takes `https`, the stream wraps the plain transport, and `wget`,
      `http` and `pkg` speak HTTPS with the chain, the name and the dates
      checked. A machine of 1970 is a refusal with a sentence. The board
      keeps the copy of ESP-IDF and its shell has no command of TLS yet
      (ADR 0024 §3, [debts.md](debts.md))
* [x] The signed index and a signed `.mox`: `index.json` with the name, the
      version, the machine, the ABI, the size and the digest; the verifier
      reuses the Ed25519 of ADR 0020
* [x] `pkg update/install/remove/list/info` over `/var/pkg` and `/usr/bin`,
      atomic through a new `vfs_rename` (MicroFS and FAT implement it; a
      volume without it installs with a warning). A directory is renamed
      only inside its parent yet (see debts.md)
* [x] The interface of the console is pacman's, the build is not: `-S`, `-R`,
      `-Q`, `-Sy`, `-Su`, `-Syu`, `-U`, `--noconfirm`, `--needed`,
      `--download-only`, `--dry-run`, `--allow-downgrade`, `--offline`,
      `--root`, `--repo`, `--color`, `NAME=VERSION` (ADR 0024 §6)
* [x] The repository `microos-progs`: a public tree with a signed index at
      <https://turbososiska304.github.io/microos-progs/>, the key
      `microos-progs.pub` and the artifacts the index names; the kernel tree
      keeps only the programs its tests and its `bin` need. The CI of the
      repository and the packages of the programs with their licenses are
      the next pass ([debts.md](debts.md))
* [x] Tests: the fake transport of `components/net` serves a chunked body, a
      redirect and a dropped connection; the installer is tested on a
      temporary volume with a bad signature, a wrong machine and a downgrade;
      `make x86-net-smoke` fetches a file from a local server and installs it,
      and serves three certificates of a test authority over TLS: the
      trusted one arrives (1.3 and 1.2), the one of another name and the one
      of another authority are refused, and the repository of `pkg` is HTTPS

## K13 - A bootable image of x86-64 (prototype, done in 1.2.0)

> The machine of the port boots in QEMU from `-kernel` and `-initrd`: the
> hypervisor is the boot loader. A medium is the other half of "a machine":
> a firmware reads it, a boot loader starts the port's loader, and the root
> filesystem comes with the image. The decision is
> [ADR 0026](adr/0026-bios-iso.md), the page is
> [ports/x86_64/iso.md](ports/x86_64/iso.md).

* [x] An El Torito ISO (BIOS) with the loader, the signed kernel and the
      root image: `make x86-iso` writes `build-x86-iso/microos.iso` with
      GRUB when `grub-mkrescue` is installed and with limine otherwise; the
      boot loader is not vendored
* [x] The root image as the second module of the boot: the port registers
      its bytes as a read-only block device (`boot0`, `boot0p1`), the boot
      mounts it at `/` and `/bin` is the tree of the CD; the heap starts
      above the modules, and the loader refuses a module over the kernel
* [x] `make x86-iso-smoke`: the ISO boots with `-cdrom` and nothing else,
      mounts the root image and runs `/bin/hello` from the boot and from the
      shell
* [ ] UEFI: the EFI application, the memory map of the firmware, GOP and the
      exit of the boot services; the framebuffer of ADR 0023 is here, so the
      screen a GOP hands over is the next door to a physical machine, and
      the loader for it is not written yet
* [ ] A hybrid image: the ISO written to a USB stick boots as a disk
      (`limine bios-install`, or a protective MBR and the GRUB record)
* [ ] A writable root: the root of the CD is read-only, and a person's files
      need a disk (or an overlay in the memory of the machine) and the
      installer of K12
* [ ] A reproducible ISO: the volume descriptor follows
      `SOURCE_DATE_EPOCH`, the directory records still carry the clock

## The release of 1.2.0

> The notes are [`docs/releases/1.2.md`](releases/1.2.md); the page of the
> wiki is [`wiki/release-1.2.md`](wiki/release-1.2.md). The board is the
> compatibility port of ADR 0018, and the second core is `1.3.0`.

* [x] K11: the framebuffer of 1280x800, the Spleen 8x16 console, the colour
      of a cell and SGR, the text mode as the fallback (ADR 0023)
* [x] K12: the stream and `wget`, the signed index and `pkg`, mbedTLS and
      HTTPS (ADR 0024)
* [x] K13: the BIOS ISO with the read-only root module (ADR 0026)
* [x] `make x86-user`: the tar of the release is the machine that runs
* [x] The ports: zlib, sbase as the canonical `/bin`, PCRE2, Lua and bc/dc,
      the 8 MiB ceiling of an image, and the repository `microos-progs`
* [x] The shell fixes of `1.1.2` (a directory without a table, `.` and `..`
      as a path)
* [ ] The gates of the release: the three sessions of 600 seconds, run after
      the integration of the branches (the table in the notes)
