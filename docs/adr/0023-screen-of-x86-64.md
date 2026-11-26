# ADR 0023 - The screen of x86-64: a framebuffer, not a GPU

* Status: accepted, built in K11 (1.2)
* Date: 2026-11-22
* Supersedes: nothing
* Related: ADR 0004 (the port), ADR 0018 (the screen of the board), ADR 0021
  (the settings of `/etc`), ADR 0012 (the phases of 1.x)

## Context

The x86-64 port draws into the text mode the firmware leaves behind: 80
columns by 25 rows at `0xB8000`, two bytes a cell, sixteen colours, no font
and no picture (`docs/ports/x86_64/README.md`, "The window"). It is enough
for a console a person types into, and it is the one screen the port has
ever had. The price is visible the moment the machine is useful: a boot log
and a command scroll a small window, a drawing command has nowhere to draw,
and the monitor of a modern host shows the window scaled, not filled.

The first question is what "a GPU" would mean. A linear framebuffer is the
picture the machine can show; it is set by the firmware (VBE), by a
hypervisor (Bochs VBE, virtio-gpu) or by a driver of a card. A 3D engine of
an Intel, AMD or NVIDIA card is a different project: PCI enumeration with
BARs and MSI, DMA and an IOMMU, firmware blobs and a command stream of
hundreds of pages, all behind a machine that already has an MMU. The kernel
of this project has an MMU on x86-64 and no driver model above a block
device and a network card; a 3D driver is not a phase, it is a different
operating system's worth of work.

The second question is where the screen lives. The console already draws
through `kernel/render.h`: a width, a height and five operations, with the
panel of the board behind it. A framebuffer is the same picture with a
different owner, and the code of the console does not have to know which.

## Decision

### 1. The first screen of 1.2 is a linear framebuffer

The port programs the Bochs VBE interface of QEMU (the I/O ports `0x1CE` and
`0x1CF` and the PCI device `1234:1111`) to 1280x800, 16 bits per pixel,
RGB565. The last part is the point: `k_renderer` already speaks RGB565, so
the drawing code of the console, the font and the boxes moves to the new
screen unchanged, and the board and the machine share one picture model.

The window of the framebuffer is found over the PCI configuration space the
network port already reads; `paging.c` maps its physical range. The size of
the picture is about 2 MB, so the map is a handful of 2 MiB pages, not a new
allocator.

### 2. The text mode stays as the fallback

A machine without VBE keeps the text mode of the firmware, and the port
chooses between the two at the boot: the framebuffer when the interface
answers, the text screen otherwise. The 80x50 mode with the 8x8 font of the
VGA is the cheap improvement of the fallback, not a step of its own: more
rows for a machine that cannot show a picture.

### 3. The console counts cells, the port draws them

The console of the framebuffer works in cells of a font, and the font of a PC
is 8x16: the board's 5x7 page is built for a panel of 135x240 and folds the
lower case into the upper one, which is not what a shell on a screen of
1280x800 wants. The first screen took the 8x16 of Spleen 2.3.0
(BSD-2-Clause, `third_party/spleen`), so a line of the picture is 160
characters and the picture holds 50 rows; the gap between two lines is a key
of `/etc/font` (`line_gap`, 0, 2 or 4 pixels: 50, 44 or 40 rows). The console
keeps the control characters and the two sequences of `clear` it has today.
The colours come from `/etc/console` (ADR 0021), so `config set
console.theme` reaches the picture as it reaches the text screen. The
keyboard of IRQ1 does not change.

### 4. The GPU: 2D maybe, 3D no

virtio-gpu 2D in QEMU is a reasonable later step if the picture needs a
device instead of a memory: resources, a scanout, a command stream, still no
acceleration of the drawing. It is not part of the first screen. A 3D driver
of a real card is not promised in 1.x or 2.0; the honest path to a real
machine's picture is UEFI GOP, which is a loader of its own and a separate
change, not a driver.

### 5. The tests see the picture

`make x86-vga-smoke` reads the text memory of the VGA today; a framebuffer
has no such memory, so the smoke reads the picture with the `screendump`
command of the QEMU monitor (PPM) and checks the cells it expects and the
colours of `/etc/console`. `make x86-run` opens the same window for a person.
The text fallback keeps the `pmemsave` checks, so both screens are tested.

## Consequences

