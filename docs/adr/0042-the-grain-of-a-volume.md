# ADR 0042 - the grain of a volume is a view, and the wall is not the grain

* Status: accepted, implemented
* Date: 2026-11-24
* Related: ADR 0008 (block devices), ADR 0019 (ext2), ADR 0027 (a root that is
  not MicroFS), ADR 0029 (the ext2 writer), ADR 0032 (a buffer a device can
  read: the cache and the read ahead), ADR 0037 (the write of ext2 is a run),
  ADR 0038 (a table of records: the format of MicroFS), `docs/plan-1.8.md`
  (M4), `docs/debts.md`

## Context

The debt of 1.7 said, in as many words: "the grain of the disk is 1 KiB: the
sector of the block layer is the block of MicroFS, and ext2 on that volume is a
block of 1 KiB, so a file costs the block layer about five sector operations
per kilobyte written and nine per kilobyte read - every one of them a call, a
hash of the cache, a copy, and a request of the device."

Half of that sentence was closed by ADR 0037 before this lane: the writer of
ext2 writes and reads a file in runs of 64 KiB, and the block layer gathers a
run into one command of the medium, so the five operations of 1 KiB became
about a tenth of one. What was left of the debt was the *grain* itself, and the
plan of 1.8 asked for it in five steps: a view of a disk at a sector larger
than the medium's, an ext2 of `-b 4096`, every write path of the filesystem
against `e2fsck -fn` at that grain, the same for MicroFS (a decision of the
format), and the numbers of both grains.

The three grains of the system are not the same number and never were:

* the **sector of the medium** - 512 bytes of a platter, a page of the flash of
  the board. The ports report 1 KiB for a disk because the block of MicroFS is
  1 KiB and its reader asks a sector at a time; the driver below multiplies.
* the **block of the volume** - a fact of the format. Ext2 keeps it in the
  superblock at the second kilobyte of the volume, and MicroFS keeps one of
  1024 bytes in its own header.
* the **box of the cache** - the sector of the device the block layer was
  handed (ADR 0032).

A volume cannot know the second number before it has read the first kilobyte of
the superblock, and a driver cannot know it at all: it is not a property of the
medium. That is the whole difficulty of the first step.

## Decision

### 1. The grain of a volume is a view over the medium

The block layer grows a device that is arithmetic over another one:

```c
struct k_blockdev_view view;

if (k_blockdev_view_open(&view, medium, 4096) == 0) {
    mount(k_blockdev_view_device(&view));
}
```

The view multiplies a sector and a count by its factor and hands the medium one
call - no copy, no buffer, no allocation. It has a sector size of its own, a
count of whole sectors of the medium it can name, and the operations, the name
and the context of the medium below; a call of its erase or of its sync is the
call of the medium with the sectors multiplied, and a medium without one names
neither.

The view is **not an entry of the table**. The table is the devices of the
machine (`k_blockdev_find`, `/proc/blockdevs`, the shell), and a volume is not
one of them: a disk of 5 GiB with a root of 4 KiB blocks would otherwise be
listed twice, and `K_BLOCKDEV_MAX` is eight slots. The filesystem owns the
structure, opens it at the mount when its superblock names a grain the medium
does not have, and forgets it at the unmount. The counters of the view
(`reads`, `writes`, `sectors_read`, `sectors_written`) are the measurement of
the test and of this ADR; no code decides anything by them.

### 2. ext2 takes the grain of its superblock

`ext2_volume_mount` reads the superblock through the medium - it cannot do
otherwise, the block size is inside it - and then, when the block is larger
than the sector of the medium, opens the view and reads and writes the whole
volume through it. The reader accepts a device sector up to the largest block
of the format (`EXT2_DEV_SECTOR_MAX`, 4096).

The writer gets the other half of the run: `device_write` gathers a run of
whole sectors into one call the way `device_read` has since ADR 0037. A block
of the volume reaches the block layer as one operation at either grain, and a
run of 64 KiB is one call of the cache.

### 3. The read ahead continues a *caller*, not a call

Writing the tests of the grain found a defect of the read ahead of ADR 0032
that the grain of 1 KiB hid: `cache_read` armed the expectation of the stream
(`jump_next`) from *inside* the call, after the first sector of a miss. A call
of one sector - MicroFS, a walk of the records of a directory - arms it and the
*next call* uses it, which is what it was built for. A call of a *run* - the 4
KiB sector of the view, the 64 KiB read of ext2 - armed it and then matched it
itself, on the second sector of the same call, and read the whole read ahead of
the device (128 sectors, the run of the disk of the port) for a four-sector
record.

