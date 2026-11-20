# The x86-64 port: bare metal in QEMU

A port of the kernel for a machine with no operating system under it: no
libc, no POSIX, no firmware. It is the proof that the kernel is portable, and
the machine where a program can really be loaded and run (the window of
`execve` is here).

| | |
|---|---|
| port | `ports/port_x86_64` |
| loader | `loader32.S`: a multiboot image that QEMU loads with `-kernel`; it copies the kernel to 16 MiB, builds the identity map of the first gigabyte and turns the long mode on |
| console | the text screen of the VGA (80x25, `0xB8000`, light grey on black) and COM1, 115200, `\r\n`, out of one `k_port_write()` |
| input | the PS/2 keyboard on IRQ1 (scancode set 1, Shift and Caps Lock) or COM1 |
| clock | the PIT at 1 kHz; critical sections save and restore the flags |
| tasks | `switch.S` (callee-saved registers and the stack) and a round robin over the ready queue |
| store | `ata0`: a raw image on the primary IDE channel, PIO, MicroFS on its first partition, mounted at `/` (ADR 0008); `ram0`, 512 KB of the memory of the machine, when there is no disk |
| programs | `k_port_exec_supported()` is true; the window is 16 MiB at 32 MiB (`0x02000000`-`0x03000000`); a program is a file in `/bin` (the MOX1 format of `kernel/exec.h`) |
| build | `make x86-build` (needs `cmake`, `ninja`, `xorriso` is not needed) |
| check | `make x86-smoke` (serial), `make x86-vga-smoke` (screen and keyboard), `make x86-disk-smoke` (two boots on a disk) - all need `qemu-system-x86` |

## Run it

```bash
make x86-build
make x86-run          # a QEMU window: the VGA console and its keyboard
make x86-run-serial   # the same machine in a terminal (-nographic)
make x86-run-disk     # the window with an 8 MB disk on the IDE channel
make x86-run-disk-serial  # the same disk in a terminal
make x86-smoke        # boot and check the lines of the banner and the shell
make x86-vga-smoke    # check the screen and the keyboard without a display
make x86-disk-smoke   # two boots on one image: a file written in the first is read in the second
```

`make x86-run` opens the window of QEMU with `-display gtk`. The display
backend is a package of its own in some distributions; the target asks QEMU
for its list of backends first and prints the command to run when the one
asked for is not there.

| distribution | the package of the GTK window |
|---|---|
| Arch, Manjaro | `sudo pacman -S qemu-ui-gtk` |
| Debian, Ubuntu | `sudo apt install qemu-system-gui` |
| Fedora | `sudo dnf install qemu-ui-gtk` |

A QEMU built with SDL and without GTK takes `make x86-run X86_DISPLAY=sdl`;
one built without a display backend at all cannot open a window, and
`make x86-run-serial` is the machine for it. `X86_DISPLAY=none` runs the
image without a window and with a serial port that goes nowhere, so with no
console at all: it says that the machine boots and nothing more.

The serial port of the window run is `-serial null` and not `-serial none`:
`null` is a backend that takes the bytes and drops them, so the UART of the
machine is there and behaves; `none` removes the device, and a read of the
status register of a port nobody answers returns `0xFF`, whose bit 0 says "a
byte is ready" for ever. The console polls that register, so it would take
the answer of a missing chip for a key and type a line nobody wrote - the
window fills with "line too long" while a person does nothing. The console a
person reads is the screen, and the bytes of COM1 are the same ones; the port
still takes the keyboard and the UART as two halves of one console.

The two smoke tests check different consoles of the same machine.
`tools/x86_smoke.py` types into COM1 and reads the transcript of the serial
port; `tools/x86_vga_smoke.py` boots with `-display none`, reads the video
memory with the monitor of QEMU (`pmemsave`), takes a screenshot
(`screendump`) and types with `sendkey`, so it proves the screen and IRQ1
without a display server. It is a target of its own and not a part of
`make ci`, because the monitor of QEMU is a tool a build farm does not have
to have. `python3 tools/x86_vga_smoke.py --display gtk` runs the same checks
against the real window instead of its memory; the screenshot is then the
picture of the window, which is how a person watches the keys arrive.

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

## The window: the VGA text and the keyboard

The screen of the mode the firmware leaves behind is a memory: 80 columns by
25 rows of two bytes at `0xB8000`, the character and its colour. The port
does not program a video mode and does not set a palette - the text mode 3 is
the one every PC has had since 1987, and a console is text. `k_port_write()`
writes every byte there and to COM1, so the window and the serial log are the
same picture and the boot has no second path for a second console; the kernel
does not know that there are two.

The screen obeys the control characters of a terminal - `\n`, `\r`, `\t` and
`\b` - scrolls a row up when the last line is full, and puts the position of
the cursor into the registers 14 and 15 of the CRTC (`0x3D4`/`0x3D5`) after
every write, so the cursor of the hardware is where the next character will
land. The colour is one pair for every cell: light grey on black. The one
escape sequence the console of the kernel writes is the pair of the `clear`
command (`ESC[2J` and `ESC[H`), and the screen reads it; a sequence of any
other kind is dropped whole instead of being printed, because a window is not
a terminal emulator and a `[2J` in the middle of a line is not a picture.

