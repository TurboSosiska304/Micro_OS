# Programs

A program in Micro OS is a **file**, not a function of the kernel: `execve`
reads the file through the VFS, the kernel checks it against a format of its
own, and the port places it in memory and enters it. The decisions are in
[ADR 0005](../adr/0005-programs-and-protection.md) (the format and the rights),
[ADR 0006](../adr/0006-program-entry.md) (the entry) and
[ADR 0007](../adr/0007-paging-and-user-mode.md) (the address space and ring 3);
the report of the phase is [`docs/phases/phase7.md`](../phases/phase7.md).

Only one machine of the tree can run a program:

| Machine | `k_port_exec_supported()` | What `execve` answers |
|---|---|---|
| x86-64 in QEMU | true | it runs the image in ring 3 |
| ESP32 board | false | `ENOSYS` (the flash is not executable for a file) |
| host simulator | false | `ENOSYS` (a test harness must not jump into an image) |
| null port | false | `ENOSYS` |

On the board and in the simulator the shell still has `exec`; it looks for the
file, checks the execute bit, and then reports that this machine cannot run a
program. That is the contract working, not a missing build.

## The format: MOX1

An executable is a flat image: a header of 32 bytes, then the code, then the
initialised data. The bss is not in the file; the port clears it. The layout is
in [`components/kernel/include/kernel/exec.h`](../../components/kernel/include/kernel/exec.h):

| Offset | Field | Meaning |
|---|---|---|
| 0 | `magic` | the bytes `M`, `O`, `X`, `1` |
| 4 | `version` | 1 |
| 8 | `flags` | 0; a flag the kernel does not know is a refusal |
| 12 | `entry` | the offset of the first instruction, from the start of the image |
| 16 | `text_size` | the bytes of code |
| 20 | `data_size` | the bytes of initialised data |
| 24 | `bss_size` | the bytes to clear, at most 1 MiB |
| 28 | `reserved` | 0 |
| 32 | | the text, then the data |

The parser (`components/kernel/src/exec.c`) checks the image against itself
and nothing else: the magic and the version, the sizes against the file, a
text that is not empty, an entry inside the text, sizes that are multiples of
four, a known flag, and a bss under a megabyte. Whether the image fits the
window of the machine is the port's question, asked later.

`tools/mkprog.sh` turns a linked object into an image:

```bash
tools/mkprog.sh [--base 0x02000000] hello.elf hello.mox hello_main
```

The image is not relocated: the code and the data have to be linked for the
address the port names, and `--base` makes the script refuse an image linked
for another window. The x86-64 port builds its four programs this way: the
object is compiled for the window, linked with `programs/program.ld`, turned
into a `.mox` by the script and embedded in the kernel as a blob, because the
kernel needs the bytes before it has a filesystem to read them from.

## `/bin` and the boot

The x86-64 port mounts MicroFS at `/`, creates `/bin`, writes four images
there and gives them the mode of a program (`0755`) through the VFS:

| File | What it proves |
|---|---|
| `/bin/hello` | the door: `write`, arguments, pid, clock, `sleep`, `exit`; it prints `cpl 3` |
| `/bin/crash` | a fault of ring 3 is reported and the shell survives |
| `/bin/fork` | `fork()` with copy on write: the child gets 0, the parent the pid |
| `/bin/badptr` | a pointer outside the window is `EFAULT`, not a fault of ring 0 |

The boot then asks for `/bin/hello` with `ksys_execve()`: the stat, the check
of the mode, the read through the VFS, the parse and the entry are the ones of
the call and not a copy of them. The shell runs the same file with `exec`.

## `exec` at the console

```text
microos> exec hello
PROGRAM: hello from /bin
PROGRAM: cpl 3
PROGRAM: argc 1, argv[0] hello
PROGRAM: pid 4, up 2869 ms
PROGRAM: slept 10 ms, up 2880 ms
the program has finished
microos> exec crash
PROGRAM: crash writes to address zero
the program faulted: vector 14 at 0x0000000000000000
the program has finished
```

