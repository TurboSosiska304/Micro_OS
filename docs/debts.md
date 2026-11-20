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
| The entry into the kernel is ring 0: the handler trusts the pointers of the program, there is no kernel stack per task and no `KSYS_EXIT` path from a program; a program that never returns holds the CPU. | `ports/port_x86_64/src/interrupts.S`, `components/kernel/src/ksys.c` (`sys_exit`) | open | K5: page tables per task, ring 3, a TSS/IST and a kernel stack, `copy_from_user`, and an exit call that unwinds the program. |
| `stat` reports the mode and the owner to anybody and checks nothing. | `components/kernel/src/ksys.c`, `docs/phases/phase7.md` | decision | None: POSIX makes `stat` readable, and the check stays at `open` and `execve`. |
| The `x` bit of the directories of a path is not walked, so a directory of mode `0600` still lets its files be opened by their full path. | `components/mfs/src/mfs_vfs.c`, the VFS lookup | open | Check every directory on the way down, with its own tests. |
| A path over the window answers `E2BIG` (`KERR_TOOBIG`), because the contract has no `ENAMETOOLONG`. | `components/kernel/include/kernel/errno.h`, `components/kernel/src/ksys.c` (`path_check`) | open | `KERR_NAMETOOLONG` in the kernel and the number of POSIX in the library, with the boundary tests. |
| `waitpid` for a child that never comes answers `EAGAIN` after five seconds; POSIX says `ECHILD` or `ESRCH`, and `-3` is `KERR_EXIST`, not the POSIX `ESRCH`. | `components/kernel/src/ksys.c` (`sys_waitpid`), `components/kernel/include/kernel/errno.h` | open | Decide the numbers of `ECHILD` and `ESRCH`, keep `-3` for `EEXIST`, and test the negative cases. |
| A directory cannot be opened (`mfs_open` answers `MFS_EISDIR`), so `readdir` has no path from a program; the shell lists through `vfs_list()` itself. | `components/mfs/src/mfs.c`, `components/kernel/src/ksys.c` (`sys_readdir`) | open | A directory descriptor and the entries read through it, not through a path of the caller. |
| The image is read whole into one block of `kmm` (up to `KSYS_MAX_IO`, 64 KB) before the port copies it to the window: two copies in RAM at once. | `components/kernel/src/ksys.c` (`sys_execve`), `components/kernel/src/exec.c` | open | Stream or map the image, or cap it against the memory of the machine and not only against the transfer edge. |
| `mkprog.sh` does not compare the address a program is linked for with the base of the window of the port. | `tools/mkprog.sh` | open | Check the first VMA of `.text` against `k_port_exec_base()` (on x86-64 `0x02000000` plus the 32-byte header) and refuse another address. |

Closed in the same pass: the `int $0x80` entry and its ABI, `argv` through
`k_exec_run()`, `PATH` and `stat` in the shell, and the test of the padding of
`mkprog.sh`. The row that stays above is the part of the entry that waits for
K5.

## K4 - Protection

| Debt | Where | Status | What it needs |
|---|---|---|---|
| `fork()` is a placeholder: the child shares the memory of the parent and re-enters its program; `k_port_fork_copy()` exists only in ADR 0005. | `components/kernel/src/ksys.c`, ADR 0005 | open | Page tables per task and copy on write - the milestone of K5, not a change of the generic kernel. |
| A program is not protected from the kernel and the kernel is not protected from a program: `KERNEL_NOMMU` is still 1 everywhere. | `components/kernel/include/kernel/config.h`, `ports/port_x86_64` | open | The paging of K5 and the flag turned off for a port that has an MMU. |
| The owner is stored and checked but there is only one user, uid 0, so groups and a second party are not part of the model. | ADR 0005, `components/kernel/src/ksys.c` | decision | None until a second user exists. |
| The owner of the system gets no exemption of root. | ADR 0005, the mode check | decision | None: this is the rule, not a missing feature. |
| `chmod 000` cannot be expressed: a zero mode means "the record predates the modes" and is refused instead of stored. | `docs/memory.md` section 6, `components/mfs/src/mfs.c` | decision | None without a format change; a real zero needs a bit that says "the mode is set". |

