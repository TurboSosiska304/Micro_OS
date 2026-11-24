# ADR 0037 - the write of ext2 is a run of the medium

* Status: accepted, implemented
* Date: 2026-11-23
* Related: ADR 0029 (the writer), ADR 0019 (the reader), ADR 0032 (the bus
  master and the cache of the block layer), docs/debts.md

## Context

ADR 0029 gave ext2 a writer and named its cost in `docs/debts.md`: a write of
a block was the bitmap of the group read and written, the descriptor of the
group read and written, the inode and the data, and the block was zeroed
before the data of the caller was written to it - eleven sectors of 1 KiB for
every kilobyte of a file - with the bitmap of the group scanned from its
first bit for every allocation. The page of the volume of the gate was a file
of 128 MiB written at **14.4 MiB/s** and read at **34.7 MiB/s**, while the
disk of the same machine moved 16 MiB in 31 ms (about 500 MiB/s) through the
bus master of ADR 0032.

The block layer above the disk is not the problem: it holds 512 sectors of
1 KiB back (write-back) and hands the medium a run of them in one command.
The writer broke that run. The data of a block arrived between two touches of
the metadata of its group, so the sectors that left the cache together were
two or three of them, and the medium saw a command every kilobyte or two.
The reader had a cost of its own: the block of a file was named by a table of
pointers, and the reader asked the cache of the device for four bytes of it
for every kilobyte of the stream.

## Decision

### 1. The group of the writer is a slot of the volume

The descriptor of one group and one of its two bitmaps stay in the memory of
the volume while it is the group of the last change: `struct ext2_group
group`, `group_cached`, `group_dirty`, and the bitmap with `bitmap_kind` and
`bitmap_dirty` (1 KiB of the 4 KiB of the largest block, plus the 32 bytes of
the descriptor). `ext2_group_read` and `ext2_group_write` answer from that
slot, a group that is not the one of the slot settles it first, and
`ext2_write_flush` is the one place that writes the bitmap and then the
descriptor. It is called by `ext2_volume_flush` (a settle, an unmount, the
counters of the superblock), by a change of group, and by the writer at the
end of a run of the medium.

The counters in the memory are the truth while the slot is dirty; nothing
reads the medium for them. `ext2_write_state_drop` flushes the slot and
empties it, because the repair pass writes the bitmaps of the volume itself
and a copy of the slot above that write would put the old bits back; the
check of the volume drops it too, so that a check reads the volume the writer
made and not the last settle of it.

A volume that was only read writes nothing: the slot is clean and
`ext2_volume_flush` returns before its first byte.

### 2. The cursor of the allocation

`alloc_hint` and `alloc_cursor` (and `inode_hint`, `inode_cursor` for the
inodes) are the group of the last allocation and the bit after the last one it
gave. `group_alloc_run` scans from the cursor and wraps at the end of the
group, a byte that is all ones is skipped as a byte and not as eight bits, and
a freed block puts the cursor back on itself - the room of a removal is the
room of the next object. The blocks of a file are the blocks of its group in
order, and the scan of a bitmap of 8192 bits is one test and not a walk.

### 3. A run of blocks

`ext2_alloc_blocks(vol, want, &block, &count)` takes `want` consecutive free
bits of a group as one change of the bitmap and answers how many it gave;
`ext2_alloc_block` is that call with one. The run never crosses the end of the
group and never asks for more than 64 KiB of the volume (`EXT2_IO_BYTES_MAX`).

A file is written in runs:

* `file_run` plans the run of the write from the logical block on. A hold of
  holes of the end of a file is allocated as one run and named once; a chain
  of blocks the file already holds is taken together when the numbers follow
  one another; a run stops at the table that names its blocks, so the entries
  of a run are one block of pointers.
* `inode_store_run` names the run: the twelve entries of the inode itself, or
  one read, one write and the entries in the memory of the table of the leaf.
* `ext2_file_write` writes the data of a run with one call of the medium
  (`ext2_write_bytes` of up to 64 KiB). The blocks of the run reach the
  bitmap of the group, and the bitmap reaches the medium, before the bytes of
  the file do.

