# ADR 0052 - The policy of a page: W^X, a stack that grows, a heap with a ceiling, and a kernel ring 3 cannot write

* Status: accepted
* Date: 2026-11-24

## Context

The x86-64 port has had page tables since 1.0 (ADR 0007): a program is a
task of ring 3 with a space of its own, the text of the kernel and of a
program is read-only code, every page of data is NX, the boot and every
`exec` read the tables back, and a fault of a program ends that program and
not the machine. 1.9 put a guard page under every stack (ADR 0048).

What the machine did not have was a **policy**. The pieces were true one by
one and did not add up to a promise:

* the rule "no page is writable and executable" was enforced by the audit of
  an image and of the boot's map, and a program that called
  `mmap(PROT_WRITE | PROT_EXEC)` was given a writable and executable page:
  the audit had nothing to say about a page made after it;
* the stack of a program was 64 KiB and the page under it was a fault: a
  recursion deeper than 64 KiB, or one frame larger than the whole stack - a
  parser, a formatter, a descent that keeps a context per level - died on a
  machine that had 16 MiB of window;
* `brk` was charged to the memory limit of the task since 1.8 and `mmap` was
  not: a program could take any number of anonymous pages past the ceiling
  its shell had set with `ulimit -m`;
* the boundary between ring 3 and the kernel was the user bit of the entries
  and nothing else. No case exercised a store into the kernel from a
  program, and the kernel ran on the tables of the program without SMEP or
  SMAP even on a processor that has them;
* a bad access answered `vector 14, error 0x...` and a status of 255, and the
  address was in the log but the *reason* was not; a bad pointer of a call
  answered EFAULT and left no trace of which address of which program it was.

Three machines share this kernel and only one of them has an address space:
the board (ESP32) has no MMU at all (K3 in `docs/debts.md`), a program of the
board is a task that shares the memory of the kernel, and the host port
simulates a machine without page tables for a program (`k_port_mm_supported()`
is false). The policy is therefore a policy of the port, not of the kernel,
and the portable kernel may only name the rule and ask the port for the page.

## Decision

### 1. The contract, one sentence per machine

| Machine | The policy | What proves it |
|---|---|---|
| x86-64 | every page a program can name obeys the rules of this ADR: code is never writable, data is never executable, the stack grows to a limit, the heap and the mappings stop at the limit of the task, the kernel is supervisor-only, and a bad access is EFAULT with the address | `make mmu-smoke` (the attack suite), `make x86-smoke` (the audit of every program that runs), `tests/cases/test_mm.c` for the rule itself |
| host (pthread) | no address space for a program (`k_port_mm_supported()` false, `mmap` is ENOSYS, a program is a task of the kernel): the guard page of ADR 0048 and nothing else | `tests/cases/test_guard.c`, `test_mm.c` |
| board (FreeRTOS) | no MMU: a program shares the memory of the kernel, its bad pointer is a `Guru Meditation` and a reboot (K3), the overflow of a task is the canary of FreeRTOS | `make device-test`, the K3 row of `docs/debts.md` |

**What the host and the board are not checked for**, said plainly so that
nobody reads this ADR as a promise of the whole tree: on the board there is
no page to protect, no `mmap`, no ring, and a program can overwrite the
kernel - its only boundary is the byte of the image that says which machine
the bytes are for. On the host a program is a function of the kernel with a
stack of its own; the `mmap` of its library is ENOSYS and no test of the
policy runs there. The rule of W^X is tested on the host as a *rule of the
kernel* (`k_mm_prot_allowed`), because the refusal has to be the same on
every machine that ever grows a space, but no page is refused on the host
today.

### 2. A page is code or it is data

The rule is stated once, in the portable kernel, and enforced at the only
door a program has for a new protection:

* `bool k_mm_prot_allowed(k_u32_t prot)` is false for
  `K_MM_PROT_WRITE | K_MM_PROT_EXEC` and true for the five other
  combinations. A machine of the future that can give a program a page
  inherits the refusal with the call;
* `k_mm_map()` answers `EINVAL` for such a mapping before a page is made, so
  `mmap(PROT_WRITE | PROT_EXEC)` is a refusal of the call and not a page the
  next audit would find. The program keeps running: it is a wrong request,
  not a wrong access;
* the image of a program is not a mapping a program asks for: the port maps
  the header and the text with `read | exec`, the data and the bss with
  `read | write`, and refuses an image whose text does not end on a page
  (ADR 0007 §9). The audit of `exec` and the audit of the boot stay, and
  they now guard a map that has no such page to find;
* a page that becomes writable - the first write of a copy-on-write page, a
  page the kernel has to write as data - keeps its NX bit, and a page that
  is code is shared read-only by a fork and never turned into a private
  writable copy by the first write of either side.

There is no `mprotect` in the ABI, so a program cannot ask for a second
protection of a page it already has; the day the call arrives it goes through
`k_mm_prot_allowed()` like `mmap`.

### 3. The stack grows into a room

The top of the window of a program is, from the top down:

