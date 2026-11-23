# The plan of 1.7

> The release after 1.6.0 (`docs/releases/1.6.md`), which gave a program its
> memory, a task its user, the cores their interrupts, the disk its bus, the
> pipes their speed and the machine its network.

Each lane is a branch of `release_candidate` with its own gates; a lane that
does not fit moves to the next release without rewriting the promise.

## M1 - the desktop

The screen has the pieces of an interface - `/dev/fb0`, `/dev/input0`,
`components/ui`, `/bin/ui`, `/bin/edit` and `/bin/fm` - and no desktop: every
program paints the whole screen itself.

* a **pointer**: the AUX channel of the 8042 and IRQ 12, events of
  `/dev/input0` beside the keys, the ten new codes appended to
  `enum k_input_key`;
* a **window server** `/bin/wm` in ring 3: it owns the screen, holds the
  windows of its clients, composites **only the rectangles that changed**, and
  sends the events of the focus to the window that has it;
* a **shared buffer** per window through a `MAP_SHARED` file - the machine has
  no shared memory object and this is one - and a small protocol over pipes;
* a client library in `components/ui/client/`, portable and tested on the
  host, so that `/bin/edit` and `/bin/fm` move onto it without a rewrite;
* the board takes the same code with two panels and a key to switch: 320x240
  of 150 KiB does not hold a desktop, and it is not asked to.

Gates: `make x86-vga-smoke` with two windows, a click that raises one, keys
that reach the focus, a drag that moves a rectangle and a close that gives the
focus back; a picture before and after (`screendump`).

## M2 - virtio-blk, and the block of the filesystem

* ADR 0037: a second block device behind the same `struct k_blockdev_ops`,
  found on the PCI bus (or virtio-mmio), with the queues the port already
  knows from the network;
* an image that boots on it: `tools/x86_image.py` writes the same volume for
  both buses, and `make x86-smoke` runs it on the disk of the release;
* a block of 4 KiB and **runs of blocks**: the release asks the device for 11
  sectors of a kilobyte for every kilobyte of a file, which is a wall of the
  filesystem and not of the bus.

Target: **100 MiB/s** of writing and **300 MiB/s** of reading on a volume of
5 GiB, against 14.4 and 35.9 of 1.6.0.

## M3 - the cheap write of ext2

The driver moves 16 MiB of a write in 31 ms and the filesystem takes a
second: a block that is about to be written is written twice (the zeroing),
the bitmap of a group is written for every block, and the allocation scans
from the first bit.

* no zeroing of a block that a write is about to fill (the sparse file and
  `truncate` keep it);
* a cursor of the allocation per group and a group marked dirty and settled
  at the end of a run, not at every block;
* runs of blocks read and written in one call of the device.

Gates: `make x86-volume-smoke` with `e2fsck -fn` of the host clean, a sparse
file, a file past 268 KiB (the double table) and a truncation.

## M4 - MicroFS, version 2

The table of the records is 4 % of the volume whatever it holds: 214 MB and
14 seconds of `mount` on 5 GiB, against 1.8 seconds of `fsck` on ext2.

* ADR 0038: the table sized by the records the volume needs, a **group** in a
  record, and a checksum (or a journal) that tells a torn record from a whole
  one;
* a volume of version 1 still mounts; `mkfs` writes version 2; the migration
  is a command or a refusal with a sentence, and the ADR says which;
* the group reaches `statx`; what a triad does with it is the work of the
  kernel and is recorded in `docs/debts.md`.

Gates: `make x86-volume-smoke` on 5 GiB of MicroFS with `mount`, `df` and
`fsck` timed; a boot with a reset in the middle of a write that `fsck` puts
right; the board's own volume untouched.

## The horizon of 1.8, and the contract it needs now

**2D acceleration** is prepared, not written: a device that can fill and blit
a rectangle on its own - the `virtio-gpu` of QEMU first - behind a contract the
framebuffer device would answer (`fill`, `blit`, `flush`), with the software
path of today as its only implementation. The lane of 1.7 adds the contract
and a host test of it and **no driver**: a driver without a caller is a
driver nobody has measured, and the compositor of M1 has to exist first.

3D (a GL stack, virgl, Vulkan) is not planned; the board has no GPU at all,
and the CPU of a machine this size composites a screen of a kilobyte-scale
fast enough while the rectangles are dirty.
