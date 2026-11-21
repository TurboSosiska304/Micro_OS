# Debts of Micro OS

This is the single register of what the tree knows it does not do. A debt is a
gap between what a document promises and what the code does, or a limitation
with a next step that somebody can take. A decision is not a debt: the choices
that are final are listed in their own section, so that a reader does not try
to "fix" them.

The sources of the register are [roadmap.md](roadmap.md) (K3-K7),
[phase7.md](phases/phase7.md) ("What is still missing"),
[ports/esp32/README.md](ports/esp32/README.md) and
[ports/esp32/roadmap.md](ports/esp32/roadmap.md) (P3-P5), the other port
READMEs, [ADR 0005](adr/0005-programs-and-protection.md),
[README.md](../README.md) ("Known limitations"), [libc.md](libc.md),
[memory.md](memory.md) and [syscall-abi.md](syscall-abi.md).

| Status | Meaning |
|---|---|
| `open` | Known, not started; the next step is written in the last column. |
| `in work` | Somebody is changing the tree for it now. |
| `decision` | Deliberate, not a debt; the reason is in "Decisions, not debts". |

A debt is closed by code and a test, not by deleting its row: when it is done
the row moves to the release notes of the phase that did it.

## K3 - Programs

| Debt | Where | Status | What it needs |
|---|---|---|---|
| `stat` reports the mode and the owner to anybody and checks nothing. | `components/kernel/src/ksys.c`, `docs/phases/phase7.md` | decision | None: POSIX makes `stat` readable, and the check stays at `open` and `execve`. |
| `ESRCH` has no number: `-3` is `KERR_EXIST` in this ABI, and renaming it would break the frozen table. `waitpid` answers `ECHILD`. | `components/kernel/include/kernel/errno.h` | decision | None: a new number for `ESRCH` would not be POSIX either; a program that needs it reads `ECHILD`. |
| The library does not know the two new codes or the directory flag: `microos_strerror` says `unknown error` for `ENAMETOOLONG`/`ECHILD`, and `map_flags` does not pass `O_DIRECTORY`. | `components/libc/src/unistd.c` | open | The names of the two codes and the flag in the library, with a test of `open` of a directory. |
| The `x` bit of the directories of a path is not walked, so a directory of mode `0600` still lets its files be opened by their full path. | `components/mfs/src/mfs_vfs.c`, the VFS lookup | open | Check every directory on the way down, with its own tests. |
| The image is read whole into one block of `kmm` (up to `KSYS_MAX_IO`, 64 KB) before the port copies it to the window: two copies in RAM at once. | `components/kernel/src/ksys.c` (`sys_execve`), `components/kernel/src/exec.c` | open | Stream or map the image, or cap it against the memory of the machine and not only against the transfer edge. |

Closed in the same pass: the `int $0x80` entry and its ABI, `argv` through
`k_exec_run()`, `PATH` and `stat` in the shell, the test of the padding of
`mkprog.sh`, its check of the linked address, the walk of the x bit, the
directory descriptor for `readdir`, `ENAMETOOLONG`/`ECHILD`, and the entry
from ring 3 with its kernel stack, the fault handler, `exit` and the errno
of a task (K5). The row that stays above is the part of the entry that waits for
K5.

## K4 - Protection

| Debt | Where | Status | What it needs |
|---|---|---|---|
| `fork()` on a machine without an MMU is still a placeholder: the child shares the memory of the parent and re-enters its program. On x86-64 the call is real - an address space per task, copy on write and `k_port_fork_copy()` (contract 1.5) - and the board keeps the placeholder because it has no MMU. | `components/kernel/src/ksys.c`, ADR 0005, ADR 0007 | done | An MMU on the board, or the same placeholder for as long as it has none. |
| `KERNEL_NOMMU` is still 1 everywhere: the x86-64 port protects a program of ring 3, but the generic kernel does not use the MMU for a task and the board has none. | `components/kernel/include/kernel/config.h`, `ports/port_x86_64` | open | Page tables per task and the flag turned off for the ports that have an MMU. |
| The owner is stored and checked but there is only one user, uid 0, so groups and a second party are not part of the model. | ADR 0005, `components/kernel/src/ksys.c` | decision | None until a second user exists. |
| The owner of the system gets no exemption of root. | ADR 0005, the mode check | decision | None: this is the rule, not a missing feature. |
| `chmod 000` cannot be expressed: a zero mode means "the record predates the modes" and is refused instead of stored. | `docs/memory.md` section 6, `components/mfs/src/mfs.c` | decision | None without a format change; a real zero needs a bit that says "the mode is set". |

