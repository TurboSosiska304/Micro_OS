# ADR 0006 - The entry of a program into the kernel

* Status: accepted, amended by ADR 0035 (the port grew `syscall`; the vector
  0x80 stays as the second door of the same convention)
* Date: 2026-11-21

## Context

Phase 7 gave the system programs: a file in `/bin`, a format of its own
(`MOX1`, ADR 0005), a port that copies the image into a window of memory and
jumps into it. The x86-64 machine of QEMU was the first one where a program
regardless of the kernel really ran.

The program could not call the kernel. `ksys_call()` is a function of the
kernel, and a program that the kernel did not compile does not have its
address: the demonstration program of the port wrote to COM1 through `outb`
and read nothing. A program that can only touch the hardware is not a program
of this system, it is a payload.

Three facts shape the answer:

* the program is ring 0 code in the window of the port: no page tables of its
  own, no TSS, no stack switch on an interrupt, and no way to check a pointer
  it passes;
* the kernel is built without SSE and without the red zone; the general
  registers and the stack are the whole of the state;
* the port already owns the tasks and the timer, and a call that sleeps has to
  give the CPU away from inside the call.

## Decision

### 1. One vector, `int $0x80`

The port puts its stub on the vector 128 of the table of the interrupts: an
interrupt gate (type `0x8E`), present, DPL 0. The program raises it with
`int $0x80`.

The vector is the one Linux has used on this processor since 1995. It is not
`syscall`: that instruction is the modern door of a 64-bit system, and it
needs the MSRs `IA32_STAR`, `IA32_LSTAR` and `IA32_FMASK` programmed with two
segment selectors and a flag mask. This kernel has no ring 3 to return to and
no per-processor state to keep, so the MSRs would describe a machine the port
does not have yet. `int` is one instruction, its gate is one entry of a table
the port already fills, and it works the same for a program of ring 0 and -
once the address space exists - for a program of ring 3, which is the part
`syscall` cannot do without more set-up.

### 2. The registers of the call

The number of the call in `RAX` and six arguments in `RDI`, `RSI`, `RDX`,
`R10`, `R8`, `R9`; the result in `RAX`, a negative number is an error. This is
the convention of the kernel call (`ksys_call()`), the convention of the
wrappers of the library, and the convention of Linux on this machine. `RCX`
and `R11` are left out because the instruction `syscall` keeps them for
itself: the day the port grows that door, the arguments do not move.

The width is the width of a register of the machine, exactly as on the board
(where the same numbers travel in 32-bit registers).

### 3. The handler

`isr_syscall` in `ports/port_x86_64/src/interrupts.S`:

1. pushes the nine registers the caller may lose (`RAX`, `RDI`, `RSI`, `RDX`,
   `R10`, `R8`, `R9`, `R11`, `RCX`) and `RBP`, which it uses as the frame;
2. turns the interrupts on. They were off for the pushes, and the gate keeps
   them off on entry by design; a call may sleep, yield or wait for a
   character of the console, and the scheduler of the port decides by that
   flag;
3. aligns the stack to 16 bytes as the C ABI wants, puts the sixth argument
   where the ABI of the machine passes a seventh one, and calls `ksys_call()`
   with the number and the six registers;
4. writes the result over the saved `RAX`, pops the registers back and
   returns with `iretq`, which also restores the flags of the program (off
   again, because that is what they were when it raised the vector).

The handler runs on the stack of the program: a privilege change is what makes
the processor take another stack, and there is none in ring 0. That is why the
stack of a program is a window of its own with 64 KB in it (see
`port_exec.c`), and not the stack of the boot.

Everything the program does not get back is a register the C ABI calls
caller-saved, and everything it does get back (`RBX`, `RBP`, `R12`-`R15`) is
saved by the C compiler of the kernel. The program is not compiled against
the handler, so nothing of it is taken on trust.

### 4. The status of a program that returns

The demonstration program returns with `ret`, and the value of the C return is
what `RAX` holds. The port's stub (`port_exec_exit`) stores that register in a
variable of the port before the stack of the kernel comes back, and
`k_port_exec_enter()` reports it to the kernel with `k_exec_exit()` - the seam
between the two: the port owns the moment, the kernel owns the number.

`k_exec_run()` answers with the status (0 to 255, the low byte, as the status
of a process), `execve` answers 0 - it has no process to exit into and has to
notice that the program finished instead - and the port prints
`x86-64: the program exited with N`. A program that never returns never
reaches the seam, and the `execve` that ran it never returns either.

### 5. The arguments

`execve` copies at most eight words and 512 bytes of their strings into the
frame of the call and hands `k_exec_run()` that vector; the port already took
one (`k_port_exec_enter(image, argv)`, contract 1.3) and passes the count and
the pointer in `RDI`/`RSI` at the entry of the program. The frame of `execve`
stays alive for as long as the program runs, so the pointers are good for the
life of the program: one that returns ends the call, and one that never
returns keeps the stack of its caller.

The kernel does not check the pointers of the caller - without an MMU there is
nothing to check them against - but it does bound the copy, so a string that
never ends stops at the edge of the buffer instead of walking through memory.

## What is left for the next milestone (K5)

* **Ring 3.** The gate is DPL 0 because the program is ring 0 code. A program
  in ring 3 needs the gate to open for it (DPL 3), a TSS with a stack for the
  privilege change (the handler would then run on a kernel stack and not on
  the stack of the program), and a return frame the processor accepts.
* **A space of its own.** Page tables per task, the window of the programs
  mapped into the task that runs them, and a copy of the arguments at an
  address the program may read. Only then does a pointer of a program mean
  something the kernel can check (`copy_from_user`), and only then can the
  handler trust the buffer of a call.
* **`exit`.** Today a program ends by returning, and a program that never
  returns owns the machine. With a process there is a call and a status to
  wait for (`waitpid`), and `execve` stops being the last thing a caller does.

## Consequences

**Good**

* A program of the x86-64 port calls the system through one instruction and
  the same numbers as the rest of the tree; `ls`, `write`, `sleep` and
  `getpid` are the same calls for the kernel and for a file in `/bin`.
* The call wakes the scheduler: `sleep` and `yield` work from a program,
  which is what makes a program a program and not a long function.
* The status of a program comes back without a new system call and without a
  change of the contract of the port (1.3 stays): the seam is
  `k_exec_exit()`.

**Bad**

* The handler runs on the stack of the program and trusts its pointers: a
  program of ring 0 can hand the kernel any address, and the kernel will
  dereference it. That is the price of no MMU and no address space, and it is
  written down rather than forgiven.
* A program that never returns still takes the machine with it: no `exit`, no
  `fork` with its own space, no way for the kernel to take the CPU back from
  code that is not in a system call.
* The interrupts are off in the code of the program itself: a program that
  computes for a second is not preempted. A program is not a task of the
  scheduler until the address space makes one possible.
