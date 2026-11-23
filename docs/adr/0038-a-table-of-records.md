# ADR 0038 - A table of the records that is sized by the records

* Status: accepted, implemented
* Date: 2026-11-23
* Related: ADR 0005 (the mode and the owner), ADR 0013 (the moments of a
  change), ADR 0022 (the layout of 1.1), ADR 0029 (the gate of a volume of
  5 GiB), `docs/memory.md`, `docs/debts.md`

## Context

`META` of a volume is the map of the free blocks and the table of the
records, and the table is what `META` is mostly made of. The format of 1.0
sized `META` as **four percent of the volume**, took the map out of it and
gave the rest to the table (ADR 0022). On the flash of the board that is
right: 4% of 8 MB is 82 blocks, the map takes one and the table 81, which is
2,591 records for a volume of 66 objects.

On a volume of 5 GiB the same rule gives `META` 209,674 blocks: 615 for the
map and **209,059 for the table** - 214 MB of table for the 88 objects of
the image of the release, and 6,689,887 records of which 319 are used. The
table costs the mount, every `df` and every `fsck` a walk of 209 MB, and the
cost is not the table's bytes but the walk of it: 13.8 s to count the used
records at the boot, 14.8 s in every `df` (the gate of ADR 0029,
`make x86-volume-smoke`, `docs/debts.md`). The cache of a block of the table
took the 32 reads per record out of that walk; what is left is the size of
the table itself, and it follows the volume and not the objects.

The number of records a volume needs is a property of the tree on it, not of
its size: a volume of 5 GiB that holds 88 objects needs 88 records. And the
count is not fixed for ever either: a person can add files to a volume until
it is full, so a table cannot be sized once at the format for the objects
that happen to be there.

Two more things were owed to the volume from the same list: the record has
no **group** (the owner has fifteen bits of the flags, the group has none,
and `struct vfs_stat` is frozen), and a record has **no checksum**, so a
write that a power cut stopped in the middle of the 32 bytes of a record was
a record the reader could not tell from a whole one (ADR 0013 says so in as
many words).

## Decision

### 1. The version: 3

The version of a volume says which record the table holds and where the
table is. Version 3 is the format of this release:

| `version` | Record | `features` | Read by |
|---|---|---|---|
| 1 | 16 bytes, no time | `CRC` | every version |
| 2 | 32 bytes, `mtime` and `ctime` | `CRC \| TIME` | every version |
| 3 | 64 bytes: the record of 2, the group, the padding and a checksum | `CRC \| TIME \| RECORD_CRC` | this one |

The task of the release asked for "volume version 2". Version 2 was taken by
ADR 0013 (the moments of a change, shipped in 0.4), and a version of a
volume is a promise that is never reused: a volume of version 2 must keep
meaning what it meant when it was written. The new format is therefore
version 3, and versions 1 and 2 are read exactly as they were.

### 2. `META` is the map and the table, and nothing else

The formatter sizes the table by the **number of the records**:

```
table_blocks = ceil(R * 64 / 1024)         R records of 64 bytes
R            = max_records + max(max_records / 4, 64)
DATA         = the volume - META - INFO
META         = table_blocks + ceil(DATA / 8192)   (the map, one bit per block)
```

`max_records` is the argument of `mfs_format()`, the same one the format
always had: the number of records the caller knows it needs. The reserve of
a quarter (64 at the least) is what makes a volume usable without a growth
of the table for a while; the mount of a blank device asks for 256, the
builder of an image for 256, so a volume of the release has a table of 20
blocks.

The layout of the two parts inside `META` changed with the version, and the
reason is the growth of the table:

```
version 1 and 2:            version 3:
+----------+--------+       +--------+----------+
|   map    | table  |       | table  |    map   |
+----------+--------+       +--------+----------+
meta_start  INFO           meta_start  INFO
```

The map of a version 3 volume is anchored at the **top** of `META`, next to
`INFO`, and the table below it. The table grows downwards into the data
section, and a record keeps its address for the life of the volume: the
record `i` of the table of a version 3 volume is the `i`-th slot counted
from the top of the table, not from its start.

### 3. The record of 64 bytes

