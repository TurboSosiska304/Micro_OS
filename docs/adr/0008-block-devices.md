# ADR 0008 - Block devices, transports and the tables of the partitions

* Status: accepted
* Date: 2026-11-21

## Context

`kernel/blockdev.h` has been the interface of the storage since MicroFS was
written: a sector size, a count of them, read, write, an optional erase and
sync. A filesystem asks for sector 17 and does not care what answers. What
answered, though, was never a disk:

* on the board, a partition of the internal flash (`flash0`), programmed and
  erased through the cache of `blockdev_flash.c`;
* in the simulator, a file of the host (`file0`);
* on x86-64, an array of 512 KB in the memory of the port (`ram0`), which is
  a good model of a flash and a poor model of storage: every reset makes the
  volume again, and the program of `/bin` is written into it again at every
  boot.

The platform roadmap asks for a second block device (K6), the kernel roadmap
for a machine that keeps the programs it runs. There is also a larger
question behind the small one: the same interface is supposed to name an SD
card, a SATA disk, a virtio device and a flash chip someday, and nothing in
the tree says how those names, the partitions on them and the units of the
medium relate.

This ADR answers four questions: which media the system names and where
their drivers live; who reads the table of the partitions and what it
believes; what the first real disk of the project is and why it is that one;
and what is deliberately absent.

## Decision

### 1. One interface, several transports

A transport is a driver of a port, never code of the kernel. `components/kernel/`
keeps the registry and the parser of the tables; a driver knows the registers
of one controller and the medium behind it.

| Medium | Name | Driver | Unit | Completion |
|---|---|---|---|---|
| internal flash of the ESP32 | `flash0` | `components/port_esp32/src/blockdev_flash.c` | 1 KiB, cache of 4 KiB erase sectors | polled, the SPI of the chip |
| a file of the host | `file0` | `ports/port_host/src/blockdev_file.c` | 1 KiB | the call returns |
| memory of the x86-64 port | `ram0` | `ports/port_x86_64/src/blockdev_ram.c` | 1 KiB, reads as an erased flash | the call returns |
| **ATA/IDE, the legacy channel** | `ata0` | `ports/port_x86_64/src/ata.c` | 512-byte medium sectors, presented in 1 KiB | polled status |
| virtio-blk | `vd0` | a port component (planned) | 512, presented in 1 KiB | virtqueue, polled or IRQ |
| AHCI/SATA | `sd0` | a port driver over PCI (planned) | 512 | DMA, IRQ |
| NVMe | `nvme0n1` | a port driver (planned) | 512 or 4096 | queues, MSI-X |
| SDHCI: SD and eMMC | `mmcblk0` | a port driver of the board (planned) | 512 | DMA or PIO, IRQ |
| USB mass storage | `sd0` behind a host stack | needs a USB stack first | 512 | bulk transfers |

The names are the names of Linux, shortened to what a person reads at a
prompt: the medium and the controller, then `p` and the number of the
partition - `vd0p1`, `sd0p2`, `mmcblk0p3`, `ata0p1`. The ATA channel is
`ata0` and not `sd0` because the transport is not SCSI and never will be: a
name that lies about the driver is worse than a name that is short.

A device says what one sector of it is (`sector_size`) and how many
(`sectors`), and the calls are in those units. The filesystems of the kernel
speak 1 KiB - `MFS_BLOCK_SIZE` - and refuse a device of another unit, because
the layout of a volume is written in its blocks. So a driver of a medium with
512-byte sectors aggregates them, and `ata.c` reads the two sectors of a block
with one command of the drive. A partition keeps the unit of its parent: the
child is the parent with the first sector of the partition added, and nothing
below the parser knows that a disk exists.

`read`, `write`, `erase` and `sync` keep their meanings: `erase` is the
operation of a medium that needs one (NOR flash), `sync` brings the writes to
the medium (the cache of the drive, the program of the erase sector), and
both are optional - a disk has no erase and a memory takes no sync.

### 2. Where the parser lives

The table of the partitions is read by `components/kernel/src/partition.c`:
portable C11, no register, no port. A table is a property of the medium and
not of the controller - the MBR of a QEMU image is the MBR of a SATA disk, of
a card in a reader and of the image a tool writes on a laptop - so the parser
is the same code on every port, and the tests of the host run it without a
machine.

The port registers the disk and asks for the scan at its init
(`port_x86_64_disk_init`), the boot mounts the volume by name
(`port_x86_64_root_device`), and `mfs_vfs_mount` does not know whether the
name is a disk, a partition or an array.

