# The screen

A machine of Micro OS has a screen and a serial console, and the kernel
writes the same line to both. What the screen is depends on the port: a
135x240 TFT on the board, a framebuffer of 1280x800 on x86-64 (the text mode
of the VGA when the machine cannot program one), and a window of QEMU or a
real monitor in front of both.

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

## x86-64: the framebuffer of 1280x800

The port programs the Bochs VBE interface of the VGA of QEMU - the device
`1234:1111`, the I/O ports `0x1CE`/`0x1CF` - to 1280x800 and 16 bits per
pixel in RGB565, which is the format the renderer of the kernel draws in
(ADR 0023). The console is a grid of cells of the 5x7 font at twice its size,
10x14 pixels: **128 columns by 57 rows**. The boot says which screen it chose:

```text
x86-64: the screen is the framebuffer of 1280x800, 16 bpp, 128x57 cells
```

The window of the card is above the identity map of the kernel, so the port
maps it into the top 2 GiB and every address space shares it: a `write` of a
program reaches the same picture as the boot log. A character is one glyph
blitted into the memory, the cursor is the reverse of a cell, and the scroll
moves the rows of the picture up by one cell. There is no double buffer and no
flush, because the memory of the card is the glass.

`console.theme` and `console.invert` repaint the grid, and the setting is
applied when `/etc` is read, before the shell exists, and at every `config
set`:

```text
microos> config set console.theme light
console: theme light, invert 0, the screen the framebuffer of 1280x800, 16 bpp, 128x57 cells
```

Dark is grey on black, light is black on light grey, and `invert` swaps the
two; the picture of a machine with the light theme is a light grey screen with
black letters.

The picture can be taken out of the machine while it runs: the monitor of QEMU
writes it as a PPM (`screendump`), and `make x86-vga-smoke` reads the cells of
the first line, the colours and the cursor out of it. A change of one of the
glyphs the test reads - the letters of "MicroOS" - fails that test, which is
the point of a screenshot in a smoke.

### The fallback: the text mode of 80x25

A machine whose VGA has no Bochs VBE interface - another card, or `-vga
cirrus` - keeps the text mode the firmware left: 80 columns by 25 rows at
`0xB8000`, two bytes a cell, the attribute of `/etc/console` (`0x07` grey on
black, `0x70` black on light grey) and the cursor in the registers 14 and 15
of the CRTC. The boot says so:

```text
x86-64: the screen is the text mode of the VGA, 80x25
console: theme light, invert 0, the screen the text mode of the VGA, 80x25
console: the attribute of the screen 112
```

The 80x50 mode with the 8x8 font of the VGA is not built: see
[debts.md](../debts.md), K11. The console counts the cells either way, so the
two screens differ in the grid and not in what the kernel writes to them, and
the keyboard of IRQ1 is the same for both.

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
which is a loader of its own and a separate change. [ADR
0023](../adr/0023-screen-of-x86-64.md) has the decision and what was built.

## The serial console

COM1 of x86-64, the UART of the board and the pipes of the host substrate
carry the same bytes as the screen. The settings of a screen never hide the
serial line: a machine whose picture did not come up still speaks there, and
the smoke reads both.
