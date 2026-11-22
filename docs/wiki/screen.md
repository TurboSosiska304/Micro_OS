# The screen

A machine of Micro OS has a screen and a serial console, and the kernel
writes the same line to both. What the screen is depends on the port: a
135x240 TFT on the board, the text mode of the VGA on x86-64, and a window of
QEMU or a real monitor in front of both.

## The board: a framebuffer of 135x240

The panel is an ST7789v2 of 135 by 240 pixels. The console draws into a
framebuffer of the kernel (`kernel/render.h`) and a task flushes it to the
panel; the font is 5x7, and the settings of `/etc` scale it:

```text
microos> config set display.rotation 90
display: rotation 90, invert 1, brightness 100, theme dark
microos> config set font.scale 2
font: scale 2, columns 0, rows 0 (0 is the whole picture)
```

`rotation`, `invert`, `theme` and `font.scale` are the keys of the systems of
[settings.md](settings.md). `brightness` is kept and reported, but the pin of
the backlight has no duty cycle yet, so the panel does not dim.

## x86-64: the text mode today

The port draws into the text mode the firmware leaves behind: 80 columns by
25 rows at `0xB8000`, two bytes a cell, sixteen colours, no font and no
picture. It is enough for a console a person types into, and it is all the
machine has in 1.1:

```text
microos> config set console.theme light
console: theme light, invert 0
```

`console.theme` and `console.invert` repaint every cell of the VGA. There is
no graphics mode: no font of its own, no line, no picture, no window that is
not the whole screen.

## The framebuffer of 1.2 (K11)

[ADR 0023](../adr/0023-screen-of-x86-64.md) chooses the next screen:
a **linear framebuffer** through Bochs VBE in QEMU, 1280x800 and 16 bits per
pixel in RGB565, which is the format the renderer of the kernel already
speaks. The console becomes a grid of the 5x7 font scaled twice - about 128
columns by 57 rows - with the colours of `/etc/console`, and the text mode
stays the fallback (improved once to 80x50 with the 8x8 font of the VGA).

The smoke reads the picture with `screendump` instead of the text memory of
the VGA, so a change of the picture is visible in a screenshot, not only in a
log line.

## What a desktop would need

The framebuffer is the first step, not a desktop. A desktop needs a picture a
program can draw on (a 2D surface), a pointer (the PS/2 mouse of QEMU), and a
**compositor** - windows, focus and the routing of the input. The compositor
has to be a program of the system, not the kernel, like X or Wayland; x86-64
already has ring 3, page tables of a program, NX/W^X, `fork`, `exec`, signals
and pipes, which is the base of a screen server and its clients.

The board cannot have one: no MMU, 2 MB of PSRAM, 135x240. The ESP32 stays a
terminal.

A 3D driver of a real card is not promised. `virtio-gpu` 2D in QEMU is an
option after the framebuffer; a physical machine's picture needs UEFI GOP,
which is a loader of its own and a separate change.

## The serial console

COM1 of x86-64, the UART of the board and the pipes of the host substrate
carry the same bytes as the screen. The settings of a screen never hide the
serial line: a machine whose picture did not come up still speaks there, and
the smoke reads both.
