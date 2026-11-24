# ADR 0048 - The guard of a stack: a fault of a task is not a fault of the machine

* Status: accepted
* Date: 2026-11-24

## Context

The system had one rule on paper and three behaviours in fact:

* a program of ring 3 that touched an address it did not own was taken down
  by the fault handler, the task of the kernel that waited for it got status
  255, and the shell answered the next line (ADR 0007). The page under the
  stack of a program was already not mapped, and `/bin/badptr` and `/bin/nx`
  prove it in `make x86-smoke`;
* a **kernel task** whose stack overflowed walked into the heap under it. The
  stack was a block of the port's allocator and nothing stopped the stack
  pointer at the end of it; a store past the bottom wrote over the block of
  another task, or over the header of the allocator, and the machine found
  out much later. `docs/debts.md` records the discovery: one local variable
  added to `kshell_invoke()` filled the 16 KiB stack of the shell under
  `pkg -S` and the fault arrived in `k_sched_tick`, far from the frame that
  caused it (K5, and the same row under K8);
* a program of the **board** is a task that shares the address space of the
  kernel; its bad pointer is a `Guru Meditation` and a reboot, and the K3
  table of `docs/debts.md` says so with the address of the fault.

The x86-64 port and the host are the two machines with page tables. The board
has none. The decision has to give the two the whole contract and say the
truth about the third, without spending the hundred kilobytes of SRAM of the
board on a page that does not exist there.

Two facts about the port make the implementation less obvious than "map a
page":

1. the stacks of the kernel tasks of the x86-64 port live in the low identity
   map - the same 2 MiB pages that name the heap, the arena and the devices -
   and every address space shares that map;
2. the fault of a stack may arrive when the stack pointer has already left the
   stack. The processor then cannot push the frame of the page fault, and the
   first fault becomes a double fault before any C of the kernel runs.

## Decision

### 1. The contract, one sentence per port

A fault of a task is the end of that task. The machine survives it, the task's
parent sees a status, and the rest of the system keeps running.

| Machine | A task that faults | What proves it |
|---|---|---|
| x86-64, program of ring 3 | the program is taken down, status 255 | `/bin/crash`, `/bin/nx`, `/bin/badptr`, `/bin/crash stack` |
| x86-64, kernel task | the task is taken down, status 255 | `guard` of the shell and its line in the log |
| host (pthread) | the task is taken down, status 255 | `tests/cases/test_guard.c` |
| board (FreeRTOS) | the machine stops, with the name of the task | the canary of FreeRTOS, section 5 |

The status is 255 because that is the status the fault of a program has had
since K5 (`port_x86_64_program_fault()`), and a parent should not need to know
which kind of task its child was in order to read the number.

### 2. x86-64: a page under the stack of every kernel task

The allocation of a context (`task_context_new()` in
`ports/port_x86_64/src/port_x86_64_task.c`) is one block of the port's
allocator, laid out like this:

```
base                    the struct port_task and the alignment slack
align_up(base, 4 KiB)   |
+ 4 KiB                 the guard page: present = 0, no frame may be pushed
+ 4 KiB                 the stack, k_port_task_stack_size() bytes (32 KiB)
```

The guard is the page *below* the lowest address the stack may use. The stack
still grows down from its top; the frame that crosses the end of the stack
lands in a page that is not present.

The page is not present in the **kernel's** low identity map. The identity map
is made of 2 MiB pages (ADR 0007), so the first guard of a chunk of 2 MiB
splits that chunk into 512 pages of 4 KiB. Every page of the split keeps the
protection of the large page it replaced - present, writable, supervisor, NX -
and only the guard is cleared. One table of the pool is spent per chunk that
holds a stack, not per task: a second stack in the same chunk reuses the
table. `port_x86_64_paging_kernel_guard_install()` refuses an address that is
not a writable NX page of data, so a mistake cannot take the text of the
kernel out of the map, and `kernel_low_pt()` refuses the chunks of the window
of the programs, which are not identity memory at all.

The block goes back to the allocator in `release_pending()`, on the next
context of the core, and the guard goes back first:
`port_x86_64_paging_kernel_guard_remove()` puts the identity page of data
back before `k_port_free()` is called. Without that the freed block would
carry a hole, and the next allocation that landed on it would fault on a page
nobody promised to be missing.

A page of a map is not a property of a task until the task runs on it. A
space that was built **before** the split of the chunk of a stack still names
the large page in its own PD. That is harmless for every task but one: the
child of a fork clones the space of its parent and then gets a stack of its
own, and its space may be older than the guard of that stack.
`port_x86_64_paging_space_sync_kernel()` copies the current entry of the
kernel's PD into the PD of the space before the child can run, and the same
call is made before an `execve` enters a program on the stack of its task.