```
USER_LIMIT                 the top of the window (0x03000000)
  stack                    the first 64 KiB, mapped by the exec
  ...                      the room: 1 MiB, made page by page on the fault
  STACK_FLOOR              the lowest page a grown stack may own
  guard                    one page, never mapped
  restorer                 one page of ring 3 code, the return of a handler
USER_TOP                   the ceiling of the heap and of `mmap`
```

`USER_TOP` is the address of the restorer and it moved down by the room: the
heap and the mappings of a program lose 1 MiB of the 16 MiB of the window,
and the image of a program (8 MiB at most) still fits with the arena, the
arguments and the vectors.

A data fault of ring 3 that names a page in the room and no mapping of the
program (the fault of a `push`, of a `sub rsp` followed by a store, of a
frame larger than the mapped stack) is a **growth**, answered by mapping
every page from the fault up to the lowest page the stack owns, writable and
NX, and retrying the instruction. The lowest page is read from the tables and
not from a record: a fork copies the tables, an `execve` makes them again,
and a page the program already has cannot be taken twice.

The limit is the task's. `KSYS_RLIMIT_MEMORY` (`ulimit -m`) is the ceiling
of the stack as one object: a task that lowered it to 128 KiB has a stack
that stops at 128 KiB however much room the port reserved, and a task at the
default of 2 MiB gets the whole 1 MiB of room. A fault below the limit - the
guard page and the restorer under it - is the fault of the program, and so is
a fetch into the room: an instruction that jumps under the stack pointer is
not a frame of a call. A stack overflow is therefore not an answer of a call;
it ends the task, as POSIX leaves a `SIGSEGV` of a stack.

A growth of more than one page is a line of the log
(`x86-64: the stack of the program (pid N) grew by K page(s) to 0x...`): it
is the only place the promise is visible, because the program sees the same
address it always had. A recursion that walks down one page at a time is not
logged - that is how a stack is used, and a line per page of a deep recursion
is a log nobody reads.

### 4. The heap and the mappings stop at the same ceiling

`brk` has been charged to the memory limit since 1.8 (the allocator charges
the task, and `sys_brk` checks before it moves the break). The anonymous
mapping is the other way of asking for memory, and it is counted in the same
budget:

* a mapping that is anonymous and private, asked by the program whose space
  it is (`handle == k_port_mm_current()`), is checked against
  `task_memory_fits(self, mapped + len)` and refused with `ENOMEM` when it
  does not fit; `struct k_mm_space::mapped` is the number of such bytes and
  the region carries the mark `K_MM_CHARGED`;
* the bytes go back to the budget when the mapping is unmapped and when the
  space is released, so a program that maps and unmaps one window in a loop
  pays for the window and not for the number of windows it ever had;
* a mapping the *kernel* makes in the space of another task - the bss of an
  image while `execve` places it - is not charged: it is the program's memory
  and not the shell's.

The two calls now stop at the same number, which is what `ulimit -m` of the
shell says. What the model is not: the stack is limited by the same number
but not *counted* in `mapped`, `mem_used` is the heap and the blocks of the
allocator, and a `fork` copies the count of the anonymous mappings without
charging the child's task. A total `RLIMIT_AS` of every page of a program is
a debt (`docs/debts.md`): it needs an accounting of the frames of the port
and a decision about whether a page of a file counts.

### 5. The kernel is supervisor-only, and the processor is asked for the two bits

The user bit of an entry is checked at every level of the walk, and the
kernel is at the top of the address space in a PML4 entry that carries no
user bit (ADR 0007). That is the boundary ring 3 cannot cross, and it is a
rule that needs no feature: `/bin/mmu_kernel` writes the first page of the
image, meets a page fault whose error code is `0x7` - present, write, ring
3 - and dies with EFAULT. The error code is the point of the case: a page
that was not mapped at all would answer `0x6`, and the kernel would be
missing rather than protected.

The other direction - the kernel reaching into the memory of a program - is
what SMEP and SMAP are for, and the port asks:

* CPUID leaf 7 EBX bit 7 (SMEP) and bit 20 (SMAP) are probed once per core in
  `port_x86_64_paging_init_cpu()`, and CR4 gets the bit that the processor
  advertises. A processor without them boots and runs the same kernel, and
  the boot says which of the four cases it is:
  `x86-64: SMEP and SMAP are on, ...` or
  `x86-64: this processor has no SMEP or SMAP, the supervisor bit of every
  kernel page is the boundary`;
* SMEP needs nothing of the kernel: no entry of it executes a byte of ring 3
  (the restorer of a signal handler runs in ring 3), and a stray jump into
  the window is now a fault;
* SMAP needs a window around every copy the kernel makes through a pointer of
  a program. The kernel names it as a pair of calls of the port contract
  (1.14): `k_port_exec_user_access_begin()` and `_end()`, STAC and CLAC on
  this port and nothing on the board, the host and the null port. The window
  is opened after the range was checked with `k_port_exec_contains()` - the
  copy that `copy_from_user`, `copy_to_user`, `path_check`, the `stat`
  family, the terminal and the signal frame make - and it is closed at once;
