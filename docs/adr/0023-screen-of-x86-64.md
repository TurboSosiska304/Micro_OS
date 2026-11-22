# ADR 0023 - The screen of x86-64: a framebuffer, not a GPU

* Status: accepted
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