The TLB is the other half of a page table. The port writes CR3 at every
context switch (`commit_switch()`), which flushes the TLB of the core, so a
stale large-page entry cannot outlive the switch to the task whose stack the
guard belongs to. A core never writes the stack of another task, so there is
no shootdown to send; a machine whose switch stops writing CR3 would owe the
kernel one (the K8 debt of the TLB, `docs/debts.md`).

### 3. The fault that cannot push its frame

If the store that overflowed left RSP inside the guard, the processor tries
to push the frame of the page fault there, faults again, and raises the
double fault. The page fault therefore has two shapes:

* the frame fits on the stack (the faulting address is in the guard but the
  stack pointer is still above it, or the fault is a read or a call from a
  frame that has not moved RSP): the handler of vector 14 reads CR2, sees the
  guard of the task that runs, and ends the task;
* the frame does not fit: the double fault of vector 8 takes IST1, and CR2
  still names the page that broke the first fault. The handler of the double
  fault makes the same test and ends the same task.

The two tests are one call, `port_x86_64_task_guard_fault()`: the guard is one
page, it belongs to the context of the core and to no other, and an address
inside it is an overflow. A page fault from ring 3 is not tested this way
(`cs & 3`), because a program may name a supervisor address and its CR2 would
be that address; its fault is the fault of a program and takes the path it
always took.

The task ends through `task_exit(255)`, on the stack of the exception: the
double fault runs on IST1, the page fault on the stack of the task when that
stack still holds the frame. The dead context is never resumed; the next
context of the core releases its block and its address space, exactly as it
does for a task that ended by itself. `task_exit()` is the kernel's exit and
not a second path: the descriptors close, the children are reparented to init,
the parent is woken, and the log line names the task:

```
x86-64: the task 'stack-test' (pid 42) reached the guard page at 0x...
x86-64: vector 8, rip 0x...: the task ends, the machine stays
```

The boot context of a core (kmain on the core 0, the idle loop of an
application processor) is not a task and has no guard. Its stack is the stack
of the machine; an overflow there is a bug of the kernel and still stops the
machine. The stack of an application processor (`src/smp.c`) is a block of the
heap without a guard for the same reason: it is the boot context of that core,
not a task.

### 4. Host: the guard of a pthread, and the handler on a stack of its own

A task of the host port is a pthread. glibc already puts a page under the
stack of a thread that is not readable and not writable, and the port asks for
one page explicitly (`pthread_attr_setguardsize()`). The port then reads the
bounds of the stack of each thread with `pthread_getattr_np()` and keeps them
with the control block of the task.

A `SIGSEGV`/`SIGBUS` handler installed by the port answers only an address
inside `[base - guard, base)`. Every other signal, a real bug of the kernel
included, is handed to the handler the process had before (the one of the
address sanitizer, in the build of the gate), so the port does not hide a
crash.

The handler cannot run on the stack that just overflowed. Each thread takes
one alternate stack of a fixed pool (`sigaltstack()`, 16 KiB, 32 slots) before
it starts, and the handler runs there and `siglongjmp()`s to the escape point
of the thread. The task then ends through `task_exit(255)` like any other, and
the pool slot is given back by the cleanup handler of the pthread. The pool is
fixed on purpose: a stack allocated per task and freed by the task itself
would be a leak in the address sanitizer of `make test-asan`, and a run of the
soak makes thousands of tasks.

The longjmp is the honest limit of the host: the stack of the fault is
abandoned, and a task that faulted in the middle of a kernel lock leaves the
lock held. The host is the machine of the tests, and a test that overflows a
stack from a kernel lock would be a test of the kernel, not of the guard.

### 5. The program of the board: the canary, and the truth about it

The board has no MMU, so a task of the kernel and a "program" share one
address space. There is no page to take out, and the stack of a task is 8 KiB
of the hundred kilobytes of internal SRAM (ADR 0018): a copy of the stack, a
red zone, or a guard page of 4 KiB per task would be memory the board does not
have.

The decision is to change nothing in the memory of the board and to say what
the machine does:

* FreeRTOS is built with `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` (the
  default of ESP-IDF 5.3): the last words of every stack are a canary, and the
  scheduler tests it at every switch. An overflow is **detected** and named,
  with the name of the task, before the board aborts. That is the free half of
  the rule: the fault is attributed to a task;
* it is not the whole rule, and the port does not pretend otherwise. The
  canary is tested after the overflow, when the bytes under the stack have
  already been written. The neighbour may be the block of another task; killing
  the task at that point would leave the heap corrupted, which is worse than a
  machine that stops. The board therefore keeps the panic, with the task named
  by the hook of the RTOS. The K3 row of `docs/debts.md` (a fault of a program
  of the board is a reboot) is the same debt from the other side, and the next
  step of both is an MMU, not a guard.

The SRAM budget is unchanged: this ADR adds no byte of RAM to the board.

### 6. The map of a program: no page both writable and executable