* The console of the port is rewritten from cells in video memory to a cell
  grid drawn through `k_renderer`; the scrolling, the cursor and the escape
  parser stay the same logic.
* `paging.c` gets the mapping of the window, `port_x86_64` gets the mode set,
  the PCI lookup moves from `virtio_net.c` to a small shared helper.
* A screenshot becomes an artifact of the smoke, so a change of the picture
  is visible in a diff of bytes, not only in a log line.
* The port still does not promise a real card: QEMU is the machine of the
  port, and UEFI GOP is the later door to a physical one.

## What was built (K11, 1.2)

* **The device**: the VGA of QEMU, `1234:1111`. The configuration space is
  `src/pci.c`, which the network driver uses as well - the lookup moved out of
  `src/virtio_net.c` and both drivers now read the same four registers. The
  window of the picture is a memory BAR of the card whose size the device
  reports to be at least the picture (`port_x86_64_pci_bar_size()`). With the
  `vga` device of QEMU it is the first BAR, the 16 MB of the memory at
  `0xFD000000`; the search walks all six BARs by their size, so a card that
  keeps its memory elsewhere is not mis-mapped.
* **The mode**: `src/vbe.c` programs the Bochs VBE registers on the I/O ports
  `0x1CE`/`0x1CF`: the identifier `0xB0C0`-`0xB0C5`, 1280x800, 16 bits per
  pixel, the virtual width equal to the visible one, then the enable with the
  linear frame buffer. The size, the depth and the memory are read back before
  the mode is accepted; a card whose interface does not answer is the fallback.
* **The window**: the framebuffer is at `0xFD000000`, above the identity map
  of the first gigabyte, so `port_x86_64_paging_map_device()` maps it at
  `PORT_X86_64_SCREEN_VIRT` - `0xFFFFFFFFA0000000`, half a gigabyte into the
  top 2 GiB - in 2 MiB pages, writable and NX. The PDPT of the higher half is
  shared by every address space, so a `write` of a program draws on the same
  picture. The picture of 1280x800x2 is 2,048,000 bytes: one 2 MiB page.
* **The renderer**: `src/fb.c` registers a `struct k_renderer` named
  `bochs-vbe` with the width, the height, the cell of 8x16 (the font of the
  PC, `ports/port_x86_64/src/font_spleen.c`) and the five operations. A
  character is a glyph blitted into the memory, a scroll is a move of the rows
  of the picture by the step of a line, a flush does not exist: the memory is
  the glass.
* **The console**: `src/console.c` keeps the position, the tab stops, the erase
  of the backspace, the scroll, `\n`, `\r`, `\t`, `\b`, `ESC[2J` and `ESC[H`
  and the em dash of the banner, exactly as the text console of 1.1 did, and
  writes cells to the screen it was given. `src/vga.c` is that screen for the
  text mode and `src/fb.c` for the framebuffer; neither knows the parser.
  The grid is 160 columns by 50 rows of the 8x16 font, and the step of a line
  is the cell plus the `line_gap` of `/etc/font` (16, 18 or 20 pixels: 50, 44
  or 40 rows of the picture). The cursor is the reverse of a cell, and the
  text mode keeps its cursor in the CRTC as before.
* **The font and its licence**: Spleen 8x16 2.3.0 of Frederic Cambus,
  BSD-2-Clause, generated into a table by `tools/spleen_font.py` from the BDF
  in `third_party/spleen/`; the licence travels with the bundle
  (`tools/bundle.sh`) and `docs/licensing.md` records the rule. The 5x7 font
  of `components/font` stays the font of the board.
* **The colours**: the `console` system of `/etc` (`theme`, `invert`) is
  applied when the files are loaded, which is before the shell exists, and at
  every `config set`: the screen repaints the cells it holds, so the line a
  person typed is read again in the new pair. Dark is grey `0xAD55` on black,
  light is black on `0xAD55`, and `invert` swaps the two.
* **The fallback**: a machine whose interface does not answer (the smoke boots
  `-vga cirrus` for it) keeps the text mode of the firmware, 80x25 at
  `0xB8000`, and the console of 1.1 unchanged; the boot says which screen it
  chose (`x86-64: the screen is ...`). 80x50 with the 8x8 font of the VGA is
  *not* built: it needs a font load and CRTC timings, and the text mode is the
  fallback of a machine that has no picture, not a screen to invest in
  (`docs/debts.md`, K11).