* a read and a write of a program are the two places where the kernel hands
  a buffer to a layer under it instead of copying it (ADR 0007 §8: the VFS
  reads and writes the buffer of the caller directly). Their window is the
  call itself, which is as wide as the operation the program asked for. The
  narrower window is a bounce buffer at the edge and a change of the whole
  VFS; it is a debt, and the edge is where SMAP would want it;
* AC does not survive a context switch: `commit_switch()` closes the window
  on every switch, so a task taken down in the middle of a copy does not
  leave SMAP open for the next one.

The four path calls that validated a name and handed the pointer to the VFS
(`mkdir`, `unlink`, `chmod`, `chown`, and `rename` with two names) now copy
the name into the kernel's own window first, the shape `open` and `stat`
have had since ADR 0007. A VFS that only reads memory of the kernel is the
other half of the same promise.

One hole of the old edge is closed with it, and it was not a page: the calls
20 to 22 (`malloc`, `free`, `realloc`) hand out blocks of the heap of the
kernel, and a program that guessed an address and freed it would give a block
the kernel still holds back to the allocator. They are not part of the ABI of
a program - the library of a program allocates from its own arena and from
`brk` - so a caller with an address space is refused (a zero, the failure the
calls already answer, and one line of the log), and a task of the kernel, the
shell and the init of the boot, keeps them.

### 6. A bad access is EFAULT with the address

Two shapes, one answer:

* a **fault** of a program (a store into its text, a fetch of its stack, a
  store into the kernel, a page it does not own, a frame past the ceiling)
  prints the line of the fault with the vector, CR2, the error code and the
  RIP - as it has since 1.0 - and a second line that names the program and
  the address and says EFAULT; the task then ends with status 255 and the
  shell answers the next line;
* a **bad pointer of a call** (a range that is not memory of the program, at
  a call or during the byte-by-byte walk of a path) is `KERR_FAULT` - POSIX
  EFAULT, 14 - and the port writes the address, the size and the pid to the
  log before it answers. A program that loops on the same bad pointer fills
  the log; that is the price of naming the address.

The reason is not a new error code: EFAULT is what POSIX calls a bad address
and what this system has answered at the edge of a call since ADR 0007. What
2.0 adds is that the same word is now said for a fault of the program and
that the address is in the log in both cases.

### 7. The gate

`make mmu-smoke` (new, in `tools/release_gate.sh`) boots the machine of
`-cpu qemu64,+smep,+smap` and types the seven attacks of
`ports/port_x86_64/programs/mmu_*.c`: the text that is written, the stack
that is executed, the kernel that is written, the page that is not owned,
the heap past the limit, the mapping that is writable and executable, and
the stack of 128 KiB. Every death is read for the vector, the error code,
the address, the EFAULT and the status; every refusal of a call is read for
the code of POSIX; the growth is read in the log; a program is run after
every death; and the audit line of every program that ran is counted - the
count of the audit is the count of the `exec`, and not one of them was
refused. `MMU_CPU=qemu64` runs the same suite on a processor without the two
bits, which is the other half of the promise of §5.

`make x86-smoke` keeps the audit over its own run (the text, the data, the
kernel's map and the W^X line of every program), `tests/cases/test_mm.c`
checks the rule of §2 on the host, and the gates of the release are unchanged
in number: this lane adds one smoke and no change of the ABI.

## Consequences

**Good**

* A program cannot ask for a page that is code and data at once. The rule is
  the call's answer and not an audit after the fact, and the refusal is the
  same on every machine that ever has a space.
* A program with a deep frame works: the stack grows by up to 1 MiB (64 KiB of
  first mapping and 1 MiB of room, 1088 KiB in all), page by page, and a task
  that sets its own limit has the stack that limit promises.
* `ulimit -m` is one ceiling for the heap and the anonymous mappings, and a
  program that maps and unmaps does not spend it on history.
* Ring 3 cannot read or write a byte of the kernel, on every machine, by the
  user bit; on a processor of 2014 or later the kernel also cannot run or
  read a page of a program outside the window it names, and the boot says
  which machine it is.
* A bad access has one word, EFAULT, and an address in the log, so a person
  reads which pointer of which program was wrong.

**Bad**

* The policy is the port's: the board and the host have no page to enforce it
  on, and this ADR is the sentence that says so. A program of the board still
  overwrites the kernel if it is wrong.
* The window of SMAP around a read and a write is the whole call, because the
  VFS touches the buffer of the program directly; a bounce buffer would be
  narrower and is a change of the file layer.
* The stack is limited by the memory limit but not counted in it, and the
  anonymous mappings are counted per space and copied by a fork. A total
  `RLIMIT_AS` is a debt.
* The room of a stack is a constant of the port (1 MiB) and the window lost
  1 MiB to it: a program of the future that needs 12 MiB of image and bss at
  once will need the number changed, and the number is in one header.
* A stack that grows is a stack a wild pointer can grow: a store into the
  room is a page and not a fault. The room is the task's own and bounded, so
  the worst case is the task spending its own ceiling.
* SMEP and SMAP are features of the processor, and a machine of QEMU without
  them (`qemu64`) tests the supervisor bit only; the suite of the gate names
  the difference instead of hiding it.
