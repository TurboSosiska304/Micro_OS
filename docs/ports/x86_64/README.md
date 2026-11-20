# The x86-64 port: bare metal in QEMU

A port of the kernel for a machine with no operating system under it: no
libc, no POSIX, no firmware. It is the proof that the kernel is portable, and
the machine where a program can really be loaded and run (the window of
`execve` is here).

| | |
|---|---|
| port | `ports/port_x86_64` |
| loader | `loader32.S`: a multiboot image that QEMU loads with `-kernel`; it copies the kernel to 16 MiB, builds the identity map of the first gigabyte and turns the long mode on |
| console | COM1, 115200, `\r\n`, blocking reads with `hlt` |
| clock | the PIT at 1 kHz; critical sections save and restore the flags |
| tasks | `switch.S` (callee-saved registers and the stack) and a round robin over the ready queue |
| programs | `k_port_exec_supported()` is true; the window is 32 MiB at 32 MiB; the jump is the current milestone |
| build | `make x86-build` (needs `cmake`, `ninja`, `xorriso` is not needed) |
| check | `make x86-smoke` (needs `qemu-system-x86`) |

## Run it

```bash
make x86-build
make x86-run      # QEMU, the serial console, the shell
make x86-smoke    # boot and check the lines of the banner and the shell
```

The image is two files: `build-x86/loader32.elf` (the loader) and
`build-x86/microos64.bin` (the kernel, a flat image). The heap of the kernel
stops below the window of the programs; the kernel is linked at 16 MiB and
the programs are placed at 32 MiB.

## What works

* the boot: the loader, long mode, the banner, the contract check at 1.3;
* the console, the clock, the interrupts (IDT with 32 exceptions, PIC, PIT),
  the context switch, the scheduler, `ps`, `uptime`, `free`, `dmesg`;
* the shell on COM1, with `help`, `uname`, `uptime`, `ps`, `free`, `memtest`
  and the rest of the portable commands.

## What does not work yet

* no filesystem and no network: `ls`, `cat`, `write` answer an error because
  nothing is mounted (the VFS is there, the port has no block device yet);
* `execve`: the format (MOX1) and the window are in place, the jump is the
  next milestone;
* the page tables per task: the identity map of the loader is the first step,
  not the model (`KERNEL_NOMMU` is still 1).

## Why it exists

The kernel is portable only if something other than the board runs it. This
port caught two things the board never would: a build that needed
`-U_FORTIFY_SOURCE` on a distribution that fortifies by default, and the
execution of the kernel on a machine with an MMU that is not used yet.
