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
* [x] A first program in `/bin`: the x86-64 port writes `/bin/hello` to its
      RAM volume at the boot and asks for it with `execve`; the shell runs the
      same file with `exec`
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
      instead of a fault of ring 0 (contract 1.6, ADR 0007; `/bin/badptr` in
      `make x86-smoke`)
* [ ] The kernel in a higher half: the plan of the loader, the linker script
      and `page_phys()` is written down in ADR 0007, not taken
* [ ] A port for a real ARM with an MMU: the same contract, a second `arch/`
* [ ] `KERNEL_NOMMU` becomes 0 and the concessions become features

## K6 - The filesystem and the network (in progress)

* [x] A second filesystem behind the VFS: the block devices and the MBR/GPT
      partitions are there (ADR 0008), the types are registered and
      mounted by name (`vfs_mount_type`), and FAT12/16/32 is a read-only
      reader with long names (ADR 0009); a volume of `/mnt/...` is mounted
      next to MicroFS in the tests
* [ ] FAT32 write: create, write, unlink, and the free-cluster map a write needs
* [ ] ext2, the reader with inodes and a superblock a checker can walk
* [ ] `/etc/fstab` and the `mount` command over the type registry
* [ ] Timestamps of a file, fed by the clock of the kernel
* [x] A page of the state over HTTP: `httpd` on the sockets, `/` and `/health`
* [x] MQTT over the sockets: a 3.1.1 client at QoS 0 and 1, with a fake broker in the tests

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

## K8 - Many cores (planned)

* [ ] A `struct k_cpu` per core and a run queue per core; the scheduler never
      runs one task on two cores (ADR 0012)
* [ ] The allocator, the VFS, the console and the block layer safe under two
      cores; the locks that sleep and the locks that spin are named
* [ ] x86-64: AP bring-up, local APIC timer, I/O APIC and the IPI of a TLB
      and a reschedule; `make x86-smoke` with `-smp 2`
* [ ] ESP32: the kernel tasks on both cores of the Pico, no driver assuming
      one core
* [ ] `KERNEL_SMP` (ADR 0010) and the host TSan run as the gate