```
offset  0  flags        the type, the mode and the uid, as ADR 0005 packed them
offset  4  first_block
offset  8  size
offset 12  entries
offset 16  mtime        the two moments of ADR 0013
offset 24  ctime
offset 32  group        the gid of the object
offset 36  reserved     zero
offset 40  padding      20 bytes, zero
offset 60  crc          CRC32 of the first 60 bytes
```

The first 32 bytes are the record of ADR 0013 unchanged, so a reader of the
fields of 1.0 reads a version 3 record without a second layout. The group
needed a word of its own because the 32 bits of `flags` were full - two of
the type, nine of the mode and fifteen of the uid - and taking bits from the
uid or the mode of a volume that mounts would have been a silent cut of a
number.

A record is 64 bytes, which divides a block of 1024 into sixteen: a record
never crosses the border of a block, so a record is written and read as one
block, and the write of a record is the commit of the object.

### 4. The checksum: CRC32, and why not SHA-256

**CRC32, of the first 60 bytes of the record, in the last word of it.** The
question a record has to answer is not "did somebody change this on purpose"
- there is no signature in this system and no threat of one - but "did the
write of 1 KB that this record was stop after the first byte", and a CRC32
of 60 bytes answers it with a probability of error of 2^-32. SHA-256 would
answer the same question with 2^-256 at the price of 32 bytes out of 64,
which is the whole room the padding and the word of the group take, and of a
hash that the checker of the volume would have to carry. The two other
checksums of the format (the header and the map, of the format of 1.0) are
CRC32 as well, so the volume has one polynomial and one function.

The **header** gained its checksum with the version: a version 3 header is
longer than the 48 bytes the old one covered, and the fields it added
(`bitmap_blocks`, `inode_blocks`, `inode_size`, `inode_records`) decide
where the table is. The checksum of a version 3 header therefore covers the
whole header with the field of the checksum empty; the checksum of a
version 1 and 2 header keeps its 48-byte span, which is what those volumes
were written with.

### 5. The growth of the table

A table that runs full grows by four blocks (64 records) into the data
section, under the lock of the filesystem:

1. the blocks of the top of the data section are taken; a block that an
   object holds is **moved** to a free block below the new end, its bytes
   copied and the pointer of its chain patched - the data survives;
2. the replacements are marked in the map and the map reaches the device
   *before* a chain names the new blocks, so a power cut between the two
   loses a block to the map (which the check gives back) and never the data;
3. the blocks of the tail become empty records;
4. the header is written last: it is the commit of the growth, and a volume
   that loses the power before it is the volume it was, with a few blocks of
   its tail moved and nothing lost.

Only a version 3 volume grows. The table of a version 1 and 2 volume is the
whole of `META` and the volume keeps the size it was formatted with.

### 6. The check: a record that does not checksum is not a record

`mfs_check()` and `mfs_repair()` read a used record whose checksum fails as
a **free record**: the object it would have described was never committed.
Its name is removed from its directory (an entry that points at a free
record, the rule the repair already had), its blocks go back to the map, and
the record is written back empty. A record of version 1 and 2 has no
checksum and the old rules are all it has.

A reader of the volume (`mfs_open`, `mfs_stat`, a lookup) answers `MFS_EIO`
for a record that does not checksum: a program never reads the fields of a
write that did not finish. `mfs_get_info()` - the count behind `df` - reads
the flags of a record without the checksum, because a `df` on a volume that
`fsck` is about to repair has to answer.

What the checksum does **not** promise: the record of an object is written
in place, so a record that a power cut tore is the *whole* object's record
lost, not its previous version - there is no journal and no second copy.
The bytes of a file have no checksum either: a content that a reset stopped
in the middle is not detected, and the repair promises a volume that mounts
and walks, not the bytes of the write that failed. Both are in
`docs/debts.md`.

### 7. The group

The record of version 3 has a word for the gid, `mfs_inode_group()` reads it
and `mfs_vfs.c` answers it through `statx`; a write of the object keeps it,
which is the property that made the storage worth having now. `mfs_chgrp()`
sets it and a volume of version 1 and 2 answers `MFS_ENOTSUP` - a number that
would not come back is refused, the rule the mode already followed.

