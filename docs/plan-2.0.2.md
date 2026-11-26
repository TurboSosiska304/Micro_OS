# The plan of 2.0.2

2.0.0 and 2.0.1 were tested on a disk image this tree writes itself - a
writable root, a framebuffer in RAM - and both were broken on the medium a
person actually boots: the UEFI stick of a laptop. This release is not a
feature: it is the machine of 2.0 on the medium the release hands out, with a
gate that boots that medium.

## The defects the owner's laptop found

1. **The desk on a read-only root.** `boot0p1` is mounted read-only by the rule
   of ADR 0041, and `wm` keeps the pixels of a window in a file of `/tmp`:

   ```
   root: boot0p1 cannot open its writes, the root stays read-only
   wm: no file for the pixels of a window
   wm: /bin/term exited with code 138
   ```

   The desktop says "the desktop is up" and has no window. `settings` cannot
   write `/etc/display` either, so the palette and the font of the machine are
   the defaults of the build.
2. **A framebuffer without write combining.** The pixels of the screen go to
   the memory of the video card with no MTRR and no PAT entry: every 4 bytes is
   one uncached transaction on a real machine. The console prints 5-10
   characters a second after the kernel takes the screen, and `wm` draws at the
   same speed. In QEMU the video memory is RAM, so no gate saw it.
3. **The end of a machine that does not switch off.** After the clean end and
   `power: the machine goes down: off`, the laptop keeps the machine on and the
   kernel faults on a user address:

   ```
   x86-64: exception 14 (page fault), error 0x0000000000000000,
           rip 0xffffffff800655c0, cr2 0x00000000bffed1f4
   ```

   Error 0 is a page that is not there: the shutdown tore an address space
   down and something read it afterwards.
4. **The tests ran a different machine.** `x86-vga-smoke` starts `wm` from
   `init` on a writable disk; the shell starts it after `exec`, on a stick.

## The gates of the release

* `make uefi-stick` and a smoke that boots **the stick it wrote**: the desk from
  the shell (`exec /bin/wm`), `Alt+T` opens `/bin/term`, the terminal answers
  `help`, `Alt+F4` closes it, the prompt returns.
* The same boot of `x86-iso` (the ISO is read-only by design and must have the
  same desk).
* `tmpfs` on `/tmp` when the root is read-only, with a check that a file of 4
  MiB can be written there and that it is gone after a reboot.
* The framebuffer mapped write-combining, with a number: the time of one screen
  of text and of one frame of `wm`, before and after, on a machine of QEMU with
  the write-combining emulated and on the laptop of the owner.
* `poweroff` on the laptop reaches S5 and the machine is off, with no fault
  after the write of the sleep type; `make power-smoke` covers the path with
  `acpi=off` as well.
* The full gate of 2.0.1 again, since the merge of these paths touches the
  kernel, the port and the desk.

## The decision of the owner: ramfs, and all the memory that is free

`/tmp` is a **ramfs**, not a tmpfs with a fixed ceiling, and it is **not
mounted on the board at all** (the desk of 2.0 is the machine of x86-64; the
board has about 100 KB of SRAM and no window to draw in).

The size is not a number of megabytes: a file takes blocks from the kernel's
allocator as it is written and gives them back when it is removed, so the limit
of a file is the memory that is free - the same promise as the ramfs of Linux.
One reserve stays out of reach of a person's file, because a machine has to
keep running when `/tmp` is full: the last megabyte of the heap (a constant of
the build, `RAMFS_RESERVE_KB`) is not given to a write, which answers
`ENOSPC` instead, and `df` says how much is free.

A deletion returns its memory at once, `unlink` of an open file keeps the
blocks until the last descriptor closes, a read past the end of a file answers
zero, and `truncate` frees the tail. The board does not build the filesystem.

## The machine we ship is the machine we test (the owner, 26 November)

The stick of 2.0.1 says two things in the first lines of its boot:

```
x86-64: 8 cpu(s) reported by the firmware, 1 in use (apic 0xfee00000)
x86-64: this kernel is built without KERNEL_SMP, the core 0 runs alone
```