The keyboard hangs on the first line of the 8259 (IRQ1, the vector `0x21`).
The stub in `interrupts.S` saves the registers around the call of C, the
handler reads the byte of the data port `0x60`, translates it with the
scancode set 1 (the 8042 of the machine translates the set 2 of the device
into the set 1 of the PC), puts the character in a ring of 64 and sends the
EOI. A task takes the characters out of the ring: `k_port_console_poll()` is
non-blocking, `k_port_console_getchar()` waits in `hlt` when there is
nothing, and both ask the keyboard before they ask COM1, so the two consoles
are alive at once and neither has to be chosen at the boot.

The keys are the US layout: the letters, the digits and the punctuation a
command line needs. Shift changes the characters and Caps Lock capitalises
the letters, which is the whole of what the lock does on a PC. Enter is
`\r`, Backspace is `\b` and Ctrl+C is the character 3 - the three editing
commands the shell of the kernel reads (`src/shell.c`), and the reason a
typo can be erased in the window at all. Tab is not a key of this console:
the shell splits a command line on spaces, and a tab would arrive as part of
a word.

What the window does not do:

* it is the text mode and only the text mode: no graphics, no font of its
  own, no colour but the one pair;
* the scancode set 1 and the US layout only: a keyboard of another layout
  sends the same numbers and gets the letters of this table;
* the E0 sequences are dropped whole, so the arrows, Home/End, Delete, the
  second form of the keypad and the right Ctrl do nothing. The editing of a
  line is the one the shell has, not the one a library with a history has;
* no repeat of a key in the driver: the device repeats it, and the ring is
  the only buffer between the two.

## The disk

`make x86-run-disk` attaches a raw image of 8 MB to the primary IDE channel
(`-drive file=build-x86/disk.img,format=raw,if=ide`), and the port makes the
image the first time it needs it: 8 MB of an erased medium with the MBR of one
partition at LBA 2048 (`tools/x86_disk_smoke.py --mkimage`). The boot finds the
drive (`ata0`), reads the MBR, registers the partition (`ata0p1`) and mounts it
at `/`; the first time the filesystem finds it blank and formats it, and after
that the volume it finds is the one it left.

So this machine keeps what it writes: a file created in the shell, a program
in `/bin`, the mode and the owner of both, survive a reset. `make
x86-disk-smoke` is the proof and the regression test: one image, two boots,
`write /persist.txt` in the first, `cat /persist.txt` in the second.

The driver is `src/ata.c`: the ATA registers at 0x1F0, LBA28, PIO, polled -
no DMA (the port has no contract for a physical address yet) and no interrupt
of the channel. A disk that does not answer is reported and the machine goes
on with `ram0`, so `make x86-run` and `make x86-smoke` work without one.
`disk.img` is generated and not committed; `make clean` does not remove
`build-x86`, `rm -rf build-x86` does.

## What works

* the boot: the loader, long mode, the banner, the contract check at 1.3;
* the console: the VGA text screen and COM1 from one call, the keyboard of
  the PS/2 with Shift, Caps Lock, Backspace and Ctrl+C, and the serial port
  with the console it always had;
* the clock, the interrupts (IDT with 32 exceptions, PIC, PIT), the context
  switch, the scheduler, `ps`, `uptime`, `free`, `dmesg`;
* the filesystem: MicroFS mounted at `/`, on the first partition of the disk
  when one is attached and on a volume of 512 KB in the memory of the port
  when there is not; `ls`, `cat`, `write`, `mkdir`, `rm`, `df`, `fsck` and
  `mount` work on it and `/bin` exists, the modes and the owners are stored
  (`0644` for a new file, `0755` for a directory), and the boot gives
  `/bin/hello` its `0755` through the VFS;
* a disk: a raw image on the primary IDE channel, read and written by PIO, its
  MBR read by the portable parser, MicroFS on the first partition (`ata0p1`)
  and the volume formatted at the first boot; what is written to it survives
  a reset, which `make x86-disk-smoke` checks (ADR 0008);
* a program out of a file: the boot writes the image of `hello` to
  `/bin/hello`, gives it the mode 0755 and asks for it with `execve` - the
  stat, the check of the mode, the read through the VFS, the parse of the
  image and the jump are the ones of the call and not a copy of them. The
  program prints its line on COM1 and returns to the port, which the call
  reports as 0;
* the shell on the console, which is the window and COM1 at once, with
  `help`, `uname`, `uptime`, `ps`, `free`, `memtest` and the rest of the
  portable commands, plus the commands of the files:
  `chmod`, `chown` and `exec` reach the same calls as the boot, and the
  thread pointer of the library is there, so their `errno` works;
* one `errno` for all tasks: they share one address space, so they share the
  block of the `_Thread_local` the library keeps it in; a block per task
  waits for the paging.

## What does not work yet

* the disk is one, and it is the primary ATA channel: no SATA, no virtio, no
  NVMe, no card, no second disk;
* the driver polls and copies through the processor: no interrupt, no DMA and
  no queue, so a large copy is slow; there is no cache of the kernel and the
  cache of the drive is flushed after every write;
* the tables understood are the MBR of four primaries and the GPT; extended
  and logical partitions wait, and a medium of 4 KB sectors does not;
* without a disk the volume is still the memory of the port, and it does not
  survive a reset - the boot then makes the filesystem again;
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
