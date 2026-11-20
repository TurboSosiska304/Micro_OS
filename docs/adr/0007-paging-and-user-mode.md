# ADR 0007 - Paging, ring 3 and the failure of a program

* Status: accepted
* Date: 2026-11-21

## Context

ADR 0005 gave the system programs as files and ADR 0006 gave them a door
into the kernel (`int $0x80`). The program was still ring 0 code in a window
of the identity map: it could read the kernel, write its variables, stop the
timer or jump into a page that belonged to the scheduler. The privilege
boundary was logical, and the port had an MMU it did not use.

K5 is the step that makes the boundary real on the x86-64 port:

* the kernel is not reachable from a program;
* a bad pointer of a program is a line on the console, not a machine that
  stops;
* `exit` of a program returns to the call that entered it;
* every task has its own `errno`.

The board still has no MMU, so nothing of this may move into
`components/kernel/`: the map, the descriptors, the fault handler and the
return are the business of the port.

## Decision

### 1. Two maps, one address space per program

The layout is fixed and small enough to be read in one table:

| Virtual | What | Rings |
|---|---|---|
| `0x00000000`-`0x00001000` | not mapped in a program | - |
| `0x00000000`-`0x40000000` | identity map of the machine, 2 MiB pages | 0 |
| `0x02000000`-`0x03000000` | the window of the programs, 4 KiB pages | 3 |
| `0x04000000`-`0x05000000` | the physical arena behind the window | 0 |

The kernel map is the one the loader made: the first gigabyte in 2 MiB
pages, supervisor-only, rebuilt by `src/paging.c` so that the port owns it
and not the loader. A program gets a copy of that page directory with two
differences: the entries of the window point at PTE tables (user, read and
write), and the first 2 MiB are split into 4 KiB pages with page zero not
present at all; the rest of the low memory stays mapped supervisor-only,
because a system call of the program still writes the text screen at
`0xB8000` (and the disk and the keyboard are ports, not memory). The upper
entries of the program's map carry the user bit as well: the processor checks
it at every level of the walk, not only at the leaf.

The window is not identity mapped. Virtual `0x02000000` is physical
`0x04000000`, well above the heap (which stops at 32 MiB) and outside the
kernel's own pages. The kernel writes an image through the arena and the
program reads it through the window; an address a program computes is not an
address of the kernel. The rest of the window is deliberately left out of the
mapping: a program that runs past its data meets a page that is not there.

The stack is the last 64 KiB of the window, and the page below it is the
guard: it is not present, so an overrun of the stack is a page fault of the
program instead of a write into its own data.

The page tables of a program come from a pool of pages in the kernel image
(PML4, PDPT, PD and eight PTE tables). One program runs at a time on this
port, so one set is built per `execve` and destroyed when the program ends.
The next one gets fresh, zeroed tables.

### 2. The descriptors: a new GDT in the 64-bit port

The loader keeps its two-selector GDT: it exists only to switch the
processor to long mode, before there is C to build anything else. The port
builds the real table in `src/gdt.c`:

| Selector | What | DPL |
|---|---|---|
| `0x08` | 64-bit code | 0 |
| `0x10` | flat data | 0 |
| `0x18` | 64-bit code | 3 |
| `0x20` | flat data | 3 |
| `0x28` | 64-bit TSS | 0 |

The TSS is not a task of the scheduler. It carries two numbers the processor
needs at a privilege change:

* `RSP0` - the kernel stack of the program that is running, written by
  `port_exec.c` before every entry. It is a stack of its own (32 KiB in the
  image), because the call of `k_port_exec_enter()` stays alive below it for
  as long as the program runs;
* `IST1` - a 16 KiB stack for the double fault, so a fault raised while the
  current stack is broken still has somewhere to land. Vector 8 names it in
  the IDT.

The I/O map base is past the limit of the TSS: ring 3 has no port it may
touch, and `in`/`out` from a program is a general protection fault.

### 3. The entry and the way back

`k_port_exec_enter()` copies the image into the arena, maps the pages of the
image, of the arguments and of the stack, points `RSP0` at the kernel stack of
the program and switches CR3 to the program's tables. `port_exec_jump_user()`
then builds an `iretq` frame - SS `0x23`, RSP, RFLAGS with the interrupt flag
on, CS `0x1b`, RIP - and returns to ring 3. The arguments of the program are
copied into its own memory (the strings after the image, the array of pointers
after them), so a program never sees a pointer of the kernel.

