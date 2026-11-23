# ADR 0037 - The second disk: virtio-blk beside the IDE channel

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0008 (block devices), ADR 0012 (many cores), ADR 0032 (the
  address of a buffer a device reads), ADR 0035 (the grain of the I/O),
  `docs/plan-1.6.md` (M3), `docs/bench.md`

## Context

The disk of the x86-64 port is the drive of the primary IDE channel (ADR
0008), and since 1.6 it speaks bus-master DMA with a table of 4 KiB pieces and
the interrupt of IRQ 14 (ADR 0032). That work made the medium fast and left a
number that has not moved since: a file written to an ext2 volume of 5 GiB
goes at about 15 MiB/s where the plan asks for 80, and the measurement of
ADR 0032 says where the time is - 57 % in the sector operations of the block
layer, 3 % in the disk.

There is nothing left to win in the channel: a command of the IDE controller
moves at most 128 sectors of 512 bytes (64 KiB), its PRDT is a table of
physical addresses of at most seventeen pages, and *a descriptor names one
buffer*. A caller that hands the driver 512 sectors of the cache of the block
layer therefore costs eight commands, eight programming rounds of the channel,
eight waits for the interrupt - and the driver has to gather the run in a
buffer of its own, which is one more copy of the data per byte.

The tree has a second bus it already speaks: virtio, in the legacy interface
of the specification, programmed through an I/O window of the PCI bus
(`src/virtio_net.c`, K6). The card is what QEMU offers a machine for storage
(`-drive if=virtio`), it is what every other guest of the world uses, and its
queue has the two properties the IDE channel lacks: a request is a *chain* of
descriptors - so the pages of a run are named, not gathered - and the size of
a request is the size of the queue, not a ceiling of 64 KiB.

## Decision

### 1. The disk is the same contract, another device

`struct k_blockdev_ops` does not change: `read`, `write`, `erase` and `sync`
take a buffer of the caller and a count of sectors, and the filesystems above
it - MicroFS, ext2, FAT - do not know which of the two disks of the port is
under them. `erase` is NULL, as on a disk; `sync` is the flush of the device's
own cache (`VIRTIO_BLK_F_FLUSH`), which QEMU always offers and which is the
same promise the ATA flush makes.

The sector of the device is the block of MicroFS, as on the ATA port, and for
a reason that is the block layer's and not the device's: the unit of
`k_blockdev` is the unit the filesystems were written in, MicroFS refuses a
device whose sector is not its block, and the two sectors of 512 bytes that
make one of them are arithmetic inside the driver. A device whose sector is
4 KiB - the grain that would cut the sector operations of a filesystem by four
- is a change of the format of the volumes on it and not a property of this
driver; it is a row of `docs/debts.md` with the plan, and this ADR does not
take it.

### 2. The legacy interface, and why

The modern interface of virtio 1.0 (the "modern" device id of the PCI
function, a window of MMIO named by a capability) is not spoken, for the
reason `src/virtio_net.c` gave in K6 and which is still true: the MMIO window
of a device of QEMU sits near four gigabytes, the kernel of the port is the
identity map of the first gigabyte plus the image at its fixed offset, and a
driver that touches a register of MMIO would need that window mapped in every
address space of the machine. An I/O window is a port number. QEMU's
transitional device (id `0x1001`) has both, and the driver asks for the legacy
one; a machine whose card offers only the modern interface gets a line of the
log that says so, instead of a disk that is silently missing.

The split virtqueue is shared with the network card (`include/
port_x86_64_virtio.h`, `src/virtio.c`): the register file, the three rings,
the handshake of the reset and the layout of a legacy queue. The second driver
is a user of the first one and not a second copy of the same ring.

### 3. The queue, the requests, the batch

One queue, the size the device answers (128 or 256 entries), and the requests
of one call of the block layer go to the device *together*: the driver fills
the chain of each one - the header of sixteen bytes, the pages of the buffer,
the byte of status - pushes them to the available ring and notifies once, and
waits for every used entry. Eight requests are in flight at once, which is
what makes a run of 512 sectors a batch of the device instead of a command per
sector; the ring is the ceiling and a call longer than it is cut into rounds.