* **The first frame**: the mode is turned on without `VBE_NOCLEARMEM`, so the
  card clears the memory it shows as part of the switch - the cells of the
  text mode live in the same memory (the offset `0x18000` of the card is the
  `0xB8000` window) and none of them may survive into the picture. The console
  empties that text window before the switch as well, and after the first
  lines of the boot are drawn it draws the whole picture again from the grid:
  the log is not lost (the console holds the cells) and every pixel of the
  first frame goes through the display once more, which is what leaves no line
  of the 80x25 screen under the new one. The pass is after `/etc` is read, so
  it carries the colours a person chose.
* **The tests**: `make x86-vga-smoke` boots the machine twice. With `-vga std`
  it reads the picture with `screendump` (a PPM of 1280x800), compares the
  cells of the first line with the bitmaps of the font, finds the one cell that
  is all ink (the cursor), types `fsck` through `sendkey` and sees the answer
  on the serial console and the picture grow below it, then sets
  `console.theme light` and reads the paper of the new picture as the light
  grey. The *first* frame is checked on its own: the dump is taken as soon as
  the banner is in the transcript (the console writes the screen before COM1),
  and it must be 1280x800 and carry only the two colours of the console - a
  pixel that no cell drew (the grey of the VGA palette, the leftover of the
  screen before this one) fails the test. Every cell of the 8x16 grid is
  compared with the table of the font the kernel was built with (read from
  `ports/port_x86_64/src/font_spleen.c`): it is the paper, a glyph or the
  cursor, and a cell that holds anything else - the second copy of the log a
  few pixels below the first, at any offset - fails as well. The lower case is
  proved by the tails of `g`, `p` and `y`, the step of a line by the cell of
  the second line at 16 pixels, and `font.line_gap 2` by the 44 rows of 18
  pixels of a picture laid out again. The settled picture and the picture
  after `config set console.theme light` are checked the same way. With `-vga cirrus` it runs the checks of 1.1:
  `pmemsave` of `0xB8000`, the 720x400 picture, the cursor of the CRTC, the
  typed command and the attribute of every cell (`0x70`). `make x86-run` opens
  the window with `-vga std`.

## What was built (K11c, 1.2): a colour per cell

The first screen had one pair of colours for the whole picture. A command
could not ask for a red word and a log could not tell a warning from a line of
information, which is the debt `docs/debts.md` recorded against K11. This is
what the tree does now.

* **The attribute of a cell.** A cell of the grid is a letter and one byte:
  the paper in the high nibble and the ink in the low one, the pair a VGA
  calls `(background << 4) | foreground`. The console computes it from the
  state of the dialogue (`kernel/ansi.h`) and the screen keeps it with the
  letter, so a scroll, a repaint and the cursor carry it: the cursor is the
  same two colours traded. The text mode of the VGA writes the byte into the
  attribute byte of the cell of `0xB8000`, where it has always lived.
* **The palette.** `ports/port_x86_64/src/fb.c` holds the sixteen colours of
  the CGA in RGB565 (`port_x86_64_palette`): black, blue, green, cyan, red,
  magenta, brown, light grey and the bright eight. The framebuffer paints
  them; the text mode has them by definition, so the same red is the same red
  on both screens. A colour of `/etc/console` that is not one of the sixteen
  is widened to RGB565 on the picture and rounded to the nearest of the
  sixteen on the text mode.