The cost was not a copy: it was the medium. A 16 MiB file written at the grain
of 4 KiB asked the disk for 36 MiB of reads.

The rule is now the one the comment always claimed: the expectations are read
once, at the start of a call, and a call that does not continue the one before
it is a call of its own - its sectors go to the medium as one command of the
size of the call and no bigger. A prefetch names its own end, and a call of a
sector names the next sector. The behaviour of a caller of one sector at a time
is unchanged, and the buffered device of a test can see the difference
(`tests/cases/test_blockdev_cache.c`).

### 4. The tools ask the same questions of both grains

* `tools/x86_image.py --block-size 1024|2048|4096` writes the ext2 root with
  `mke2fs -b N`. The default stays 1024: the bytes of the image of the release
  are the bytes of 1.7.
* `tools/x86_volume_smoke.py --block-size both` (the default) boots the volume
  of 5 GiB twice, at 1 KiB and at 4 KiB, and every check names its grain; the
  superblock is read back with `debugfs` so that the grain of the run is the
  grain of the volume and not a comment.
* `tools/x86_bench.py --block-size N` puts the same grain under the table of
  `make bench`.
* `tests/cases/test_fs_ext2.c` writes the file past the twelve direct blocks,
  a hole past the single table, a cut inside a block and a cut that gives a
  table back at 4096-byte blocks, and gives the image to `e2fsck -fn` and
  `debugfs` at every step; the counters of the medium are printed for the same
  megabyte at both grains.

### 5. MicroFS keeps its kilobyte, and says why

The block of MicroFS is the sector of its volume *and* the unit of its records:
the header names it, the table of the inodes is a count of blocks, the map of
the free blocks is one bit per block, and the data section is a run of them.
`mfs_mount` refuses a device whose sector is not the block. A block of 4 KiB is
therefore **version 4 of the format** and not a view: a volume of version 3 has
to keep mounting with its records at the offsets they were written, `mkfs` has
to be able to write both, the map of a 5 GiB volume falls from 615 KiB of the
heap to 154 KiB (a fact in favour), and the check of the volume, the tool of
the host and the self test of the board all speak the block size.

That is a decision of the format with a migration, and it is a lane of its own
(the debt names its shape). This ADR decides the grain of *ext2* and the
mechanism of a view; it does not change the format of MicroFS.

## The numbers

**The volume of 5 GiB, a file of 128 MiB** (`make x86-volume-smoke`, two boots
each, on a quiet machine):

| grain | written | read | e2fsck | the image of 5 GiB costs the host |
|---|---|---|---|---|
| 1024 bytes | 139.9-142.9 MiB/s | 119.1-124.2 MiB/s | clean | 5 MB |
| 4096 bytes | 120.9-123.1 MiB/s | 116.6-118.0 MiB/s | clean | 5 MB |

A boot of QEMU on a loaded host is not a number of a release: one run of this
pair wrote the same file at 85.85 MiB/s at the grain of 1024 bytes, which is
why `make x86-volume-smoke` is a gate of a release and not a check of a push.

**`make bench --fs ext2`, the median of three runs** (a volume of 64 MiB, a file
of 16 MiB, out of the cache of the host):

| grain | `file_write` | `file_read` |
|---|---|---|
| 1024 bytes | 285.71 MiB/s | 444.44 MiB/s |
| 4096 bytes | 186.04 MiB/s | 363.63 MiB/s |

The same table before this lane, at the grain of 1024 bytes, was 242.42 MiB/s
written and 457.14 MiB/s read: the write of the megabyte is 18 % faster
because `device_write` now hands the block layer a run of whole sectors and not
a sector at a time (§2), and the read is where it was.

**The operations of the block layer**: the same megabyte in pieces of 64 KiB on
a volume of 16 MiB with no cache of the block layer between (the test of the
grain, `tests/cases/test_fs_ext2.c`): 110 operations of 1 KiB for the blocks of
1024 bytes and 91 operations of 4 KiB for the blocks of 4096. Per kilobyte
written that is **0.11 against 0.09** at the medium, and about **0.10 at the
block layer either way** - the five of the debt of 1.7 were closed by the run
of ADR 0037 and not by the grain.

