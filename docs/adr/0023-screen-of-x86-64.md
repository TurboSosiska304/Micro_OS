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

The console of the framebuffer works in cells of the 5x7 font scaled twice
(10x14 pixels), about 128 columns by 57 rows at 1280x800, and keeps the
control characters and the two sequences of `clear` it has today. The
colours come from `/etc/console` (ADR 0021), so `config set console.theme`
reaches the picture as it reaches the text screen. The keyboard of IRQ1 does
not change.

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
  `bochs-vbe` with the width, the height, the cell of 10x14 (the 5x7 font of
  `components/font` at twice its size) and the five operations. A character is
  a glyph blitted into the memory, a scroll is a move of the rows of the
  picture, a flush does not exist: the memory is the glass.
* **The console**: `src/console.c` keeps the position, the tab stops, the erase
  of the backspace, the scroll, `\n`, `\r`, `\t`, `\b`, `ESC[2J` and `ESC[H`
  and the em dash of the banner, exactly as the text console of 1.1 did, and
  writes cells to the screen it was given. `src/vga.c` is that screen for the
  text mode and `src/fb.c` for the framebuffer; neither knows the parser.
  The grid is 128 columns by 57 rows. The cursor is the reverse of a cell, and
  the text mode keeps its cursor in the CRTC as before.
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
  and it must be 1280x800, carry only the two colours of the console and be
  made of 2x2 blocks of pixels - the font at twice its size - so a pixel or a
  stroke that no cell of this console drew (the grey of the VGA palette, a
  line of the 8x16 font of the text mode, the leftover of the screen before
  this one) fails the test. Every cell of the grid is compared with the table
  of the font the kernel was built with (read from
  `components/font/src/font.c`): it is the paper, a glyph or the cursor, and a
  cell that holds anything else - the second copy of the log a few pixels
  below the first, at any offset, not only an odd one - fails as well. The
  settled picture and the picture after `config set console.theme light` are
  checked the same way. With `-vga cirrus` it runs the checks of 1.1:
  `pmemsave` of `0xB8000`, the 720x400 picture, the cursor of the CRTC, the
  typed command and the attribute of every cell (`0x70`). `make x86-run` opens
  the window with `-vga std`.