## K5 - A processor with an MMU, and the x86-64 port

| Debt | Where | Status | What it needs |
|---|---|---|---|
| An address space per *task*: the page tables are copied for a fork and the pages are copy-on-write, and a program is a task with a CR3 of its own (K4, ADR 0007). | `ports/port_x86_64/src/paging.c`, `ports/port_x86_64/src/port_exec.c` | done | Nothing on x86-64; the kernel is still not in a higher half. |
| The pointers of a program are checked and copied at the edge: `k_port_exec_contains()` (contract 1.6) walks the page tables, the paths and `argv` are copied into the kernel, and a bad range is `EFAULT` (`/bin/badptr` in `make x86-smoke`). There is no SMAP, so the check and the copy use the same page tables. | `components/kernel/src/ksys.c`, `ports/port_x86_64/src/port_exec.c`, ADR 0007 | done | SMAP/SMEP is the hardware half of the rule, with the higher half. |
| The kernel is still in the low identity map of the first gigabyte, not in a higher half; the window of the programs is a hole in the kernel's map. | `ports/port_x86_64/linker64.ld`, `loader32.S`, `src/paging.c`, ADR 0007 | open | The five steps of "The higher half: a plan, not taken" in ADR 0007 (linker, loader, `page_phys()`, boot/stacks, smoke gate). |
| No guard page under the stacks of *kernel tasks* (the stack of a program has one): a kernel stack that overflows walks into the heap of the port. | `ports/port_x86_64/src/port_x86_64_task.c` | open | An unmapped page at the bottom of every kernel stack, and the page tables of a task to put it in. |
| No IST for the machine check and the NMI: only the double fault has a stack of its own (`IST1`), so a fault in those handlers can still become a triple fault. | `ports/port_x86_64/src/interrupts.S`, `ports/port_x86_64/src/gdt.c` | open | `IST2` and `IST3` in the TSS and the gates that name them. |
| Without a disk the volume of x86-64 is still RAM (512 KB) and does not survive a reset; with one it is the first partition of a raw image on the primary ATA channel (PIO, MBR/GPT only, no DMA, no second disk). | `ports/port_x86_64/src/blockdev_ram.c`, `ports/port_x86_64/src/ata.c`, `ports/port_x86_64/src/kmain.c` | partial | A second medium and a driver with DMA or interrupts; the disk itself is written (ADR 0008). |
| No network: `k_port_net_*` answers "no", so the commands that need a socket say so. | `ports/port_x86_64/src/port_x86_64.c` | open | A NIC driver, or an explicit "this machine is a check, not a networked one" in the contract. |
| No store: `k_port_kv_get`/`set`/`next` answer "no" (`commit` answers 0). | `ports/port_x86_64/src/port_x86_64.c` | open | A place to keep a few keys (the RAM volume or a medium), and the same tests as the host store. |
| One `errno` for all tasks in the kernel of the board (the x86-64 port now copies the TLS block per task and follows it with `IA32_FS_BASE`). | `components/port_esp32`, `ports/port_host` | open | A thread pointer per task on the ports whose tasks share the block (the host has threads of its own). |
| The refusal paths of `k_port_exec_enter()` are not covered on the host: only a good image goes through `make x86-smoke`. | `ports/port_x86_64/src/port_exec.c`, `tests/cases/test_exec.c` | open | A host test of the checks (an image past the window, a bad entry, a bss over the ceiling) or a port test of the refusals. |
| A program is a task of the scheduler now: `execve` makes it, waits for it and the shell is asleep until it exits (K4). A program that calls `execve` starts a second task instead of replacing itself, and the open files are not inherited by a fork. | `ports/port_x86_64/src/port_exec.c`, `components/kernel/src/ksys.c` | partial | The POSIX shape of `execve` and the table of descriptors of a fork. |
| `exec_enter` accepts a hand-built `struct k_exec_image` and copies from it; the parser's checks are repeated there, but the structure is not tied to the bytes the caller holds. | `ports/port_x86_64/src/port_exec.c` | open | One owner of the validation (the parser) and a port that takes only a parsed image, or a documented rule that the port repeats the checks. |

