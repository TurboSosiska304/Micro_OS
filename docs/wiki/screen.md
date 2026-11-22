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
(ADR 0023). The console is a grid of cells of the 8x16 font of the PC (Spleen
8x16 2.3.0, BSD-2-Clause, `third_party/spleen`), one glyph a cell: **160
columns by 50 rows**. The boot says which screen it chose:

```text
x86-64: the screen is the framebuffer of 1280x800, 16 bpp, 160x50 cells
```

The window of the card is above the identity map of the kernel, so the port
maps it into the top 2 GiB and every address space shares it: a `write` of a
program reaches the same picture as the boot log. A character is one glyph
blitted into the memory, the cursor is the reverse of a cell, and the scroll
moves the rows of the picture up by the step of a line. There is no double
buffer and no flush, because the memory of the card is the glass.

The step of a line is the cell plus `font.line_gap`, a key of `/etc/font`
(ADR 0021): 0, 2 or 4 pixels give 16, 18 or 20 pixels a line - 50, 44 or 40
rows of the picture - and the grid is laid out again from the cells the
console holds, so the log is never lost when the key changes:

```text
microos> config set font.line_gap 2
font: line_gap 2, the cell is 8x16 at 18 px, 44 rows of 160 columns
font.line_gap=2
```

`scale`, `columns` and `rows` of the same file are the cap of the panel of the
board: this machine keeps them and says once that they are not its business.

`console.theme` and `console.invert` repaint the grid, and the setting is
applied when `/etc` is read, before the shell exists, and at every `config
set`:

```text
microos> config set console.theme light
console: theme light, invert 0, the screen the framebuffer of 1280x800, 16 bpp, 160x50 cells
```

Dark is grey on black, light is black on light grey, and `invert` swaps the
two; the picture of a machine with the light theme is a light grey screen with
black letters.

### The colours of a cell

A cell of the console carries its own pair of colours: the paper in the high
nibble and the ink in the low one, the byte a VGA calls
`(background << 4) | foreground`. The palette is the sixteen colours of the
CGA in RGB565, so a word keeps its colour through a scroll, a repaint and the
cursor (which is the same two colours traded).

A command or the log asks for a colour with the SGR sequences of a terminal:

| Sequence | Meaning |
|---|---|
| `ESC[0m` | the pair of `/etc/console` again, with no bold and no reverse |
| `ESC[1m`, `ESC[22m` | bold on and off (it brightens a colour of a command) |
| `ESC[7m`, `ESC[27m` | the reverse (the ink and the paper traded) and back |
| `ESC[30m`-`ESC[37m`, `ESC[39m` | the ink, or the pair of the console |
| `ESC[40m`-`ESC[47m`, `ESC[49m` | the paper, or the pair of the console |
| `ESC[90m`-`ESC[97m`, `ESC[100m`-`ESC[107m` | the bright half of the two |

A parameter the screen does not know is skipped and the rest of the list still
counts, as a terminal does. The log marks its levels: an information is plain,
a warning is amber, an error is red and a success is green - on the screen
only, because COM1 stays a plain transcript. The boot's own lines are the pair
of `/etc/console`; the colours begin with the shell, so the first frame of the
machine is the two colours a person chose.

`console.ink` and `console.paper` name the pair of the plain text instead of
the theme: a name of the palette (`black`, `white`, `grey`, `green`, `amber`,
`red`, `blue`, `cyan`, `magenta`, `yellow`) or a number of RGB565 such as
`0xF800`. The word `theme` is the value of the theme, and it is the default:

```text
microos> config set console.ink red
microos> config set console.paper 0x001F
microos> config set console.ink theme
```

A value that is neither a name nor `0xRRRR` is the theme and a warning. On the
text mode a colour that is not one of the sixteen is rounded to the nearest of
them; the picture of the framebuffer takes the number of RGB565 as it is.

The picture can be taken out of the machine while it runs: the monitor of QEMU
writes it as a PPM (`screendump`), and `make x86-vga-smoke` reads the cells of
the first line, the colours and the cursor out of it. A change of one of the
glyphs the test reads - the letters of "MicroOS" - fails that test, which is
the point of a screenshot in a smoke.

The test looks at the *first* frame of the boot as well, and strictly: the
picture must be the two colours of the console and nothing else, and every
cell of the 8x16 grid must be the paper, a glyph of the font or the cursor -
the table of the font is read from the tree, so a glyph redrawn is a failure
and a pixel that no cell drew is one too (the grey of the VGA palette, the
screen the firmware left, a second copy of the log a few pixels below the
first, at any offset). The lower case of the font is proved by the tails of
`g`, `p` and `y`; the step of a line by the cell of the second line, 16 pixels
below the first; and `font.line_gap 2` in the picture (44 rows of 18 pixels)
and in `/etc/font`. The port makes the first frame whole from its side: the
card clears its memory when the mode is set (the cells of the text mode live
in the same memory), the text window is emptied before the switch, and the
whole picture is drawn a second time after the first lines of the log - the
cells are kept, so nothing of the log is lost, and every pixel of the frame is
written again.

### The fallback: the text mode of 80x25

A machine whose VGA has no Bochs VBE interface - another card, or `-vga
cirrus` - keeps the text mode the firmware left: 80 columns by 25 rows at
`0xB8000`, two bytes a cell, the attribute of `/etc/console` (`0x07` grey on
black, `0x70` black on light grey) and the cursor in the registers 14 and 15
of the CRTC. A cell that a command coloured keeps its own attribute: the
`ls --color` of a directory is `0x01` (blue on black), an executable `0x02`
(green on black), a warning of the log `0x06` (amber on black) and an error
`0x04` (red on black). The boot says so:

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
