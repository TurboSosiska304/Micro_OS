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
(PML4, PDPT, PD and eight PTE tables). One set is built per `execve` and
destroyed when the program ends; the next one gets fresh, zeroed tables.
Since K4 the set belongs to the *task* that runs the program (see section 7),
so a parent and the child of its fork have two sets over the same frames.

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

### 7. Update (K4): a program is a task, and fork copies it

The program stopped being a call that stays on the stack of its caller. It is
a task of the scheduler now: `k_port_task_create()` gives it a kernel stack,
the first body of the task switches CR3 to the space of the program and drops
to ring 3 through `iretq`, and `exit` or a fault ends the task the way any
task ends. `k_port_exec_enter()` makes the task and waits for it with
`task_wait()`, so the shell that asks for a program is asleep until that
program is a zombie, exactly as a parent waits for a child.

The address space belongs to the task, not to the program: `commit_switch()`
writes the CR3 of the next context and its `RSP0`, and a task that ends hands
its tables and frames to the next context to release. The frames of the
window come from a frame allocator over the arena (with a reference count,
because a fork shares them), and the tables still come from the pool of pages
in the image.

`fork()` is real with contract 1.5. `k_port_fork_copy(struct task *child)` is
the seam: the kernel has made the record and the pid, the port clones the
tables of the parent, points both copies at the same frames, clears the write
bit and sets the software copy-on-write bit in both. The frame of the
`int $0x80` that is being served is copied to the top of the child's kernel
stack with zero in the slot of its result, so the child's first iretq lands
at the instruction after the call with the answer 0; the parent returns with
the pid. The page fault handler copies the 4 KiB page and retries the
instruction.

`CR0.WP` is set, so a write of the *kernel* to a copy-on-write page (the
status of `waitpid`, the buffer of a `read`) takes the same path: a kernel
that wrote through the shared frame would corrupt the child at the edge of
the system call. The exception stub now saves the registers and can return,
which is what makes the retry possible.

### 8. Update (K5): the edge of the system call copies

The privilege boundary of section 1 stops a program from reaching the kernel,
but it did not stop the kernel from trusting the program's pointers. A call
ran on the program's CR3, so a valid address of the window was readable and an
invalid one raised a page fault in ring 0 - and ring 0 has nowhere to return
to, so one bad `write` stopped the machine.

The contract 1.6 adds one seam, `bool k_port_exec_contains(const void *ptr,
k_size_t size)`, to `kernel/port/exec.h`:

* a port whose programs share the kernel's memory (the board, the host, the
  null port) answers true: every pointer a call receives is the kernel's own;
* the x86-64 port answers true when the caller is a kernel task, and
  otherwise walks the page tables of the running program page by page. The
  null page, the guard page under the stack, the part of the window that was
  never mapped and a range that ends past `0x03000000` are all "not there";
  a copy-on-write page is there and is accepted, because the fault of the
  first write already has a handler.

`components/kernel/src/ksys.c` keeps two helpers, `copy_from_user()` and
`copy_to_user()`, which ask the port and then `memcpy`; a false answer is
`EFAULT` (`KERR_FAULT`, POSIX 14), the answer of the call and not a fault of
the machine. Every pointer of the ABI goes through them: a path is checked
byte by byte and copied into a buffer of the kernel before the VFS sees it,
`argv` is copied word by word and string by string (never past the frame),
the buffers of `read`, `write`, `readdir`, `stat`, `getcwd` and the status of
`waitpid` are range-checked before a filesystem writes to them. The four path
calls that other work owns (`mkdir`, `unlink`, `chmod`, `chown`) share
`path_check`, which now checks every byte it reads, so they are safe without
being rewritten.

The failure is covered on the machine: `/bin/badptr` hands `write`, `stat` and
`read` an address outside the window, the last four bytes of the window with
a request that runs past it, and the guard page; every call answers 14
(`EFAULT`) and the shell answers the command after the program. On a machine
without an address space there is nothing to check, and the host tests keep
the valid side of the edge: the paths, the buffers and the vector of `execve`
are copied as before.

## What is not done

* The kernel is not in a higher half. It keeps the identity map of the first
  gigabyte, so the window of the programs is a hole in the kernel's map and
  not the other way round. The plan is in the next section; it is a change of
  the loader, the linker script and `paging.c`, and it is deliberately not
  mixed with the edge of this pass.