Nothing is copied and nothing is gathered. A piece is cut where the buffer
crosses a page, because two pages of a virtual address are not two frames, and
each piece is translated on its own - the rule of ADR 0032 §2, asked of the
same two calls (`port_x86_64_bus_phys`, `port_x86_64_paging_phys`). A page
that does not translate ends the request with an error: the driver has no PIO
path to fall back to, and the block layer reports the failure to the caller
the way it reports a medium that does not answer.

The completion is the used entry and the byte of status, and the wait is the
interrupt of the card: the number of the line is the one the configuration
space of the function names (QEMU routes it through the PIIX), the handler
reads the interrupt status of the device - which is what takes the line down -
and acknowledges the 8259, and the task halts the core with the interrupts on
(`sti; hlt`) with the budget of the TSC as the limit. A machine whose card
names no line of the 8259 keeps the queue polled and says so.

### 4. A line of the PCI bus is level-triggered, and the 8259 is not

This is the part of the lane that cost the day, and it is written down because
the next driver of a PCI device on this port will meet it again.

The lines of the PCI bus are level-triggered (the ELCR of the PIIX says so for
the ones SeaBIOS routed: 10 and 11 on the machine of the port), the cascade of
the slave is an edge, and the pairs do not mix:

* a handler that reads its device's status *after* a second completion has
  arrived takes the second interrupt down with it, and the master, which
  latched the edge of the cascade in the meantime, then acknowledges a request
  that has nothing behind it: the 8259 answers with the vector of its own line
  (7, or 15 on the slave), and a machine that has no entry for it stops on a
  fault that no device caused;
* a chip acknowledged while its line is still high latches the request again
  between the EOI of the slave and the EOI of the master, which is the same
  vector from the other direction.

Two rules follow. The handler *masks* the line before it acknowledges the two
chips and opens it again after them, which is the dance every kernel of the PC
does with a level-triggered interrupt; and the three vectors of the 8259 that
no device owns - the cascade and the two spurious ones - get a handler of
their own in `port_x86_64_interrupts_init` that acknowledges and returns. The
second rule is the general one: on a machine with a level-triggered line, a
spurious interrupt is normal traffic, and a port that treats it as a fault
stops on a race of a chip.

The IDE channel does not need any of this: its line is edge-triggered, which
is why `src/ata.c` has been fine since ADR 0032.

### 5. The run of a read: the device says how big it is

A device that moves a run per command was read eight sectors at a time, a
number that ADR 0032 measured on a cache of sixteen boxes and on the platter of
the board. Both disks of this port have a cache of 512 boxes and a command of
128 KiB, and the read ahead of a file is the run the block layer asks of the
medium: it is now a field of `struct k_blockdev` (0 is the eight of before, and
the board keeps it), clamped by the boxes of the cache - the staging buffer of
the cache is the largest run it may read, and a device that asks for more is
asking for a buffer that is not there. The disks of the port ask for 128
sectors: the read of a file of 16 MiB out of the cache of the host goes from
134 to 262 MiB/s, and the read of a file of 128 MiB of the volume of 5 GiB from
33.8 to 40.8 MiB/s. The write is untouched by it: its rate is the filesystem's.

### 6. The boot: two disks, one root

`port_x86_64_virtio_blk_init()` runs after the disk of the IDE channel and
registers `virtio0` with the cache of the block layer (512 sectors, write-back,
as the ATA driver does) and scans its partition table into `virtio0p1`,
`virtio0p2`. `port_x86_64_root_device()` prefers, in this order: the module of
the boot (the ISO of ADR 0026), the first partition of the virtio disk, the
first partition of the IDE drive, the volume in the memory (`ram0`). A machine
with a card that carries no partition table is a machine with no root on it
and the drive of the channel answers, so a machine that was given both boots
the fast one and a machine that was given one boots that one.

The image does not change. A disk of this port is a file of the host with an
MBR and a volume behind it, and the bus is a property of the machine: the same
`programs.img` is the disk of the IDE channel and the disk of the virtio
device. The tools therefore choose the bus where they choose the machine
(`tools/qemu_session.py`, `drive_args()` and `disk_name()`), every smoke test
takes `--disk ide|virtio`, and the Makefile takes `X86_DISK_BUS` - IDE by
default, because it is the disk of every release before this one and the one
a machine of QEMU is given with a line everybody knows.

## Alternatives