**The read ahead** (the defect of §3), a file of 16 MiB written at the grain of
4096 bytes: 36 MiB of reads at the medium before, 2.5 MiB after; and the write
of the same file from 129 to 184 MiB/s.

## Consequences

**Good.** The grain of a volume is a thing the system can now express: the
superblock of a volume names a block of 4 KiB, the block layer sees one
operation of 4 KiB for it, and the volume of the release boots, writes, reads
and passes `e2fsck -fn` at either grain. The image tool, the volume smoke, the
bench and the tests all speak the grain as a parameter, so the next release
compares two rows and not two impressions. The defect of the read ahead that
the grain exposed was a real cost at *every* grain for a caller that reads a
run - a 16 MiB write read 36 MiB of the disk - and it is fixed, with the
behaviour of the caller of one sector unchanged.

**Bad.** **The coarse grain is slower on this disk, and the reason is not a
mystery.** The medium of the port is a channel that moves a command of 128 KiB
at best, and the cost of a write is the command and not the byte: at the grain
of 4 KiB the metadata of the volume - the bitmap of a group, the block of the
inode table, the table of pointers - is four times the bytes for the same
record (a 128-byte inode, a 32-byte descriptor, one bit per block), so the
volume writes 22 384 sectors of the 16 MiB file where the grain of 1 KiB writes
20 796 and issues 459 commands where the other issues 263. A command of 49
sectors is worse than a command of 79 for the channel of the port. The grain of
4 KiB pays on a medium that cannot be asked for 1 KiB at a time (an NVMe of
4 KiB sectors, a flash of 4 KiB pages), and the port has no such medium. On the
volume of 5 GiB the two grains are 141 and 122 MiB/s written, and both are
above the target of the plan of 1.6 - the writing of the release is not the
grain of 1.7 that the debt feared, it is the run of ADR 0037.

**Cost.** The reader of ext2 keeps a buffer of one sector of the device, and the
sector is now as large as the largest block: 3 KiB more of BSS where the reader
is built (the board's `sdkconfig` has `CONFIG_MICRO_OS_FS_EXT2` off today, so
the image of the release did not grow). A write of part of a
block at a coarse grain is a read-modify-write of the sector of the device: a
program that writes a hundred bytes into a volume of 4 KiB blocks reads 4 KiB
first. Ext2's own writer reads and writes a whole block for an inode, so the
price is the block and not the record.

## Alternatives

**A driver registers a 4 KiB view when the superblock says so.** The driver
would have to read the superblock - a filesystem's business - and to know which
kind of volume is mounted on its partition; and a disk whose table says nothing
about the grain would still need the answer. The superblock is the only place
that knows, and the filesystem is the only caller that has read it.

**A partition names its own sector.** The MBR and the GPT have no field for it:
the type byte says what the volume is, not how it is written. A field invented
by this system would be a fact of the table of a disk that a tool of another
system writes, and a volume moved to another table would lose it.

**Register the view in the table.** Tempting, and wrong: the table is the
devices of the machine, `/proc/blockdevs` and the shell list it, a disk of 5 GiB
would appear twice, and eight slots are the budget of the system.

**A device sector of 4 KiB for every volume.** MicroFS refuses a device whose
sector is not its block (its format is measured in blocks of 1024), a partition
inherits the sector of its disk, and the medium of the port is not 4 KiB. The
sector of a device is the fact of the medium; the grain of a volume is the fact
of the format; a view is the one that keeps them apart.

**MicroFS at 4 KiB now.** See §5: it is version 4 of the format with a
migration, and this lane is the grain of the disk of x86-64.

## What this ADR does not do

* it does not give MicroFS a block of 4 KiB: `docs/debts.md` keeps the row, and
  the shape of it is a version 4 of the volume, a `mkfs` that writes both, a
  reader that mounts both, and the map of the free blocks of a 5 GiB volume
  falling from 615 to 154 KiB;
* it does not change the default grain of the image of the release, which stays
  1024 bytes;
* it does not make the metadata of ext2 a whole block per batch: the inode
  table block and the bitmap are still written a run at a time, and a lane that
  wants 4 KiB blocks on a device of 4 KiB sectors should keep the block of the
  inode table in the slot of the volume the way ADR 0037 kept the descriptor;
* it does not give a volume a read ahead of its own: the read ahead is the run
  of the medium (ADR 0032) and the view clamps nothing;
* it does not touch the disk of a chipset (ADR 0041), which is read-only, nor
  the disk of a person: a volume that may not be written is not written at any
  grain.
