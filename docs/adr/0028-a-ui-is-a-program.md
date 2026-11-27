# ADR 0028 - The UI is a program with a screen and a keyboard

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0007 (paging and user mode), ADR 0016 (the entry of a program),
  ADR 0023 (the screen of x86-64), `docs/plan-1.4.md` (M4)

## Context

The machine has a screen of 1280x800 with a font and sixteen colours
(ADR 0023), a console drawn by the kernel into it, and a keyboard read by the
port. There is no interface above the console: a person types lines and reads
lines, and everything the machine shows is a scroll of text.

The kernel draws because it must: a boot that says nothing is a boot that
looks dead, and a panic has to be visible. But an interface is not a kernel
subsystem. A window, a panel, a focus, a font of a title - the shape of all of
that changes with taste, and every change of a taste does not deserve a
reflash of the firmware of a board.

There is no `mmap` in the ABI and no terminal layer above the console: a
program that wants the screen must be given the screen the way a program is
given a file, and a key must arrive as bytes, not as a scancode of the port.

## Decision

### 1. Two devices, and the UI is a program

The kernel exposes, through `devfs`:

* **`/dev/fb0`** - the framebuffer. `read` gives a `struct fb_info` (width,
  height, depth, the format of a pixel, the geometry of the font of the
  console and the palette). `write` takes a **rectangle**: a header of
  `x, y, w, h` and then `w * h` pixels in the format `fb_info` names. A
  rectangle is what a UI changes; a frame of 1280x800 is 2 MiB and the
  arena of a program is a fraction of that, so the interface is a blit and
  not a mapping.
* **`/dev/input0`** - the keys. `read` returns `struct input_event` (a
  moment, a code, a value): the code is the key of the PS/2 set 1 translated
  to the names of the machine (`key.enter`, `key.f1`, `key.a`), the value is
  pressed or released. A read of a program that has no event is `EAGAIN`,
  the same rule as a pipe with a producer (ADR 0014).

Both are opened like any file and closed like any file. A program without
them is a program without a screen, which is what every program of 1.2 is and
what the shell stays.

### 2. The console is the kernel's until a program asks

The console of the kernel draws into the framebuffer. A program that owns the
UI asks for the screen with `ioctl(fb, FBIO_TAKE)` - the kernel stops drawing
the console, saves nothing and writes into the *inactive* picture; `FBIO_GIVE`
hands it back and the kernel repaints the console from its own grid, which it
kept. A panic always takes the screen back: an interface that hides a panic is
worse than a panic.

The serial copy of the console never stops: a machine whose UI is broken is
still a machine with a log.

### 3. The skeleton, and nothing above it

The first UI is one program, `/bin/ui`, and it draws:

* a **panel** of one line: the clock, the load, the volumes and the count of
  the tasks - the four numbers a person looks at;
* a **list of windows** on the left: the console and the monitor of the
  system, one line each, the focused one inverted;
* a **window** of text, which is the console's grid read through the same
  device: a "console window" that shows the last lines of the kernel log and
  of the shell;
* the keys: `Tab` moves the focus, `q` leaves, and a command of the shell
  (`ui`) starts it and returns to the prompt when it ends.

No widgets, no mouse, no theme, no configuration. The skeleton proves the four
things a UI needs - a screen, a key, a frame that does not flicker and a way
back - and leaves the taste to the program that comes after it.

### 4. The shape of the program

`/bin/ui` is a program of the library of 1.3 and uses no privilege of the
kernel: two `open`s, a `read` of the events, a `write` of the rectangles. The
window model lives in the program (`components/ui/` in time, a library of the
userland like any other), not in the kernel: a kernel that knows what a window
is cannot be ported to a machine that wants a different one.

## The device may accelerate a rectangle (1.7)

A write of `/dev/fb0` is a `memcpy` into the memory the display reads, and a
card that can fill a rectangle or copy one in its own engine is a card that
should do that work itself. `kernel/fb_ops.h` is the contract of that: three
operations a device may offer - a fill, a blit and a flush - and the software
path that answers the ones it has not. `components/kernel/src/fb_ops.c` holds
the slot and the software operations, and the write of `/dev/fb0`
(`ports/port_x86_64/src/fbdev.c`) and the fills of the console go through
`k_fb_fill` and `k_fb_blit`; with no device in the slot the bytes on the screen
are the ones of 1.6, which `make x86-vga-smoke` checks pixel by pixel.

This release has the contract and no driver: the VGA of QEMU has no 2D engine,
the board has no framebuffer at all, and a device of the future - virtio-gpu
2D, or the blitter of a real card - is one `k_fb_ops_register` at the boot. The
rectangle of the contract is signed, because a caller inside the kernel
composes a rectangle that may begin outside the picture and the software path
cuts it; the device of `/dev/fb0` is given a rectangle the ABI already checked,
so the clip is a rule of a caller and not a second check of the syscall.

## The device may show a RAM surface (2.1)

Phase A gave the compositor a RAM backbuffer but still copied every damaged
rectangle into the framebuffer. `FBIO_PRESENT` (0x4D03) hands the kernel a
complete `struct fb_present_surface` instead. `kernel/blit.h` owns the choice:
`k_blit_present(dst, src)` asks the registered `k_fb_ops.present` callback once
and, if the device is absent or refuses, copies `src` into `dst` and remembers
the refusal for that registration. The compositor sees answer 0 for a flip and
1 for a successful copy; it never chooses a GPU vendor.

The source pages stay mapped until `FBIO_GIVE` or the last close. The device's
`present_release` callback runs before those pages can be unmapped and restores
the original surface. A driver which cannot guarantee that lifetime, the pixel
format, the stride or the GGTT mappings must refuse; the software copy remains
the always-available implementation. AMD has no present callback in this tree
and therefore keeps the copy path.

## Consequences

**Good.** The interface changes with a file of `/bin`, not with the firmware.
The kernel keeps the two things only it can do - the panel of the last resort
and the panic - and both stay small. A machine without `/bin/ui` is the
machine of 1.2, not a broken one.

**Bad.** A blit of a rectangle over a serial of small writes is slower than a
mapping, and a UI of a window that repaints a whole frame pays for it: the
first skeleton repaints the region it changed, and a program that repaints
everything will be slow. There is no clipping in the kernel: a rectangle that
leaves the screen is refused, not clipped.

**Cost.** `devfs` grows two devices and their tests; the port grows a keyboard
that produces events and not only a line of the console; the ABI grows two
`ioctl` numbers of the framebuffer (`FBIO_TAKE`, `FBIO_GIVE`) and no syscall.
A mouse is a second device and a later ADR.

## What this ADR does not do

* It does not add `mmap`, a window manager, a compositor or a terminal layer.
* It does not change the console of the kernel, the font or the palette.
* It does not promise a UI on the board: the panel of the board is the
  console, as it is today.
* It does not decide the programs of the desktop: this is a skeleton, and the
  next ADR is the one that fills it.