* No SMAP: the check accepts the memory of the program and the kernel still
  reads it with the processor in ring 0. A pointer that becomes invalid
  between the check and the copy cannot happen on this single processor, but
  SMAP/SMEP (and NX) are the hardware half of the same rule and are still
  ahead.
* The four path calls of K4 copy nothing: they validate the range and hand
  the pointer to the VFS. The window cannot change under a call on this port,
  so the check is enough there; the copy is the shape the other calls use and
  the one a policy with SMAP would need everywhere.

## The higher half: a plan, not taken

Moving the kernel to `0xFFFF800000000000` is a change of the port, not of
`components/kernel`, and it has a fixed shape:

1. **The linker.** `linker64.ld` sets `KERNEL_BASE = 0xFFFFFFFF80000000` (the
   top 2 GiB, where the small model of the kernel fits) and keeps
   `KERNEL_PHYS = 0x01000000`. The build adds `-mcmodel=kernel`, since the
   small model cannot name an address above 2 GiB.
2. **The loader.** `loader32.S` already builds the PML4/PDPT of the identity
   map before the switch to long mode. It adds one PDPT entry (index 510 of
   PML4 entry 511) mapping the physical range `0x01000000` and up to the same
   virtual range in 2 MiB pages, and jumps to `KERNEL_BASE + _start64`
   instead of the physical entry. The low identity map stays: the VGA at
   `0xB8000`, the arena at `0x04000000` and the multiboot information are
   still reached at their physical addresses.
3. **The link between the two.** `page_phys()` in `paging.c` stops being the
   identity and becomes `virtual - KERNEL_BASE + KERNEL_PHYS`; every static
   page table, the GDT and the TSS are linked high, and `table_entry()` keeps
   returning the low identity because the kernel is mapped there too. The
   kernel's own map is then "the first gigabyte identity, supervisor only"
   plus "the kernel image at the top", and a program's map is a copy with the
   user window, exactly as now.
4. **The boot and the stacks.** `boot64.S` takes the high entry, the kernel
   stack and the double-fault stack are high addresses, `kmain` keeps the
   multiboot pointer as the physical address it is, and the fault handler's
   dump prints high addresses.
5. **The tests.** `make x86-smoke` is the gate: the banner, the program in
   ring 3, the fork and the fault of `/bin/crash` have to pass unchanged. A
   separate check reads a symbol from the ELF to prove the link address is
   the high one, so a linker script that quietly falls back is noticed.

The risk is concentrated in the loader and in `page_phys()`: one wrong entry
or one missed conversion and the machine triple-faults before the console, so
the work is a branch of its own with the smoke test at every step, not a
change of a pass that also touches the edge of the ABI.
* No NX, no SMEP/SMAP, no PCID, no SMP: every entry flushes the TLB and the
  map assumes one processor.
* `KERNEL_NOMMU` is still 1 for the whole tree. Turning it off is a change of
  the generic kernel (and of the board's concessions), not of this port alone.
* The open files are not inherited by the child of a fork, and a program that
  calls `execve` starts a second task and waits for it instead of replacing
  itself: both are the program-as-task model taken one step at a time, not
  the POSIX shape yet.
* The ARM port with an MMU is still a plan.

## Consequences

**Good**

* The x86-64 port has a real privilege boundary: the kernel is supervisor
  only, ring 3 can reach the window and its stack and nothing else, and the
  address zero is not mapped.
* A bad program is a line and a status, not a dead machine; the shell that
  ran it is alive.
* `exit` from a program ends its task, the caller of `execve` gets the status,
  and `execve` keeps answering 0.
* `fork()` returns the pid in the parent and zero in the child, the child has
  a page table of its own and the first write splits a shared page; `waitpid`
  and the shell see the status (K4).
* Each task has its own `errno`.

**Bad**

* The address space, the kernel stack and the frame of a task belong to the
  port; the generic kernel still has `KERNEL_NOMMU` and the board keeps the
  placeholder for `fork()`.
* The kernel's own map is still the low identity map; the road to a higher
  half is planned and documented, not taken.
* The pointers of a program are checked and copied at the edge (contract
  1.6), but without SMAP: the check and the copy are the same page tables,
  and the hardware half of the rule is still ahead.