* **ANSI SGR.** The parser of the console reads `ESC[...m`: `0`, `1`, `22`,
  `7`, `27`, `30`-`37`, `39`, `40`-`47`, `49`, `90`-`97`, `100`-`107`, a list
  of parameters and an empty one (which is the reset). A parameter it does not
  know is skipped and the rest of the list still counts. The state - ink,
  paper, bold, reverse - lives in `components/kernel/src/ansi.c`, which is
  portable and tested on the host (`tests/cases/test_ansi.c`); the console
  keeps the SGR state and the pair of `/etc/console` and asks it for the
  attribute of every letter. The numbers of a terminal are translated to the
  order of the palette (a terminal's yellow is the brown of a CGA), so `33`
  is the amber a warning is read in and `31` is the red of an error.
* **The serial line stays plain.** The same bytes go to the screen and to
  COM1, but the port holds an escape sequence of the serial copy and drops it
  when it ends in `m`: a transcript has no colours and the log of a machine
  with a screen is still the text it is. `clear` still sends its two
  sequences, which are not SGR.
* **The levels of the log.** `klog` marks a line by its level on a screen
  with colours: an information is plain, a warning is amber (`33`), an error
  is red (`31`) and a success is green (`32`, the macro `kok`, used by the
  `[ ok ]` lines of the boot). The ring of `/proc/kmsg` and the capture of the
  boot keep the text without a sequence, and a console that does not read them
  (the UART of the board, the host, COM1) is not sent them at all
  (`k_console_set_ansi`). The port of x86-64 turns them on when the shell is
  about to start, so the first frame of the boot is the two colours of
  `/etc/console` and nothing else - which is what the smoke test of the first
  frame reads.
* **`ink` and `paper` of `/etc/console`.** The two keys name the pair of the
  plain text: a name of the palette (`black`, `white`, `grey`, `green`,
  `amber`, `red`, `blue`, `cyan`, `magenta`, `yellow`) or a number of RGB565
  (`0xF800`). The word `theme` is the pair the theme chose, which is also the
  default of both keys, and `theme` and `invert` are read first: a person who
  named a colour meant it. A value that is neither is the default and a
  warning. On the board the keys are kept and not used (the panel has its own
  dialogue), on the host the whole system is kept; the wiki says so.
* **`ls --color`.** The shell lists a directory in blue and a file with an
  execute bit in green, `--color=never` and `--color=auto` are the plain
  listing (`auto` is off: this shell has no terminal to ask about) and no flag
  is the listing of 1.1. The colours are the SGR sequences, so a screen shows
  them and the serial copy does not.
* **The tests.** `make x86-vga-smoke` reads the colour of a cell out of the
  picture: the green of an executable and the blue of a directory of
  `ls --color`, the amber of a warning and the red of an error of the log,
  and the red ink on a blue paper of `config set console.ink 0xF800` /
  `console.paper 0x001F`. On the text mode the same commands are read as the
  attribute of the cell (`0x02`, `0x01`, `0x06`, `0x04`), which is the
  sixteen colours of the fallback. `make test` covers the SGR parser
  (`test_ansi.c`) and the sequences of `ls --color` without a screen
  (`test_kshell.c`).

## What changed in 2.0.2: write combining and row copies

The owner's laptop exposed a cost QEMU could not: after UEFI handed over its
frame, every four-byte store was an uncached transaction and the console
printed only 5-10 characters a second. The VBE path was cacheable but had no
write-combining type either. The picture needed a memory type between
write-back and uncached.

The port reserves **IA32_PAT entry 5** for write-combining. Every PAT-capable
core writes that one entry, preserves the other seven, and reads the MSR back.
A 2 MiB framebuffer PTE selects the entry with `PAT=1`, `PCD=0`, `PWT=1`, and
keeps `P`, `RW` and `NX`. The choice is PAT rather than a variable MTRR because
the PTE names exactly the kernel's framebuffer window: it consumes none of the
machine's finite variable MTRRs, changes no physical alias or identity-map
entry, and leaves every MMIO register window uncached. MTRRs are not rewritten;
their normal precedence remains in force.

| Path | Before | After |
|---|---|---|
| VBE framebuffer | `0x8000000000000083` (WB) | `0x800000000000108b` (PAT WC) |
| UEFI GOP framebuffer | `0x800000000000009b` (UC) | `0x800000000000108b` (PAT WC) |
| CPU without PAT | - | `0x800000000000008b` (WT) |

A CPU without PAT, or a PAT read-back that does not match, drops bit 12 and
keeps `PCD=0`, `PWT=1`: write-through memory. That is slower than WC but correct,
and a missing feature never becomes a panic. `make x86-vga-smoke` boots both a
PAT CPU and `qemu64,-pat`, reads back the PTE/PAT/MTRR values, checks the
picture, and requires the W^X audit to stay clean.

The console also stopped painting a cell one pixel at a time. Each 8x16 cell
is now sixteen rows of eight RGB565 pixels: the paper and font bits are
combined in a local sixteen-byte row and copied to the framebuffer with one
`memcpy`. A space clears its rows through the same path, and scrolling already
moved the picture with one `memmove`. The UEFI shadow-to-frame conversion builds
128 pixels in a local chunk and copies each chunk, for the same reason and with
the same channel expansion and reserved alpha byte. The font, colours, cell
geometry and visible pixels do not change.