## K6 - The filesystem and the network

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No timestamps: a record of MicroFS has no time of a change or of a creation, and the clock of the kernel feeds nothing. | `components/mfs/include/mfs/mfs.h`, `components/mfs/src/mfs.c` | open | A time field (the record is 16 bytes, so the format needs a decision), the writers of the record and `stat`. |
| FAT is read-only: no mode, no owner, no time and no `statfs`, and one volume at a time. ext2 is not written. | `components/fs_fat32/`, `components/kernel/include/kernel/vfs.h` | partial | Write support (a journal and an owner of the volume), the time field of `struct vfs_stat`, the count of the free clusters, and the ext2 reader. ADR 0009. |
| No `/etc/fstab` and no `mount` command: a second volume is mounted by the boot or the tests through `vfs_mount_type()`. | `components/kernel/src/vfs.c`, `components/kshell/` | open | A parser of the table and a command over the type registry; the registry is there. |
| No journal and no repair: `mfs_check`/`fsck` reads the volume and names the first problem, but a power cut can still leave a record no checker can make whole again. | `components/mfs/src/mfs_check.c`, `components/kshell/src/kshell_files.c` | open | A journal, or a repair pass with a decision about the data it throws away. |
| The record capacity of a volume is a constant of the mount (256, `MFS_MOUNT_RECORDS`); the formatter cannot size the table from the free space, and a volume cannot ask for more records later. | `components/mfs/src/mfs_vfs_mount.c`, `components/mfs/src/mfs.c` | open | Compute the count from the volume at format time, and a documented way to grow the table (or a refusal that says so). |
| The HTTP page is not started by the boot and the `400` path has no test: the command `httpd [port|off]` is manual, and a close with an unread tail can reset the loopback. | `components/net/src/net_http_server.c` | open | A decision of the platform (init script or policy) and a drain before the close. |
| No TLS, no IPv6 and no raw sockets. | `kernel/port/net.h`, `docs/architecture.md` section 7 | decision | None for the board on the network of its owner; revisit when a public network is a requirement. |

## K7 - Quality

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No fuzz of the syscall ABI and of the parsers of the filesystem: MOX1 and the edge of `exec` have a deterministic seed (`tests/cases/test_fuzz.c`), the records of MicroFS have none. | `tests/`, `components/kernel/src/ksys.c`, `components/mfs` | open | A deterministic harness for the records of the filesystem, seeded from a formatted volume and the corpus. |
| `make ci` is not the whole list of the cloud: it runs build, test, ASan, smoke and integrity, not TSan, valgrind or `x86-smoke`. | `Makefile`, `.github/workflows/ci.yml` | open | Make the local target the same list, or say in `CONTRIBUTING.md` why the two differ. |
| `tools/device-test.sh` is outside CI: no runner has the board. | `.github/workflows/ci.yml`, `tools/device-test.sh` | open | A self-hosted runner with the board, or a documented manual gate for a release. |
| The budget of the image is checked; the budget of the SRAM is not. | `tools/integrity_check.py` (`check_budget`), `docs/roadmap.md` K7 | open | A static or a boot-time check of the internal RAM (the stacks, the drivers, the kernel) next to the image budget. |
| The null probe cannot catch a libc call: it is linked with the libc of the host, so `printf` in portable code would link. | `ports/port_null/README.md`, `tools/integrity_check.py` | open | Scan the portable sources for the names of the libc, or link the probe without the libc. |
| The host port cannot reproduce preemption: the critical section is one recursive `pthread` mutex, so a race is invisible unless TSan looks for it. | `ports/port_host/README.md` | open | A model of preemption, or TSan as the only detector (it is in the cloud CI). |

## The board (ESP32, P3-P5)

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No sound detector on top of `drv_mic`: the driver reads RMS and peak, nothing acts on them. | `components/drivers/src/drv_mic.c` | open | A task that watches the level and a threshold of the person. |
| No RTC alarm and no deep sleep, and no wake from EXT1. | `docs/ports/esp32/roadmap.md` P3, `components/drivers/src/drv_rtc.c` | open | The alarm of the BM8563, `esp_sleep` and `HOLD` (G4) high after the wake. |
| No `drv_ir`: G19 is shared with the LED and nothing transmits or receives. | `docs/ports/esp32/roadmap.md` P3, `components/drivers` | open | A driver of the IR pin and a rule that arbitrates it with the LED. |
| No factory reset, no enclosure and display modes of the wearable. | `docs/ports/esp32/roadmap.md` P3 | open | A command that erases the volume and the store, and the modes of the case. |
| No OTA and no second application slot: `factory` 2 MB and MicroFS 5.75 MB leave no room, and repartitioning loses the volume. | `partitions.csv`, `docs/ports/esp32/roadmap.md` P4 | open | A new partition table (`factory` + `ota`), an update client and a rollback; the volume has to be backed up first. |
| No PMIC: power and sleep belong to the ESP32, and `HOLD` (G4) must be high after a wake. | `docs/ports/esp32/README.md` "Limits" | decision | None: the board has no fuel gauge, and the power switch is the ESP32. |
| The battery percent is a curve of the voltage, not a measurement of the charge. | `components/drivers/src/battery_soc.c` | open | A discharge model and its calibration at the known points of the cell. |
| Internal SRAM is ~100 KB for the kernel, the drivers and the stacks; there is no check of that budget (see K7). | `docs/architecture.md` section 10 | open | The SRAM half of the budget check. |