## K5 - A processor with an MMU, and the x86-64 port

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No paging per task: only the identity map of the loader exists, so the higher half, user and kernel pages and a fault handler that means something are absent. | `ports/port_x86_64/loader32.S`, `ports/port_x86_64/src` | open | Page tables per task, a switch of `CR3`, a map for the kernel and for the window, and a fault handler. |
| No ring 3: the GDT of the loader and the gates of the IDT are ring 0 only, and there is no TSS, so a program runs with the privilege of the kernel. | `ports/port_x86_64/loader32.S`, `ports/port_x86_64/src/port_x86_64.c` | open | User code and data selectors, a TSS with `RSP0`, gates with `DPL 3`, and the trap path back. |
| No guard page under the stacks of tasks: a stack that overflows walks into the heap of the port. | `ports/port_x86_64/src/port_x86_64_task.c` | open | An unmapped page at the bottom of every stack, which needs the MMU first. |
| No IST and no separate double-fault stack: `isr8` runs on the stack of the moment, so a fault in a fault handler can become a triple fault. | `ports/port_x86_64/src/interrupts.S`, `ports/port_x86_64/src/port_x86_64.c` | open | IST entries in the TSS for the double fault (and the machine check and NMI), with a stack of their own. |
| The volume of x86-64 lives in RAM (512 KB): there is no disk and no flash, and every boot formats it and writes `/bin/hello` again. | `ports/port_x86_64/src/blockdev_ram.c`, `ports/port_x86_64/src/kmain.c` | open | A block device on a medium that survives a reset (a real flash or a QEMU drive). |
| No network: `k_port_net_*` answers "no", so the commands that need a socket say so. | `ports/port_x86_64/src/port_x86_64.c` | open | A NIC driver, or an explicit "this machine is a check, not a networked one" in the contract. |
| No store: `k_port_kv_get`/`set`/`next` answer "no" (`commit` answers 0). | `ports/port_x86_64/src/port_x86_64.c` | open | A place to keep a few keys (the RAM volume or a medium), and the same tests as the host store. |
| One `errno` for all tasks: `_Thread_local` is one block under one `%fs`, because the tasks share the address space. | `ports/port_x86_64/src/port_x86_64.c`, `components/libc` | open | A block per task and `IA32_FS_BASE` switched with the context - after the paging. |
| The refusal paths of `k_port_exec_enter()` are not covered on the host: only a good image goes through `make x86-smoke`. | `ports/port_x86_64/src/port_exec.c`, `tests/cases/test_exec.c` | open | A host test of the checks (an image past the window, a bad entry, a bss over the ceiling) or a port test of the refusals. |
| A program runs with the interrupts off and without the clock: no tick can be served while it is in the window. | `ports/port_x86_64/src/port_exec.c` | open | The trap entry of K3 and a scheduler path that can take the CPU back. |
| `exec_enter` accepts a hand-built `struct k_exec_image` and copies from it; the parser's checks are repeated there, but the structure is not tied to the bytes the caller holds. | `ports/port_x86_64/src/port_exec.c` | open | One owner of the validation (the parser) and a port that takes only a parsed image, or a documented rule that the port repeats the checks. |

## K6 - The filesystem and the network

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No timestamps: a record of MicroFS has no time of a change or of a creation, and the clock of the kernel feeds nothing. | `components/mfs/include/mfs/mfs.h`, `components/mfs/src/mfs.c` | open | A time field (the record is 16 bytes, so the format needs a decision), the writers of the record and `stat`. |
| No second block device and no second filesystem behind the VFS. | `components/`, `components/kernel/include/kernel/blockdev.h` | open | A second consumer of `kernel/blockdev.h` and a mount that is tested next to MicroFS. |
| No `fsck` and no journal: a power cut costs 4 KB, and a damaged record is not repaired. | `docs/memory.md` section 8, `components/mfs` | open | A checker over the META section, or a journal, or an explicit "no" in the contract of the volume. |
| The record capacity of a volume is a constant of the mount (256, `MFS_MOUNT_RECORDS`); the formatter cannot size the table from the free space, and a volume cannot ask for more records later. | `components/mfs/src/mfs_vfs_mount.c`, `components/mfs/src/mfs.c` | open | Compute the count from the volume at format time, and a documented way to grow the table (or a refusal that says so). |
| No MQTT: the tree has an HTTP client and a server, NTP and telnet, and no client of the broker. | `components/net` | open | A client of MQTT over the sockets. |
| The HTTP page is not started by the boot and the `400` path has no test: the command `httpd [port|off]` is manual, and a close with an unread tail can reset the loopback. | `components/net/src/net_http_server.c` | open | A decision of the platform (init script or policy) and a drain before the close. |
| No TLS, no IPv6 and no raw sockets. | `kernel/port/net.h`, `docs/architecture.md` section 7 | decision | None for the board on the network of its owner; revisit when a public network is a requirement. |

## K7 - Quality

| Debt | Where | Status | What it needs |
|---|---|---|---|
| No fuzz of the syscall ABI and of the parsers of the filesystem. | `tests/`, `components/kernel/src/ksys.c`, `components/mfs` | open | A harness (libFuzzer or AFL) and a corpus seeded from `tests/corpus/vectors.txt`; the parser of `exec.c` first. |
| `make ci` is not the whole list of the cloud: it runs build, test, ASan, smoke and integrity, not TSan, valgrind or `x86-smoke`. | `Makefile`, `.github/workflows/ci.yml` | open | Make the local target the same list, or say in `CONTRIBUTING.md` why the two differ. |
| `tools/device-test.sh` is outside CI: no runner has the board. | `.github/workflows/ci.yml`, `tools/device-test.sh` | open | A self-hosted runner with the board, or a documented manual gate for a release. |
| `git fsck` is skipped in a worktree: the check tests `os.path.isdir(root/.git)` and a linked worktree has a `.git` file. | `tools/integrity_check.py` (`check_git`) | open | Accept a file as well as a directory, or ask `git rev-parse`. |
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
* **`v0.4.0-rc1` exists.** `docs/ports/esp32/roadmap.md` P2 stopped at
  `v0.3.1`; the tag of the release candidate is added, and `v0.4.0` is left as
  the next one.
* **The translations.** `docs/ru/roadmap_ru.md` and the other Russian files
  carry the banner that says they may lag; this document has no translation
  yet, and `docs/ru/README.md` says so.