The laptop of the owner has four cores and eight threads and the image uses
one of them, because `KERNEL_SMP` is off by default and every gate boots the
image built without it. That is the same defect as the desk on a read-only
root, one level up: **the release hands out a machine that no gate has run**.

So the image of 2.0.2 is built with `KERNEL_SMP=ON`, and every gate of the
port runs that image:

* `KERNEL_SMP` is the default of `ports/port_x86_64` and of the stick and ISO
  targets, not an option a lane turns on for a test.
* `KERNEL_CPUS_MAX` is raised from four to **eight**: the machine of the owner
  is 4 cores and 8 threads, and a limit of four silently ignores half of it.
  The cost is a `struct k_cpu`, a run queue and a 64 KiB stack per core, and
  the number is measured, not guessed (`-smp 8` in QEMU, the log line of the
  boot).
* The line of the boot stays honest in both directions: a firmware that
  reports more cores than the build holds says so, and a core that does not
  answer the start-up IPI is named.
* The gates run with eight processors: `make x86-smoke`, `make x86-vga-smoke`,
  `make x86-smp-smoke` and `make soak-smp` on the image that is shipped, and
  the smoke of the stick asserts `8 cpu(s) reported by the firmware, 8 in use`
  (the machine of QEMU) before it runs the desk.

The debts of K8 are the reason this is a line of the plan and not a flag:
`docs/debts.md` records the big kernel lock, the per-core state, the I/O APIC,
the wake IPI, the FPU state and the guard page as **done**; what is open is a
data race of a test (`test_smp_double_claim`), a two-core stress that migrates
a task inside a filesystem call, and the weak default of a port with one core.
None of them is a reason to ship one core; the first one is a reason to fix
the test.

## What is not in this release

**C23.** Measured on 26 November: the host and the x86-64 port compile with
`-std=c23` and no GNU extensions with zero errors, so the move is one flag - but
it is a move of 2.1, not of a release whose job is the desk and the shutdown of
a laptop that hangs today (`docs/roadmap-to-3.0.md`, 10b).

## The checklist of the owner's laptop

The image is handed over with a hash and a numbered list, and one boot answers
every question of the release. The order matters: the first two steps are the
medium, the rest is the machine.

1. **Write the image.** `sudo dd if=<image> of=/dev/sdX bs=4M conv=fsync
   status=progress && sync`, then `sudo head -c <bytes> /dev/sdX |
   sha256sum` - the number must be the one in the release notes. Secure Boot
   off, the stick chosen in the boot menu.
2. **Read the first ten lines of the boot.** Three lines decide three defects:

   ```
   e1000e: the card is at 00:1f.6        <- the wire does not hang the boot
   x86-64: 8 cpu(s) reported by the firmware, 8 in use   <- every core runs
   MicroOS shell ready                   <- the shell is up
   ```

   A boot that stops before the third line is the defect of 2.0.1 again, and
   the last line on the screen is the thing to send.
3. **Time the text.** Count the lines that appear in five seconds. The number
   of 2.0.1 was five to ten characters a second; the number of this release is
   the one the plan asks for.
4. **The desk.** `exec /bin/wm` - the desk appears with a clock. `Alt+T` - a
   terminal window. Type `help` - the window answers. `Alt+F4` - the window
   closes and the prompt returns. A window that stays empty, or a terminal
   that dies, is the read-only root of 2.0.1 (it must be gone).
5. **`mfetch`.** Paste the report: it carries the version, the cores in use,
   the memory and the disk. It is the shortest answer to "what machine is
   this", and it is what the notes of the release quote.
6. **`poweroff`.** The machine goes off - the fan stops and the light of the
   power button goes out. A machine that stays on, or a fault on the screen
   after `power: the machine goes down`, is the defect of 2.0.1.

What comes back: the hash mismatch if any, a photo of the screen when a step
fails (the last three lines are enough), the count of step 3, and the text of
step 5. One boot, one message.
