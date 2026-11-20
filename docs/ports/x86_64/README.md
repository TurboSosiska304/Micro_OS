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
| store | `ram0`: 512 KB of the memory of the machine in sectors of 1 KB, MicroFS, mounted at `/` |
| programs | `k_port_exec_supported()` is true; the window is 16 MiB at 32 MiB (`0x02000000`-`0x03000000`); a program is a file in `/bin` (the MOX1 format of `kernel/exec.h`) |
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
the programs are placed at 32 MiB. The program of `/bin` is made first:
`build-x86/hello.mox` is the image of `ports/port_x86_64/programs/hello.c`
(`programs/program.ld` links it for the window, `tools/mkprog.sh` turns it
into the format) and the kernel carries it as a blob, because the machine has
to have the bytes before it has a filesystem to read them from. The build
passes `--base 0x02000000` to `mkprog.sh`, which refuses an image linked for
another window: nothing relocates the program, so a wrong address would run
and read its data from somewhere else.

## What works

* the boot: the loader, long mode, the banner, the contract check at 1.3;
* the console, the clock, the interrupts (IDT with 32 exceptions, PIC, PIT),
  the context switch, the scheduler, `ps`, `uptime`, `free`, `dmesg`;
* the filesystem: a volume of 512 KB in the memory of the port, formatted at
  the first boot and mounted at `/`, so `ls`, `cat`, `write`, `mkdir`, `rm`,
  `df` and `mount` work on it and `/bin` exists; the modes and the owners are
  stored (`0644` for a new file, `0755` for a directory), and the boot gives
  `/bin/hello` its `0755` through the VFS;
* a program out of a file: the boot writes the image of `hello` to
  `/bin/hello`, gives it the mode 0755 and asks for it with `execve` - the
  stat, the check of the mode, the read through the VFS, the parse of the
  image and the jump are the ones of the call and not a copy of them. The
  program prints its line on COM1 and returns to the port, which the call
  reports as 0;
* the shell on COM1, with `help`, `uname`, `uptime`, `ps`, `free`, `memtest`
  and the rest of the portable commands, plus the commands of the files:
  `chmod`, `chown` and `exec` reach the same calls as the boot, and the
  thread pointer of the library is there, so their `errno` works;
* one `errno` for all tasks: they share one address space, so they share the
  block of the `_Thread_local` the library keeps it in; a block per task
  waits for the paging.

## What does not work yet

* the volume lives in the memory of the machine and does not survive a reset:
  there is no disk and no flash on this port yet, so every boot makes the
  filesystem and writes `/bin/hello` into it again;
* no `fork()` of its own and no paging: the identity map of the loader is the
  first step, not the model (`KERNEL_NOMMU` is still 1), so a program is not
  protected from the kernel and the kernel is not protected from a program;
* no network: `k_port_net_*` answers "no" and the commands that need a socket
  say so.

## Why it exists

The kernel is portable only if something other than the board runs it. This
port caught three things the board never would: a build that needed
`-U_FORTIFY_SOURCE` on a distribution that fortifies by default; the execution
of the kernel on a machine with an MMU that is not used yet; and a program
that read its data one byte away from where it was written, because
`tools/mkprog.sh` padded its segments at the end of the file instead of before
the data. A machine that really jumps into the image is the only way to notice
that the header was right and the bytes were not.
