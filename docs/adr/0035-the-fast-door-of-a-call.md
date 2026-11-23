# ADR 0035 - The fast door of a system call

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0006 (the entry of a program), ADR 0007 (the map and ring 3),
  ADR 0012 (the cores), `docs/syscall-abi.md`, `ports/port_x86_64/src/`
  (`interrupts.S`, `syscall.c`, `gdt.c`, `cpu.c`)

## Context

ADR 0006 gave a program of the x86-64 port its door: the vector 128 of the
table of the interrupts, `int $0x80`. It chose the instruction deliberately and
named the alternative in the same paragraph: "It is not `syscall`: that
instruction ... needs the MSRs `IA32_STAR`, `IA32_LSTAR` and `IA32_FMASK`
programmed with two segment selectors and a flag mask. This kernel has no ring
3 to return to and no per-processor state to keep, so the MSRs would describe a
machine the port does not have yet." It also promised the shape of the day the
port grew that door: "`RCX` and `R11` are left out because the instruction
`syscall` keeps them for itself: the day the port grows that door, the
arguments do not move."

Both of the missing pieces exist now. The program is ring 3 code with page
tables of its own (ADR 0007), the port has a per-core record under
`IA32_GS_BASE` (ADR 0012), and the TSS already names the kernel stack of the
program on its way in. What the gate still costs is the crossing itself: the
processor reads a descriptor of the IDT, checks its privilege, loads `RSP` and
`SS` out of memory, pushes five words, and the stub returns by `iretq`, which
reads four words and two descriptors again. On the emulated machine of the
tests a call is about a microsecond, and a program of the tree makes hundreds
of thousands of them (`grep`, `zcat`, `lua`, the shell of a build).

## Decision

### 1. `syscall` in, `sysret` out, and the vector stays

The port programs four registers of every core, once for the boot at the end of
`port_x86_64_interrupts_init()` and once per application processor on its way
up (`src/syscall.c`):

| Register | What it carries |
|---|---|
| `IA32_EFER.SCE` | bit 0: the instruction `syscall` exists in 64-bit mode |
| `IA32_STAR` | `[31:16]` = 0x08 (kernel code; SS is 0x10), `[63:48]` = 0x30 |
| `IA32_LSTAR` | the address of `isr_syscall_fast` |
| `IA32_FMASK` | `DF | IF | TF | AC | NT` = 0x44700 |

`IF` in the mask is not decoration: the frame of the call is not on the stack
yet when the first instruction runs, and an interrupt taken there would push
its frame over the stack of the program, which is not a stack of the kernel.
The stub turns the interrupts on itself once the frame is safe, exactly as the
gate did, so `sleep` and `yield` still really wait.

`SYSRET` is told one number and derives both selectors from it - CS is
`STAR[63:48] + 16`, SS is `STAR[63:48] + 8` - and the two ring 3 descriptors of
the iretq frames (0x18 code, 0x20 data) are in the other order. Swapping them
would rewrite every frame of a signal, a fork and an exec, and would change
numbers the rest of the port and the documentation name; so the GDT carries a
second pair of ring 3 descriptors (0x38 data, 0x40 code) laid out the way the
instruction wants it, and nothing else in the port moved.

The vector 128 stays, with its handler of the same frame. It is what a program
that was not built against this library calls, and it is what the restorer of a
signal handler returns through (`port_exec.c` writes those bytes). Both doors
build the same 160-byte frame in the same order (`PORT_FRAME_*`), run the same
body and leave through the same tail, which picks the instruction: `sysret` for
a program's frame, `iretq` for anything else.

### 2. Where the entry finds a stack

`syscall` changes no stack. The entry takes it from the record of the core that
`IA32_GS_BASE` points at - the record the port already keeps for
`k_port_cpu_id()` - in four instructions:

```
    movq %rsp, %gs:PORT_X86_64_CPU_USER_RSP   # the stack of the program
    movq %gs:PORT_X86_64_CPU_KERNEL_RSP, %rsp # the stack of the call
```