A name with a slash is a path; a name without one is looked up in `PATH`, where
the first regular file with an execute bit wins. The vector of the arguments is
the line after the name, with `argv[0]` the name that was typed, as in Linux.

The port also prints the lines of the task (`task: created 'program' ...`,
`x86-64: the program exited with 0`); they are left out here. The shell waits:
`execve` returns only when the program has ended (or when it never started),
and the port prints the status the program ended with (0 to 255).

The errors, in the order they are met:

| Answer | Reason |
|---|---|
| `ENOENT` | the path names nothing |
| `EISDIR` | it is a directory |
| `EPERM` | the file has no `x` bit for the caller (`0400` is not a program) |
| `EINVAL` | the file is not a `MOX1` image of this system |
| `ENOMEM` | the image does not fit the window of the port |
| `ENOSYS` | this machine has no window of programs |

A failed `execve` spends no descriptor of the caller: the file is read through
the VFS, not through the table of the task.

## The entry: `int $0x80`

A program of x86-64 is not a function of the kernel, so the call has to be an
instruction of the processor. The vector 128 is a gate of the port:

| What | Where |
|---|---|
| the instruction | `int $0x80` |
| the number of the call | `rax` |
| the six arguments | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| the result | `rax` (negative is an error) |

The registers are the ones of Linux; `rcx` and `r11` are not arguments because
the instruction `syscall` keeps them. The gate opens for ring 3. The processor
takes `RSP0` from the TSS on the way in, so the handler runs on a kernel stack
of the program, not on a stack the program could have pointed anywhere; the
handler puts the kernel's thread pointer back, turns the interrupts on, calls
`ksys_call()` and returns with `iretq`. The table of the calls is in
[`docs/syscall-abi.md`](../syscall-abi.md#the-table) and in
[kernel-abi.md](kernel-abi.md).

A program ends with `exit` (number 0), not with `ret`: a return in ring 3 has
no kernel frame to land in. The port notices the exit, unwinds to the call that
entered it and reports the status (0..255). A program that faults is printed as
`the program faulted: vector N at 0x...`, taken down, and its status is 255;
the shell that ran it is alive and answers the next command.

## The address space and `fork`

A program is a task of the scheduler with page tables of its own:

* the window is 16 MiB at 32 MiB (`0x02000000`-`0x03000000`), mapped onto a
  physical arena at 64 MiB; the kernel reaches the bytes through its identity
  map, the program through its own pages;
* the stack is 64 KiB at the top of the window, with an unmapped guard page
  below it; the null page is not mapped, so a write to address zero is a fault
  and not a silent success;
* the kernel is supervisor-only and out of the reach of ring 3; the program
  runs with `cpl 3` and calls the kernel only through the gate;
* `fork()` copies the page tables, points both copies at the same frames
  read-only, and the first write of either side faults, copies the 4 KiB page
  and takes the write bit back. The child returns 0, the parent returns the
  pid, and `waitpid()` sees the status.

On a machine without an MMU the same call is a placeholder: the child shares
the memory of the parent and re-enters the program, and the port says so.
There is no `fork()` with an address space of its own on the board, and no
plan to pretend otherwise.

## What a program cannot do

* **No ELF and no dynamic linker.** The format is `MOX1`, everything is
  statically linked, and there are no shared objects.
* **No libc of the host.** A program has the calls of the ABI and whatever the
  image carries; the x86-64 programs talk to the kernel through
  `programs/prog.h`, and the libc subset of `components/libc` is what the shell
  and the components of the kernel use, not a library a program is linked
  against.
* **No descriptors inherited by a fork**, and a program that calls `execve`
  starts a second task instead of replacing itself. The POSIX shape is on the
  list of debts.
* **No `mmap` and no `brk`.** A program cannot ask for a range of its own.
* **No protection by SMAP yet.** The edge checks the page tables of the caller
  and then reads the same pages in ring 0; the hardware half of the rule waits
  for the higher half and the flags of the processor.

The open rows are in [`docs/debts.md`](../debts.md); the plan is
[`docs/roadmap.md`](../roadmap.md), K4 and K5.

[Back to the wiki index](README.md)