The port already audits the tables before a program runs: the text is RX, the
data, the bss and the stack are RW and NX, the restorer of a handler is RX,
and `port_x86_64_paging_audit()` counts a page that is both writable and
executable. The audit is a refusal at `execve` and at the boot, and the port
now leaves its answer in the log at every program:

```
x86-64: the map of a program is W^X
```

The line is one short sentence and not the name of the file: it is written
before the entry of a program, the console of the machine is a UART written
byte by byte, and a longer line made the window server of `x86-vga-smoke` miss
the registration of its clients (see "Consequences"). The audit itself is the
same walk of the tables that the boot and every exec already made.

The format of an image gains the one flag it has ever named: `K_EXEC_FLAG_WX`
(bit 0 of the flags word, `kernel/exec.h`). The bit was an unknown flag and
was refused before this decision; naming it lets the parser answer with the
sentence that says what the image asked for instead of the generic refusal,
and it is a test of its own in `tests/cases/test_exec.c`. The layout of the
format already makes the request impossible to honour - the text ends on a
page and the data begins on the next one - so the refusal is a promise of the
port made before the tables are built.

## Alternatives

* **A virtual mapping of its own per stack** (a small window per task, mapped
  to the same frames): the K5 row's suggestion. It costs a PML4, a PDPT, a PD
  and a PT per stack from the pool of 256 pages, and a switch of the mapping
  at every switch of the context. The split of the identity chunk costs one
  table per 2 MiB chunk, once, and nothing at the switch. The port chose the
  split.
* **A check of the stack pointer at every switch**: no page tables, but the
  check only sees the overflow at the next switch, after the writes, and the
  write that overflowed may already have destroyed the neighbour. It is the
  canary of the board under another name and it is not the whole rule.
* **A guard only for the shell**: the task that runs the shell is not the only
  one with a stack of 32 KiB and a deep call; the fix belongs to the substrate
  of every task, and the test is a task of its own.
* **An IST stack for every page fault**: the handler of a page fault may read
  the filesystem and sleep, and a context that sleeps on the IST stack of the
  core would come back to a stack another fault of the core had overwritten.
  Only the double fault, which never sleeps and never returns, has a stack of
  its own. The double fault is also the shape the overflow takes when RSP has
  already left the stack, which is the one fault that cannot be pushed on it.

## Consequences

* A stack overflow of a kernel task of x86-64 and of a task of the host is a
  line in the log and a status of 255, and the machine keeps running. The
  `guard` command of the shell and `tests/cases/test_guard.c` are the gates;
  `/bin/crash stack` is the same failure for the stack of a program.
* A block of a task of x86-64 costs three pages more than its stack: one page
  for the header and the alignment, one for the guard, and the page the
  allocator's split may leave. A machine of QEMU has the megabytes; the board
  pays nothing.
* A guard is a page, and a frame larger than a page can jump over it. A
  function with a 5 KiB local array that sets RSP below the guard and writes
  there does not touch the guard at all. The budget that would catch it is a
  stack probe in the compiler or a check of RSP at every entry, and neither is
  in this tree; the K7 row of `docs/debts.md` (the SRAM budget is not checked)
  is where the second half lives.
* A task that faults while it holds a `k_mutex` may not end cleanly: the lock
  is not given back, and another task that waits for it waits for a task that
  no longer exists. The decision does not change the locks; it makes the first
  fault visible with the name of the task, which is what a person needs to
  find the lock.
* The report of the audit is one short line, and the lesson is worth keeping:
  a longer line that named the file (the first shape of the change) made the
  light desktop of `make x86-vga-smoke` lose the registration of its two
  window clients, every run, because the console of a machine is a UART
  written byte by byte and the entry of a program of the desktop followed the
  line. The line is not the place to write a sentence of the size of a
  terminal; the audit and its refusal are unchanged, and the name of the
  program is the line of the task that runs it.
* The `docs/debts.md` rows of K5 (a guard page under a kernel stack) and K8
  (the same, as a property of the substrate of the many cores) are closed by
  this decision on the machines with an MMU; the row of the board (K3, a
  program of the board is a reboot) stays open, with the canary as the whole
  of what the SRAM of the board allows.

## Update (2.0): the stack of a program grows, the guard is the end of the room

The stack of a *program* of ring 3 is the one this ADR did not move: the page
under it was the guard, and a program of two pages of frame was taken down for
being deeper than 64 KiB. ADR 0052 gives the program a room of 1 MiB under its
first 64 KiB: the page under the stack of the moment is the growth point, the
frames are made on the fault up to the memory limit of the task, and the page
under the room - `0x02eee000` of the window - is the guard of this ADR. A
recursion that runs past the limit meets it with the same error code (`0x6`)
and the same status (255), one megabyte lower than in 1.9, and `make
x86-smoke` reads the address. The stacks of the *kernel* tasks, the split of
the 2 MiB chunk and the answer to the double fault are unchanged.