`KERNEL_RSP` is not a second truth: `port_x86_64_tss_set_rsp0()` writes the
number into RSP0 of the TSS and into the record in one function, and it is the
single place every entry into ring 3 goes through (a switch to a program, the
first entry of an image, a fork child). The offsets live in
`port_x86_64_asm.h`, a header of preprocessor definitions only - the assembler
cannot read a header that names `k_u64_t` - and `src/cpu.c` asserts every one of
them against `offsetof()`, so a field that moves is a build that stops.

The frame is built by hand in the order the processor would have pushed it,
with `RCX` (the address of the call) and `R11` (the flags) in their own slots:
the instruction keeps them, so the frame has no other value for the two and a
signal frame saved out of it shows the address and the flags of the program.

### 3. The return is guarded, and the guard is not paranoia

The tail checks the two words that can hurt a `sysret`: `RCX` must be a
canonical address (a shift of 16 bits and back is the test) and the selector of
the frame must be the ring 3 one. If either fails, the frame is returned with
`iretq`, which reads the selector itself and faults with the stack of the
kernel still in the register.

The frame a `sigreturn` restores is memory the program can write, so a program
can name an address the instruction cannot return to. With `iretq` that is a
#GP in ring 0 on the stack of the kernel, and the port takes the task down the
way it takes down any fault. With an unguarded `sysret` it would be a #GP in
ring 0 *after* the stack of the program is in `RSP`, which is a fault handler
running on a stack the program chose.

The last fifteen instructions run with the interrupts off (`cli`), because the
stack of the program is loaded before the instruction that uses it. `sysret`
restores the flags of the program from `R11` at the transition, which is the
same place `iretq` restores them from the frame.

### 4. What does not change

* The numbers and the semantics of the calls (`kernel/ksys.h`), the register
  convention, and the argument order. `RCX` and `R11` were already clobbered
  in the library and are clobbered by the instruction.
* The frame: 160 bytes, the same offsets, so `k_port_task_signal_enter()` /
  `leave()`, the child of a `fork()` and the frame replacement of an `execve`
  work untouched.
* `execve` of a program that replaces its own image: it rewrites the frame at
  the top of the kernel stack (RIP, RSP, RFLAGS, CS, SS) and the tail returns
  through it - `sysret` for a canonical entry and the ring 3 selector, `iretq`
  otherwise. Nothing in `port_exec.c` changed.
* The board and the host: the door of the port is a pointer to `ksys_call()`
  there (ADR 0018), and this is a decision of one port.

## Consequences

**Good**

* A `getpid()` of a program of this machine costs about 0.55 µs instead of
  0.92 µs on the QEMU of the tests - 1.8 million calls a second instead of 1.1,
  a factor of 1.6 (the numbers and the tool are in the commit message and in
  `ports/port_x86_64/programs/bench.c`).
* The door of a program is now the door Linux has: a program of somebody else,
  or a person reading the disassembly of one, sees the instruction it expects.
* The `int $0x80` path got the same fast return for free: both doors leave
  through the one tail.

**Bad and open**

* Two doors and a copy of the ring 3 selectors in the GDT: a person reading
  `gdt.c` has to know why 0x38/0x40 exist, which is what this ADR is for.
* The record of a core carries two words that only the entry of a call reads;
  a port of one core still pays for them (16 bytes per core).
* The two instructions between the entry and the stack switch run on the stack
  of the program: the offset of the record is not a place a `mov` can be
  interrupted *only* because `IA32_FMASK` cleared the interrupt flag, and an
  NMI ignores that flag. The port configures no source of an NMI (no line of
  the I/O APIC delivers one, and the kernel raises none), so the window is not
  reachable today; a port that wants one gives the NMI an IST stack of its own
  in the TSS, the way the double fault has one already.
* The measurement is of a virtual machine. `soak-smp` and the smoke tests show
  that the fast path does not race and does not corrupt a frame, and no test
  measures a real processor's `syscall` against its `int`.
