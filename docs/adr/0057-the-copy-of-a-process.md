# ADR 0057 - The copy of a process: the fork of 64 MiB in a millisecond

* Status: accepted
* Date: 2026-11-25

## Context

ADR 0005 promised a `fork()` whose child is a *copy* of the parent and not a
re-run of its program; K4 built it (ADR 0007 §7): the x86-64 port copies the
page tables, points both copies at the same frames, takes the write bit away
and copies a page on the first write. 1.9 and 2.0 made the rest of the
promise: the task is a process with a status, the pages are copy-on-write and
the exit of a child is a status its parent waits for.

What was left was the **cost**, and a machine that could not show it:

* the window of the programs was **16 MiB** (ADR 0007), so a program of
  64 MiB - the number the plan of 2.0 promises - could not exist on this
  port at all;
* the **ceiling of the memory of a task** was a constant of the portable
  kernel, 2 MiB, "the PSRAM of the board" (`kernel/task.h`). A machine with a
  window of megabytes and a laptop with gigabytes all started a task at the
  ceiling of a microcontroller;
* the copy of the tables walked the window **one page at a time**, and each
  page took the critical section of the machine to count its frame and an
  `invlpg` to drop its entry: about **16 000 lock pairs and 16 000
  invalidations** for 64 MiB, and the same walk at every exit of a program;
* `k_mm_fork()` gave the child a **file of its own per region**, reopened by
  path: the image of a program is three file-backed regions, so the fork of a
  small program read the volume three times, and the fork of a program of
  64 MiB paid that before it shared a byte;
* and a page that either side wrote was named by a line of the log **in the
  middle of the output of the program**, one line per page.

The machines that are not this one are unchanged: the board has no MMU (K3),
and a program there is a task of the kernel's address space; the host has no
address space for a program and its `fork()` is a pthread that shares the
memory of the parent. This ADR says what the x86-64 port does and what the
two others do not.

## Decision

### 1. The window of the programs is 96 MiB, and the ceiling is the machine's

The window moves from 32 MiB - 48 MiB to **256 MiB - 352 MiB**, and the
physical arena behind it from 64 MiB - 80 MiB to **64 MiB - 160 MiB**. A
program of 64 MiB then has its image, its heap or mapping of 64 MiB, the
room of its stack and the vectors of the entry in the same space, with about
30 MiB to spare. A program is linked at the new base (`PROGRAM_BASE` of the
port's CMake and `PORT_X86_64_USER_BASE` are one number, and `mkprog.sh`
refuses an image of another base).

The window is also a hole in the identity map of a program's space, and the
kernel reads the modules of the boot - the root image of an ISO - where the
loader put them, through that map. A module may therefore not live in the
physical range 256 MiB - 352 MiB, and `src/blockdev_module.c` refuses one that
does instead of reading a page that a program's space does not map. The
machine needs 160 MiB of RAM below the first gigabyte for the arena to be
whole; `make fork-smoke` and the boot of the gate run on 256 MiB.

The ceiling of `KSYS_RLIMIT_MEMORY` is a fact of the machine and the port
answers it: **`k_port_memory_limit()`**, contract **1.15**
(`kernel/port/mem.h`). On x86-64 it is the window below the restorer
(99 540 992 bytes, just under 95 MiB); on the board, the host and the null
port it stays the 2 MiB of the PSRAM, so nothing of those machines changes.
`KSYS_RLIMIT_MEMORY_MAX` remains in the ABI as that number - the ceiling of a
machine whose programs share the memory of the kernel - and a program learns
its own with `getlimit`.

### 2. The copy is the tables of the parent and not its pages

`port_x86_64_paging_space_clone()`:

* the child's tables are created empty and the window's chunks get their
  table of 4 KiB pages **on the first page of the chunk that is mapped**
  (`port_x86_64_paging_map_user`), so a small program costs three tables and
  not the forty-eight of 96 MiB;
* the clone walks the **present chunks** of the parent and, for each, copies
  its 512-entry table with one `memcpy`; inside the chunk it makes one pass
  over the entries: the frame of every present page is counted
  (`g_frame_ref`, one increment), and a page that was **writable** loses the
  write bit in both spaces and gets the software bit `PTE_COW`;
* the whole walk is **one critical section**, not one per page, and the TLB
  of the parent is dropped with **one write of CR3** at the end instead of an
  `invlpg` per page. The port writes CR3 at every switch anyway, so no core
  keeps a stale entry of this space (the K8 debt of the TLB);
* **no byte of a page is copied**. The table of the parent is made the shared
  table first and the child's table is the `memcpy` of it: one store per page
  in the walk, and not two.

A write on either side then takes `port_x86_64_paging_handle_cow()`: the
frame is copied (4 KiB), the copy keeps the protection and the NX bit of the
page it replaces, the old frame loses one reference, and the instruction that
faulted is run again. A page that was read-only before the fork - the text -
is shared without a copy-on-write mark, so a program still cannot rewrite its
own code.

### 3. The child gets names and not descriptors