`partition_scan(disk)` registers a child for every partition of the table and
returns how many, or a `KERR_*` code: `KERR_NOENT` for a disk without a
table, `KERR_INVAL` for a table that contradicts itself, `KERR_NOSPC` when
the kernel has no room, `KERR_IO` when the disk did not answer. The children
are `struct k_blockdev` of their own (`read`, `write`, `erase`, `sync`
forwarded with the offset added), named after the parent, and they live in
static slots: the registry keeps a pointer for the life of the system, so a
child cannot be a stack frame.

The table of the kernel is eight devices (`K_BLOCKDEV_MAX`), which is one
disk, its four partitions and the volume of the port with room to spare. The
registry gained `k_blockdev_unregister` for the scans that replace the
children of a disk and for the media that come and go; the tests use it to
leave the table as they found it.

### 3. What is believed about a table

Two formats, both written in the first sectors of the medium:

* **MBR**: the four entries of sixteen bytes at 446, the signature `55 AA` at
  510, addresses in 512-byte units. The first sector of a partition is never
  LBA 0 and never 0, an entry that ends after the last sector of the disk is
  not an entry, and two entries that claim the same sector are refused.
  Extended partitions (types 5, 0F, 85) are not walked: their chain of EBRs
  is a second format and it waits. The protective entry (type EE) is
  remembered, because an MBR that only protects a GPT whose header is gone
  means a damaged disk and not an empty one.
* **GPT**: the header in LBA 1 (`EFI PART`, revision 1.0, a size of at least
  92 bytes and a CRC32 of the header with its own checksum field zeroed), the
  array of the entries at the LBA the header names, the count and the size of
  an entry (at least 128 bytes), the CRC32 of the whole array, and the range
  of the usable sectors. **Both checksums are verified before a single entry
  is believed**: an array that a crash rewrote is not a table with one bad
  entry. An entry outside the usable range, a partition that ends before it
  starts, an array or a header outside the disk - the table is refused.
  The backup header at the end of a disk is not read: the primary is the one
  the boot has to be able to use, and a disk whose primary is gone is a repair
  of a tool.

The parser is a reader. It never writes to the medium, not even to "fix" a
checksum, and it verifies the bytes of the disk before and after in the test.
A table it does not understand costs a volume; a table it rewrites costs the
data of a person.

The scan is atomic: the whole table is validated first, and then the children
are registered; a broken table registers nothing. A scan of a disk that
already has children releases them first (`partition_release`), so the
registry never holds two children with the same name.

A partition that does not start or end on a whole sector of the device is
refused, and this is why: the calls of a device are in its sectors, so an
offset of 512 bytes inside a device of 1 KiB sectors has no request that can
reach it. The medium of the x86-64 port is presented in 1 KiB units and the
tables are written in 512-byte LBAs; every tool in practice aligns a
partition to a megabyte, so the refusal costs nothing real, and it is
explicit instead of a silent rounding.

**There is no "the whole disk is a volume" special case.** A disk without a
table is not mounted, and a boot that finds one says so. A formatter that
writes over a disk because it could not read the table is how the only copy
of something is lost; a person who wants a volume on a raw disk writes a
table first, and `tools/x86_disk_smoke.py --mkimage` is the smallest example
of that tool.

### 4. Cache, queue and interrupts

What this iteration has: one request at a time, polled, no queue, no cache
of the kernel, and `sync` after every write (the FLUSH CACHE command of the
drive). Every wait is bounded - a drive that stops answering returns an error
and the boot goes on with the volume in its memory - because a machine that
hangs on a disk is a machine that cannot say what is wrong with it.

What performance will want, in the order it will be worth it:

1. **a queue of a few requests** (two to eight) so that a write of eight
   sectors is one command and not eight;
2. **the interrupt of the channel** (IRQ 14, PIC line 6 of the slave, the IDT
   already has the vector): the processor has something to do while the
   medium works, and a task that waits for a sector sleeps;
3. **a write-back cache of the kernel** with an explicit barrier, and a
   readahead of the pages the filesystem is about to ask for;
4. **DMA** (bus-master IDE, AHCI, virtio), which is also the point where the
   port needs a contract from a virtual to a physical address - the paging
   work of the kernel is the owner of that contract.

None of it is in this ADR's code. The order matters: a queue and an interrupt
without a physical address contract are a driver for one machine, and the
project has four ports.