## K0 - The library

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No `mmap` and no `brk`: the memory of the tasks is shared and a program cannot ask for a range of its own. | `docs/libc.md` "What the library cannot do" | open | The address space of K5 and a call over it. |
| No `dup2`: a program cannot send its output to a file. | `docs/libc.md`, `components/libc/src` | open | Duplicate an entry of the descriptor table and test the two descriptors on one file. |
| The descriptor 0 is not the console; a program that wants the input opens `/dev/tty0`. | `docs/libc.md`, `components/devfs/src/devfs.c` | open | Bind 0 to `tty0` for a program (or a decision that it never is). |
| `printf` of a float agrees with the host only to the precision asked for; the last digit can differ. | `docs/libc.md` | open | A `dtoa` with the exact rounding, or a documented precision. |

## Decisions, not debts

* **The board never runs a program.** Its flash is not executable for a file,
  and internal SRAM is the memory of the kernel and the drivers; `execve`
  answers `ENOSYS` (ADR 0005).
* **No root bypass.** There is one user, uid 0, and it gets no exemption: a
  file of mode `0200` cannot be read by anybody, the only user included.
* **`chmod 000` is not expressible.** Zero in the mode bits means "the record
  predates the modes", and the reader falls back to `0644`/`0755`.
* **No TLS, no IPv6, no raw sockets.** The board is on the network of its
  owner; the contract of the port offers TCP, a server and one-shot UDP.
* **Telnet has no authentication.** The board is on the network of its owner,
  and `telnet off` closes the door.
* **MOX1, not ELF.** A flat image with a 32-byte header, linked for the window
  of the port; a loader of ELF is a chapter that a machine with 100 KB of
  SRAM does not need.
* **`stat` checks nothing.** It reports; the check happens at `open` and
  `execve`.
* **The host port cannot run a program.** Jumping into an image inside the
  simulator would run it with the libc of the host under it; the format is
  exercised there as data.
* **The board has no PMIC.** `HOLD` (G4) is the power switch of the design,
  not a missing chip.
* **The applications are not in this tree.** The sensor logger, the clock and
  the weather station are a repository of their own; a board with 8 MB of
  flash does not carry programs nobody asked for.

## Notes that were stale

This pass corrected the statements below; they are kept here so that a reader
of an older copy knows what changed.

* **The boot of x86-64 does go through `execve`.** `docs/phases/phase7.md`
  said the boot called the parser and the loader itself; `program_run()` in
  `ports/port_x86_64/src/kmain.c` calls `ksys_execve()`, and the phase report
  has said the opposite in two places. Both are fixed.
* **`drv_mic` was a stub.** `docs/architecture.md` listed it with the
  finished drivers while the module had no check and the doctor said `skip`.
  The driver is written now (PDM RX at 16 kHz, RMS and peak) and the notes
  say what it does; only the detector on top of it is still ahead.
* **README said there were no file owners at the edge.** Every record has a
  mode and an owner, `chmod`/`chown` reach them and `open`/`execve` check the
  triad; what is missing is the second user, not the ownership. The bullet is
  reworded.
* **The window of x86-64 is 16 MiB, not 32 MiB.** `docs/ports/x86_64/README.md`
  said "the window is 32 MiB at 32 MiB"; it starts at 32 MiB and ends at
  48 MiB.
* **TSan and the image budget are checked already.** `docs/roadmap.md` K7 kept
  them open; `.github/workflows/ci.yml` runs `make test-tsan`, and
  `tools/integrity_check.py` checks the image. The SRAM half of the budget
  stays open.
* **The layout of MicroFS is not "4% and one block of INFO".** `docs/memory.md`
  said `INFO` was 1 block and `META` was always 4%; the formatter puts `INFO`
  in the last erase sector (4 blocks), starts `META` at 4% and grows it until
  the bitmap and the table fit, and stores the three boundaries in the header.
  The example table and the list of improvements are corrected.
* **`v0.4.0-rc1` and `-rc2` exist.** `docs/ports/esp32/roadmap.md` P2 stopped
  at `v0.3.1`; both release candidates are named now, and `v0.4.0` is left as
  the next one.
* **The translations.** `docs/ru/roadmap_ru.md` and the other Russian files
  carry the banner that says they may lag; this document has no translation
  yet, and `docs/ru/README.md` says so.
