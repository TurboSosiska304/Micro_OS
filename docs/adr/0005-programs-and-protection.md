# ADR 0005 - Programs, permissions and the limits of a machine without an MMU

* Status: accepted
* Date: 2026-11-20

## Context

Phase 7 is the first one that treats a program as a program: a file in
`/bin` that the kernel loads and runs, a file that has an owner and a mode,
and a syscall table that checks its arguments because a program is no longer
just another function of the tree.

Two facts about the machines change what is possible:

* the board (ESP32) has no MMU, and its flash is not executable for an
  arbitrary file: a program would have to be copied to internal SRAM, which
  has ~100 KB for everything;
* the x86-64 port of the kernel has an MMU and RAM it can execute: it is the
  machine where a second program can really be loaded and run.

The kernel stays portable, so whatever is a property of the machine stays in
the port.

## Decision

### 1. A program has a format of its own

An executable is a flat image with a small header (`kernel/exec.h`):

```
magic "MOX1", version, flags
entry offset, text size, data size, bss size
```

It is linked at a fixed address, because without an MMU there is nothing to
relocate: the port says where a program may be placed
(`k_port_exec_base()`), and the image is copied there. No ELF: the loader of
Linux is a chapter of its own, and a kernel with 100 KB of SRAM is not the
place for it. `tools/mkprog.sh` turns an object of a program into the image.

### 2. `execve` is a duty of the port

`kernel/port/exec.h` (contract 1.3) declares:

* `bool k_port_exec_supported(void)` — can this machine run a program from
  memory at all;
* `void *k_port_exec_base(void)` — where a program may be placed;
* `int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])`
  — copy the image, enter it; the call does not return on success.

The board answers "no" and is honest about it; the x86-64 port loads and
jumps. The kernel parses the image, checks it against the base and the size
of the machine, and asks the port to run it. On the board `execve` returns
`ENOSYS`, and the shell says why.

### 3. Permissions without a second user

MicroFS inodes keep their 16 bytes: the mode and the owner are packed into
the upper bits of `flags` (the low two are the type).

* mode — 9 bits, `rwx` for the user, the group and the others, as in Linux;
* owner — 15 bits, a uid.

There is one user on the board (uid 0), so the owner is a number that is
written and checked, not a part of the design of the system yet. New files
get `0644` (`0755` for directories); `chmod` and `chown` are commands and
syscalls. The check of the mode happens at the edge of the system (`open`,
`stat`), with the descriptor of the caller, exactly where the read/write
access is checked today.

This keeps the on-disk format of the volume of 0.3.1: no silent reformat in
the name of a new field.

### 4. `fork()` with its own address space waits for the paging of a port

A real `fork()` needs page tables; the board has none. The kernel does not
pretend: the placeholder stays on the board, and the address space of its own
is the next step of the x86-64 port (page tables per task, copy on write),
not a change of the generic kernel. The syscall keeps its number.

### 5. `fork()`: the ABI in the kernel, the mechanism in the port

The number, the answer (the pid in the parent, 0 in the child) and the
waiting are the contract of the kernel - they do not change per machine. What
happens under the call is the business of the port, because it is the port
that owns the address space:

* the board, without an MMU, can only share the memory of the parent and
  re-enter its program: the child is a task that starts at the entry of the
  parent, and `KERNEL_NOMMU` says so;
* a port with an MMU (the x86-64 one, once its paging is done) copies the
  page tables of the parent, marks both copies read-only and copies a page on
  the first write - the child then continues at the instruction after the
  call, as POSIX promises.

So the kernel keeps one implementation of the *shape* (`sys_fork` decides the
pid, the parent relations and the waiting) and asks the port for the
*substance*: copy the context of the caller (`k_port_fork_copy()`, 0 when the
caller is the child, the pid of the child in the parent, -1 when the machine
cannot). Today only the first behaviour exists; the second is the milestone
of the x86-64 paging.

### 6. The syscall table is the only door

With programs that the kernel did not compile, every argument is checked at
the edge: a pointer must be inside the memory the caller may touch, a length
must be sane, a path must be a path (not a hole of the kernel). The checks
live in `kernel/ksys.c` and are covered by tests with negative cases.

## Consequences

**Good**

* The kernel stays one tree and one contract; the board gets permissions and
  a clear "this machine cannot run a program", the x86-64 port gets programs.
* No on-disk format change: the volume of 0.3.x survives.
* The syscall edge becomes real, which is the point of the phase.

**Bad**

* A program is not relocatable: it is linked for the base of the port.
* `fork()` on the board remains a placeholder for at least one more phase.
* The format is ours, so the tools of Linux (`gcc`, `objcopy`) need a wrapper
  (`tools/mkprog.sh`).