### 5. eMMC and SD on SDHCI: the project, not this iteration

There is no card of this kind on the machines the system runs on today, so
this section is a design and not a driver. It is written down because the
next medium of the board is a card, and because the design shows what the
block layer already has and what it will need.

**The controller.** SDHCI is a register file, usually behind PCI (the card
reader of a laptop) or memory-mapped on the SoC (the board): the argument of
a command, the command and its flags (data present, direction, index check,
CRC check), the response of 32 bits (R1, R3, R4, R5) or 120 bits (R2), the
status of the buffer, the clock divider and the power, the interrupt status
and its mask, and the descriptor table of the DMA (SDMA or ADMA2). Polling
the interrupt status of the controller - never the card - is enough at first,
the same way `ata.c` polls the drive.

**The card.** An SD card is brought up by a script, not by a command:
`CMD0` (idle), `CMD8` (the voltage and the version of the interface, the
answer R7), `ACMD41` (the OCR, repeated while the card is busy; the host
sends `CMD55` before every ACMD), `CMD2` (the CID, R2), `CMD3` (the RCA, R6),
`CMD9` (the CSD, R2 - the size of the card is in it), `CMD7` (select),
`CMD16` (the block length, 512), and then `CMD17`/`CMD18` (read one block, or
many) and `CMD24`/`CMD25` (write one, or many). An eMMC card answers a
slightly different script (`CMD1` instead of `ACMD41`, `CMD6` for the bus
width and the speed, the EXT_CSD as a data transfer of 512 bytes) and has
registers the SD card does not: the boot partitions, the RPMB partition, the
reliable write, the cache. R1 is the status of the card (a bit per class of
error), R2 is a register of 128 bits, R3 is the OCR of the voltage, R6 and R7
are shorter answers; a command without an answer has none.

**The medium.** Blocks are 512 bytes everywhere (a card that reports another
size is not one of these), which is the unit of the block layer below the
1 KiB the filesystems speak - the same aggregation as the ATA driver, and the
same 512-byte sectors of the partition tables.

**What the boot needs before it can mount a card.** The voltage switch of
`CMD11` for 1.8 V is optional and waits; the high-speed mode waits; the
EXT_CSD is read only when the card is an eMMC and only for the boot
partitions, which the kernel does not use yet. A card that is inserted after
the boot is not seen (no hotplug, no polling of the slot), and a card that is
taken out while a volume is mounted is a lost volume - both are the work of
a driver with a card in hand.

**What the design says about the layer.** The card is a device the port
registers under the name `mmcblk0`, its partitions are children of it, and
nothing of the filesystem changes. What the layer does not have and a card
will want is a timeout that means something (the card can stop answering for
a second during an internal write) and a queue of more than one request.

### 6. What is not in this iteration

Stated plainly, so that nobody expects it:

* one disk, the primary ATA channel, master; one controller of one kind; no
  SATA, no virtio, no NVMe, no card;
* PIO, polled, one request at a time; no interrupt, no DMA, no queue, no
  cache of the kernel; the drive's own cache is flushed after every write;
* MBR with primary entries and GPT; no extended or logical partitions, no
  hybrid MBR, no backup GPT, no Apple or BSD labels;
* media with a sector size that divides 1 KiB (512, 1 KiB); a 4 KB-native disk
  needs the re-blocking layer this ADR leaves for later;
* four partitions of one disk in the registry, eight devices in the system;
* no hotplug, no eject, no `mount` of a second volume: MicroFS has one volume
  at a time and the disk replaces the memory volume at the boot;
* no permissions on a device, no owner of a disk, no write protection.

## Consequences

What it bought: the x86-64 machine has a medium that survives a reset
(`make x86-run-disk`, `make x86-disk-smoke`), the tables of the partitions
are a portable part of the kernel with tests of their own, the registry can
lose a device, and the question "where does the driver of the next medium
live" has an answer written down.

The price: the driver of a disk is a file of a port, so a second port that
wants the same ATA code copies it - which is the port boundary working as
designed (ADR 0004), and the reason a board gets an SD driver and not an
abstraction of one. The registry of eight devices is static and small; the
1 KiB unit of the filesystems costs a copy in every driver of a 512-byte
medium; the polling costs the processor the time of the transfer.

Next, in an order that makes sense: the interrupt and the queue of the ATA
driver or, better, a virtio-blk driver once the port has a physical address
contract; a second filesystem behind the VFS, which is what K6 asked for
first; then SDHCI on the board when there is a card to put in it.