`struct vfs_statx` gained the field `group` (after `owner`), and so did
`struct microos_statx`, so the ABI of `KSYS_STATX` grew from 40 to 48 bytes:
the call copies the structure whole, so the two headers and the kernel move
together or not at all. The door of the system calls that *sets* a group and
the triad of the group in the check of the access (`vfs.c`) belong to the
change that gives the groups a meaning; this one is the storage and the
answer of `statx` (`docs/debts.md`).

### 8. Compatibility and migration: read the old, format the new

A version 1 and 2 volume mounts, reads, writes and checks exactly as it did:
same layout, same record, no rewrite at the mount and no rewrite at the
first write. It keeps its 4% table and its slow walk - a volume of 5 GiB of
version 2 is the volume of 5 GiB of 1.6, 13.8 s of boot and all - and
`df`/`fsck` of it are the price of its own format, not of this one.

There is **no conversion command**, for the reason ADR 0013 gave for the
times: the table of an old volume would have to be rewritten in the place of
the records of the old one, the map of an old volume sits where the table of
a new one begins, and a power cut in the middle of that rewrite leaves a
volume with two half tables and no way to tell which is which. A version 3
volume is made by `mkfs` (or by the mount of a blank device) and the files
are copied to it; the mount of an old volume says one line about it.

### 9. What it costs

| | 16 MiB volume | 5 GiB volume |
|---|---|---|
| `META` of 1.6 | 656 blocks (656 KB) | 209,674 blocks (214 MB) |
| `META` of 1.7 | 22 blocks (22 KB): 2 map + 20 table | 660 blocks (660 KB): 640 map + 20 table |
| records of the table | 2,591 | 319 |
| boot to the shell | 0.27 s | **13.8 s -> 0.3 s** |
| `df /` | 0.02 s | **14.8 s -> 0.07 s** |
| `fsck` | 0.06 s | **0.11 s** |

The three numbers in bold are the gate of `make x86-volume-smoke` on the disk
of QEMU (the old ones are the ones `docs/debts.md` recorded for 1.6). The
walk of the table is gone: the table of a 5 GiB volume is twenty blocks, and
what a mount, a `df` and a `fsck` cost now is the map of the free blocks
(640 KiB of 5 GiB, one bit per block) and the chains of the files.

The table of a 16 MiB volume is *smaller* than it was (a table of 2,591
records for 66 objects became a table of 320) and the data section took the
difference; the table of a 5 GiB volume is 660 KB instead of 214 MB. A
record grew from 32 to 64 bytes, so a table of the same number of records is
twice as large in bytes - which the sizing makes irrelevant.

## Consequences

**Good**

* The mount, `df` and `fsck` cost the number of the records and not the size
  of the volume: a volume of 5 GiB boots in 0.3 s and answers `df` in 0.07 s.
* A record answers for itself: a write of a record that a power cut stopped
  is found by `fsck`, repaired, and costs the one object it was.
* The group has a place on the medium and reaches a program through `statx`.
* The table grows with the volume's needs, so a volume made for a small tree
  does not waste a share of itself and does not refuse the 300th file.
* The old volumes mount unchanged, and the old checksum of their header is
  still the one it was.

**Bad**

* A record of version 3 is twice the bytes of a record of version 2, so a
  table of the same record count costs twice the blocks - and `mkfs` asks
  for the same 256 records, so a small volume has fewer data blocks.
* `struct vfs_statx` grew by a word: the ABI of `KSYS_STATX` changed from 40
  to 48 bytes and a program of an older tree has to be rebuilt. The tree has
  one build, so nothing in it breaks, but the promise "a structure of the
  ABI does not grow" was broken once and this is the record of it.
* A record of an object written twice has one copy, and a reset in the middle
  of the second write loses the record of the object and not only the write.
  A journal is the answer and is not in this release (`docs/debts.md`).
* A volume of version 1 and 2 keeps the format it has: no group, no
  checksum per record, and a table of 4% of itself. Only a new volume - or a
  copy of the files to one - has the format of this release.
* The growth of the table moves blocks of files when the tail of the data
  section is not free: it is one walk of the table and a copy of a few
  blocks, under the lock, on the operation that happened to be the one that
  needed the 321st record. Nothing a person will notice, and not free.