**A. AHCI.** A controller of the PCI bus with a queue of 32 commands and a
table of 4 KiB pieces for each: the same 64 KiB per command as the IDE
channel's bus master and no scatter-gather, a register file of MMIO (the
problem of §2), and a specification of a thousand pages. It buys nothing the
channel does not already do.

**B. NVMe.** The queue is a ring of commands in memory and a command names
its pages, so it has the shape of virtio with a deeper queue. It is also a
controller that a person cannot attach to a disk of the host without an image
in the format of a namespace, and QEMU's `nvme` device is a machine of its own
to configure. Not in this release.

**C. Keep the IDE channel and make the block layer hand it bigger runs.** The
ceiling of a command is 128 sectors and a descriptor names one buffer, so a
run of 512 sectors is eight commands and a copy through the staging buffer of
the cache. The measurement of ADR 0032 already says the disk is 3 % of the
time; the copies and the calls of the block layer are the rest, and this
alternative does not touch either.

**D. The 4 KiB sector now.** It is the one change that would cut the sector
operations of ext2 by four (a block of 4 KiB is one request and not four), and
it is not free: MicroFS refuses a device whose sector is not its block, the
sector of a device is inherited by its partitions, and an ext2 volume of 4 KiB
blocks is a different image (`mke2fs -b 4096`) whose every write path and the
oracle of `e2fsck` have to be checked. It is a lane of its own with an ADR of
its own; `docs/debts.md` carries the plan and the first number.

## Consequences

* The machine can have two disks, and `make x86-run X86_DISK_BUS=virtio` boots
  the same image on the card. The boot names the device it mounted:
  `root: the root is mfs on virtio0p1`, and the log of the queue says its size
  and its line.
* Every smoke test of the port runs on either bus with one argument, and the
  gate of the release is the same gate twice: `x86-smoke` 378 checks,
  `x86-disk-smoke` 57, `x86-root-smoke` 52 and `x86-volume-smoke` 20 on both.
* The device is a hundredth of the wall clock of a file: the numbers of the
  volume of 5 GiB are the same on both disks, and the lane did not move the
  rate of a filesystem - the read ahead of §5 is what moved, and it moved for
  both. What it did move is the cost of a *run* - the request of the
  block layer that the IDE channel cut into eight commands and a copy is one
  descriptor chain here - and it removed a ceiling: the queue is the limit of
  a request and not 64 KiB.
* A queue of a device of the port can now be waited for by a task: the pattern
  of `sti; hlt` of ADR 0032 is not the property of the IDE channel any more,
  and the interrupt of a PCI line is a thing the port opens and acknowledges.
  The three spurious vectors of the 8259 have a handler for the same reason.
* The next driver of a bus master (AHCI, NVMe) has the rings, the window, the
  translation and the level-triggered line in one place instead of an example
  to copy.

### The numbers

`make bench` and `make x86-volume-smoke`, the same tree, one processor of TCG,
QEMU 11.1.3, the disk of the host behind the page cache:

| What | IDE channel | virtio-blk | Where the time goes |
|---|---|---|---|
| `file_write`, MicroFS, 16 MiB in 64 KiB blocks | 5.39 MiB/s | 5.37 MiB/s | MicroFS, and not the disk |
| `file_read`, the same file | 262 MiB/s | 246 MiB/s | the read ahead and the block layer |
| `file_write`, ext2, 16 MiB | 14.9 MiB/s | 15.2 MiB/s | ext2 and the block layer |
| `file_read`, ext2, the same file | 62.5 MiB/s | 59.5 MiB/s | the same |
| ext2, a file of 128 MiB of the volume of 5 GiB | 13.8 MiB/s, 39.7 read | 13.9 MiB/s, 40.9 read | the sector operations, both ways |

The writes of the disk itself, read out of the statistics of QEMU around a
write of 16 MiB: 447 commands of 38 KiB and 14 ms of medium on the IDE
channel, which is 1.3 % of the 1068 ms the call took. The device is not the machine's problem; it has not been since
ADR 0032. The rate of a file written to ext2 is what a filesystem that asks a
device for five sectors of a kilobyte per kilobyte - and copies each of them
twice in the block layer - costs on a processor that is emulated, and virtio
does not change the number of those calls. The ceiling of 100 MiB/s of the
plan needs the calls to fall by four, which is the 4 KiB grain of alternative
D, and it is written down there.