The reader has the other half: `ext2_block_run` reads the leaf table of
pointers once and walks its entries in the memory of the volume, so a run of
64 KiB of a file of the double or the triple indirection is one read of the
table and not sixty-four reads of four bytes; `ext2_read_file` then reads the
bytes of the run with one call of the medium, and a hold of holes is one
`memset`. The call of the medium that is a run of whole sectors goes to the
device in one `read` (`device_read`), which is the run the block layer turns
into the commands of the bus master.

### 4. A new block is not zeroed

`ext2_alloc_block` no longer writes a block of zeros before the caller writes
its data: the byte of a block that the file does not reach is the byte of a
file that is a hole, and a hole is a block of the map that is zero and not a
block of the medium that holds zeros. What is zeroed is what has to be:

* a table of pointers is allocated zeroed - every entry of a table names a
  block of the volume, and an entry that is the bytes of an object that is
  gone is a volume e2fsck calls broken;
* a part of a block of a file that a write does not cover is zeroed in the
  buffer of the volume, and the block is then written whole: the bytes past
  the end of the file read as zeros even when the file grows into a block it
  already holds;
* the tail of the block that a `truncate` keeps is zeroed, as it was, and a
  file that grows is a hole and stays one.

The new block keeps no read of the medium either: the writer of part of a
block zeroes the buffer instead of reading the block it does not own.

### 5. The order of a change, once per batch

ADR 0029 §3 is kept and its unit is the run: a block of a file reaches the
medium only after the byte of the bitmap that names it (the writer flushes the
slot before the data of a run and before a new block of a directory), an
inode is marked before a directory names it, and a name is removed before the
blocks it named are given back - the bitmaps of a removal are written by the
settle of the operation and not per block.

## Consequences

**Good.** The file of 128 MiB of `make x86-volume-smoke` (a volume of 5 GiB,
a block of 1 KiB) is written at **132-147 MiB/s** and read at **80-88 MiB/s**
(`/bin/mkbig verify`, which compares every byte; the same file read by `bench
fileread` is 185 MiB/s). The table of `make bench --fs ext2` (a volume of
64 MiB, a file of 16 MiB) moves from 15.05 to **246.15 MiB/s** written and
from 50.95 to **200.00 MiB/s** read. The disk is no longer the reader of a
document and the writer of a kilobyte. `e2fsck -fn`
of the host finds nothing in every image of the session, and the writer keeps
the shape of the format: a sparse file, a file of the double and the triple
table, a truncate and a directory are the ones ADR 0029 described.

**Bad.** The volume carries the state of a group: 4 KiB of bitmap and the
descriptor, in the struct of the one volume of the filesystem. A crash loses
the dirty slot - the bits of one run, never the bytes of a call that
returned, because a settle writes the slot, the counters and syncs the drive -
and e2fsck finds blocks that the map calls free and that an inode of the last
run names as its own; the repair of `fsck ext2 -y` or e2fsck puts the bits
right. The window of a crash is a run of blocks and not a block.

**Cost.** The change of a group costs a flush of the slot: a file that
spreads over many groups of a volume settles more often than the blocks would
suggest (one descriptor and one bitmap per group of 8 MiB of a volume of 1 KiB
blocks). A create is not faster (1.55 s to 1.53 s for a thousand files): the
price of that number is the two settles of a create and a close - the
superblock and the sync of the drive - and not the allocator, which is a
write of the medium behind the settle of ADR 0029.

## What this ADR does not do

* It does not give ext2 a read ahead of its own. The block layer reads ahead
  eight sectors, so a stream of a file is a command of 8 KiB of the medium;
  the reader asks for 64 KiB and gets eight of them. A file read at 180-200
  MiB/s; the rest of the rate is the granularity of `K_BLOCKDEV_READ_AHEAD`,
  which is a decision of the block layer (ADR 0032) and not of this lane.
* It does not index a directory: the insert of a name still scans the blocks
  of the directory for room, which is the cost of a thousand names in one
  directory and not that of a file.
* It does not cache the table of the descriptors: a file that spreads over
  two groups reads the descriptor of the second one once per change of group.
* It does not write the inode once per run of the medium; a write of the
  system call writes the record of the file at its end, as ADR 0029 said.
* It does not touch the format: no field, no feature bit, no layout. A volume
  written by this writer is the volume of ADR 0019 and of e2fsprogs.
