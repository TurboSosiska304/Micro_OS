# ADR 0029 - ext2 that can be written

* Status: accepted, implemented
* Date: 2026-11-23
* Related: ADR 0019 (the reader), ADR 0027 (a root that is not MicroFS),
  ADR 0005 (the mode and the owner), ADR 0013 (the moments of a change),
  `docs/plan-1.4.md` (M2.2)

## Context

ADR 0019 gave the kernel an ext2 reader and said, in as many words, that it
never writes: `write`, `mkdir`, `unlink`, `chmod` and `chown` were left out of
the table of the filesystem and `e2fsck` was the tool of the format, not of
the machine. ADR 0027 then made ext2 a candidate for the root of the machine
and named M2.2 as the price: a root needs a `/tmp`, a `/var` and a log, and a
root that cannot make a file is a root for reading only.

The plan bounds the step on purpose: this is not "ext2 of Linux". It is the
write path a root needs, in the shape of the format the reader already walks,
with the tools of the world as the judge.

## Decision

### 1. What is written

* The **bitmaps of a group**: the block bitmap and the inode bitmap are read
  as one block, one bit is changed and the block goes back; the free counts
  of the group descriptor and of the superblock follow it. An allocated block
  is zeroed before an inode names it, so a file never inherits the bytes of
  an object that is gone.
* The **inode**: a whole 128-byte record written back into its block, with
  the fields this writer does not model - an ACL, a fragment, the second
  half of a longer inode - kept for a change of an attribute and zeroed for
  a fresh inode. `i_blocks` counts sectors of 512 bytes, `i_dtime` is the
  moment of a removal and is never zero, and the high half of a file size
  goes where the old layout kept the ACL of a directory.
* The **directory**: a name takes the room of a record that is free when it
  fits, the tail of a record that has room to spare when it does not, and a
  new block when the directory has no room at all. A removal gives its room
  to the record in front of it, so a block stays one chain from its first
  byte to its last; a name that begins a block is marked free in place.
* The **file**: `create` is an inode and a record, `write` allocates the
  direct blocks 0..11 and one table of pointers, `truncate` cuts to zero and
  to a smaller size (the tail of a partly used block is zeroed, the blocks
  and the tables of the tail go back), `unlink` frees the blocks and the
  inode and merges the record. The double and the triple table are read and
  freed - a volume written by Linux must still be removable - but never
  written: a file that would need them is `KERR_NOSPC`, the number of "no
  room", and not a second layout nobody writes.
* `mkdir` (a block with `.` and `..`, the link count of the parent, the
  counter of the directories of the group), `chmod` and `chown` (the twelve
  bits of the mode and the 16-bit uid, with `ctime`).
* The **moments** are `k_clock_now_unix()`, like MicroFS and FAT, and the
  superblock is written when the volume leaves, not on every block.

### 2. A volume that is only read is not written

The volume keeps one `dirty` flag, set by the first allocation or inode
write. `ext2_volume_flush()` writes the counters of the superblock and syncs
the device only when the flag is set, and unmount clears it. A mount that
only lists and reads the medium leaves it byte for byte as it was - the
property ADR 0019 promised and the test `test_read_is_quiet` still sums.

### 3. The order of a change

A block is marked in its bitmap before an inode names it; an inode is marked
in its bitmap before a directory names it; the name is removed before the
blocks and the inode it named are given back. A crash between two steps
leaves an orphan that `e2fsck` finds, never a name that reaches a freed
inode.

### 4. The judge is e2fsprogs

The host tests make the volume with `mke2fs -t ext2`, mount it, change it,
unmount it, write it to a file and give that file to `e2fsck -fn`; `debugfs`
then reads the names and the sizes of the tree. Every image of every
scenario must pass: a file through the table of pointers, two hundred names
in one directory, cuts inside the three levels of a file written by
`debugfs`, a file that hits the wall of the single table, a volume of four
groups, a volume of 4096-byte blocks, and a change of a mode and an owner.
The test skips the scenarios with a printed line when `e2fsprogs` is not
installed; the CI machines of the project have it.

### 5. A check the machine can run

`ext2_check()` reads the volume mounted at a point and reports:

* the counters of every group against the bits of its two bitmaps, and the
  counters of the superblock against the sum of the groups;
* the chain of the directories from the root: every block is a whole number
  of records that covers the block, `.` is the directory itself and `..` is
  its parent, every name is inside the inode table and marked used, its
  directory block is marked used, and no directory is reached twice.

It never writes. It is the minimum of `fsck ext2` and not the whole of one:
it does not account for the blocks of every file, does not look for inodes
that no name reaches and does not repair. A tree deeper than 16 directories
or wider than 256 is named as too large for the check instead of walked with
a map that would hide a loop. The VFS has no operation for a check, so the
call is reached from the tests today and from a `fsck ext2` of the shell
when that command grows the branch.

## Consequences

**Good.** A root may be ext2 and a machine may write its `/tmp` and its log
without a journal. The write path is small enough to read in one sitting and
every image it makes is clean under the tool of the format. The check gives
the machine an answer about its own root after a crash, and the oracle of
e2fsprogs keeps both honest.

**Bad.** The writer speaks one table of pointers: the largest file of a
volume of 1024-byte blocks is 268 KiB, and a program that copies a bigger
file gets `ENOSPC`. There is no `rename` (a name changes by a copy and a
removal, which is not atomic), no `rmdir` because the VFS has no such
operation, no journal, and no repair. A volume of ext3/ext4 is still refused
at the mount. A directory that is grown by this writer is not indexed with
`dir_index`, which is legal ext2 and what a `e2fsck -D` reindexes.

**Cost.** The check visits every directory of the root at each call, and a
`stat` of a file reads its inode and the descriptor of its group with no
cache beyond the one sector of the device: the design of ADR 0019 does not
change for the writer.

## What this ADR does not do

* It does not write the double or the triple table, and it does not grow a
  file past one table of pointers (a later step; the read side and the free
  side already speak all three).
* It does not add `rename`, `rmdir`, a symbolic link or a device node to the
  VFS contract or to the filesystem. (1.5 adds `rename` and `rmdir` to both,
  and `ext2_repair` behind `fsck ext2 -y`; the rest of this list stands.)
* It does not replay a journal, and it does not repair a volume: `-y` is a
  later `fsck ext2`, and `e2fsck` of the host stays the tool of the format.
* It does not make the board boot from ext2: `main/src/main.c` still
  registers only MicroFS and FAT (the debt of ADR 0019), and ADR 0027 is the
  x86-64 machine.