`k_mm_fork()` copies the records of the regions of the parent and **does not
open the file of each**: the child's region carries the path and is closed,
and the first page of it that the child faults on opens it (`region_open()` in
`kernel/mm.c`). A fork therefore writes no record of a volume and reads none,
and a child that calls `execve` a line later never opens the files at all.
The record of a shared mapping (the bitmap of the dirty pages) is copied, so
the child writes its own pages back.

What is *not* shared is the position in the file: POSIX shares the open file
description across a fork, and this system gives the child a description of
its own, at the offset the fault needs. That is the debt of the row of
`docs/debts.md`; the mapping of a file that was unlinked after the parent
opened it is still a mapping the child cannot fill.

### 4. The counts and the clock are in the log

The port writes one line per fork:

```
x86-64: fork (pid 5) shares 16404 page(s) in 390 us (tables 370 us)
```

- the number of the frames the two spaces share (the memory of the program,
  and not a few pages of it);
- the time of the whole call, and the part of it that is the tables - the
  clock is the TSC of the processor (`port_rdtsc`), because the microsecond
  clock of the port counts the ticks of the timer and its unit is the
  millisecond.

The pages a task copied on write are counted in its context and named **when
it ends**, not at every fault:

```
x86-64: the task (pid 6) copied 6 page(s) on write
```

One line per page would be inside the output of a program that prints - the
first version of this lane did exactly that, and `make x86-smoke` found it:
the child of `/bin/fork` printed `value `, the kernel printed a line, and the
program printed `99` after it. A debug log level still names each page for a
person who asks.

### 5. The gate: `make fork-smoke`

`/bin/forksmoke` maps 64 MiB of anonymous memory, writes one byte of every
page (so the pages are frames and not a promise), forks once **as a warm-up**
- the first fork of a boot pays the slab of the records of a space, the guard
of the first task stack of a chunk and the first block of the heap, and that
is not the call this lane measures - and then forks the program of 64 MiB:

* the child writes four pages and reads its own writes;
* the parent reads the same four addresses and must find the value it wrote
  **before** the fork;
* the parent waits for the child, which ends with 0, and prints its verdict;
* the port's line carries the count of the shared pages and the time of the
  call, and the task's line carries the count of the copies.

`make fork-smoke` reads those lines: the fork shares the memory (at least the
16384 pages of the mapping), the time of the call is **under a millisecond**,
the pages the child wrote were copied, the parent sees the old values, the
child the new ones, and the shell answers after the program. The release gate
runs it after `mmu-smoke`.

### 6. The machines without a page table

| Machine | `fork()` | What proves it |
|---|---|---|
| x86-64 | the tables are copied, the frames are shared, a page is copied on its first write, the child continues after the call | `make fork-smoke`, `/bin/fork`, `/bin/procs` and `/bin/fpu2` in `make x86-smoke` |
| board (FreeRTOS) | no MMU (K3): the child is a task that shares the memory of the parent and **re-enters its program from the start** - a re-run and not a copy; a write of the child is seen by the parent | `make device-test`; the K3 row of `docs/debts.md` |
| host (pthread) | no address space for a program: `k_port_fork_copy()` is -1 and the child is a pthread with the memory of the parent, so a write *is* shared | `tests/cases/test_ksys_process.c`; the host rows of `docs/debts.md` |

The transparent truth of the second and the third row: a program that relies
on the copy semantics of POSIX - a child that writes and a parent that must
not see it - is correct on x86-64 alone. The board and the host are the price
of a machine without a page table, and this ADR names it instead of hiding it
behind the same answer.

## Consequences

**Good**

* A program of 64 MiB forks in **0.34 - 0.62 ms** on the machine of the gate
  (the line of the port: 16 404 pages shared, 322 - 601 µs for the tables),
  under the millisecond the release promises, and the call copies no byte of
  the memory.
* A program may own as much memory as its machine's window holds: 95 MiB on
  x86-64 and not the 2 MiB of a microcontroller; the ceiling is a call of the
  port and a machine that grows a window does not have to edit the kernel.
* The exit of a program is cheap for the same reason its fork is: the release
  of its frames is one critical section and one walk of the tables.
* The output of a program is its own: the kernel's counts are two lines at
  the fork and at the exit, and not one line per copied page.
* A fork does no I/O: the child's files are names until the first fault that
  needs one.

**Bad**

* The cost of the call is still **the number of pages** and not the number of
  chunks: the window is mapped in 4 KiB pages, and 64 MiB is 16 384 entries.
  A window mapped in 2 MiB pages when a mapping is large and aligned would be
  32 entries and a much cheaper call; it is not done because the memory of a
  program - its heap, its mappings, `munmap` of a part of one, the copy on
  write of a page - is 4 KiB in this system, and a second granularity is a
  change of the whole of `kernel/mm.c` for a call that is already under its
  ceiling. It is the first thing to do if the margin ever matters.
* The open file description is not shared: the child opens its own file at
  the first fault, and a file unlinked after the parent's `mmap` is a page
  the child cannot fill.
* The window has a second cost: the identity map of a program's space does
  not cover physical 256 MiB - 352 MiB, so a boot module may not live there,
  and the machine needs 160 MiB of RAM for the arena to be whole.
* The board and the host keep the placeholder (§6): a copy of a process is a
  promise of the machine with page tables.