The vector 128 is an interrupt gate with `DPL 3`: a program may raise it, the
processor switches to `RSP0`, and the handler runs on the kernel stack of the
program. It saves the registers of the call, puts the thread pointer back to
the kernel's block, calls `ksys_call()` and returns with `iretq`, setting the
thread pointer to zero first: the kernel's TLS block is not mapped in ring 3.
No `swapgs` is used, because the port keeps no per-CPU GS state.

`KSYS_EXIT` no longer means `task_exit` when a program is running. The
kernel asks the port (`k_port_exec_active()`), and the port unwinds: it
restores the kernel CR3, the kernel stack and the callee-saved registers
saved by `port_exec_jump_user()` and returns from that call into
`k_port_exec_enter()`, which reports the status to the kernel
(`k_exec_exit()`) and prints `x86-64: the program exited with N`. `execve`
answers 0, as before: the machine has a program but no process to exit into
yet.

### 4. A fault of a program is not a fault of the machine

The exception stubs pass four numbers to C: the vector, the error code, the
RIP and the CS of the interrupted code. When the selector names ring 3, the
port prints

```
the program faulted: vector N at 0x...
```

and takes the program down through the same unwind as `exit`, with the status
255. The address is CR2 for the page fault and the RIP for the other vectors.
A fault of ring 0 still prints the register dump and stops the machine: the
kernel has nowhere to return to.

The failure paths are covered on the machine: `/bin/crash` writes to address
zero in ring 3, the smoke test reads the fault line, and the shell answers
the command after it.

### 5. An errno per task

The C library keeps `errno` in a `_Thread_local` and the compiler addresses
it through `%fs`. The block used to be one for all tasks. The port now gives
every task a copy of the TLS block (`__tls_start` to `__tls_end`) and points
IA32_FS_BASE at the end of that copy on every context switch. The syscall
stub sets it to the kernel's block on entry and to zero before the `iretq`,
so a program of ring 3 does not inherit the pointer and the kernel does not
lose its own.

### 6. The contract becomes 1.4

Two calls are added to `kernel/port/exec.h`:

* `bool k_port_exec_active(void)` - is a program in flight;
* `void k_port_exec_exit(int code)` - end it and return to the entry call.

Every port implements them: the board, the host and the null port answer
false and do nothing, and the kernel keeps `task_exit()` for a machine whose
programs are tasks. The minor version moves from 3 to 4 in
`kernel/port/info.h` and in every port.

## What is not done

* `fork()` with copy on write. `k_port_fork_copy()` still exists only in
  ADR 0005; `sys_fork` keeps the placeholder of the board. The page tables of
  a program are rebuilt per `execve` and one program runs at a time, which is
  not yet an address space per task.
* The kernel is not in a higher half. It keeps the identity map of the first
  gigabyte, so the window of the programs is a hole in the kernel's map and
  not the other way round.
* A pointer of a program is still read by the kernel directly (no
  `copy_from_user`, no SMAP): a program can hand a system call an unmapped
  address and fault in ring 0, which stops the machine. The memory of the
  window is mapped in the program's CR3, so a valid pointer works; an invalid
  one is a known hole, not a check.
* No NX, no SMEP/SMAP, no PCID, no SMP: every entry flushes the TLB and the
  map assumes one processor.
* `KERNEL_NOMMU` is still 1 for the whole tree. Turning it off is a change of
  the generic kernel (and of the board's concessions), not of this port alone.
* A program is not a task of the scheduler: the shell that runs it is blocked
  in the call, and a program that never calls or exits keeps the CPU it was
  given (its interrupts are on, so the clock still ticks, but there is no
  second program to switch to).
* The ARM port with an MMU is still a plan.

## Consequences

**Good**

* The x86-64 port has a real privilege boundary: the kernel is supervisor
  only, ring 3 can reach the window and its stack and nothing else, and the
  address zero is not mapped.
* A bad program is a line and a status, not a dead machine; the shell that
  ran it is alive.
* `exit` from a program returns to the caller of `execve`, and `execve` keeps
  answering 0.
* Each task has its own `errno`.

**Bad**

* One program at a time, one set of page tables, one kernel stack for it: the
  design of the port, not of the kernel.
* The kernel's own map is still the low identity map; the road to `fork()`
  and to a higher half is open and documented, not taken.
* The pointers of a program are not yet copied at the edge of the system.
