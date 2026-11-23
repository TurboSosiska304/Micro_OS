# MicroFS (MFS) filesystem

> Russian version: [memory_ru.md](ru/memory_ru.md)

## 1. Overview

MicroOS uses its own filesystem — **MicroFS** (MFS). The whole available capacity of the drive is divided into three sections. Each section consists of a whole number of 1024-byte blocks.

```
+--------------------------------------+
|              MFS VOLUME              |
+--------------------------------------+
|  DATA                                |
|  File and directory data             |
|  Size depends on the drive           |
+--------------------------------------+
|  META                                |
|  Index + record table                |
|  Size depends on the object count    |
+--------------------------------------+
|  INFO                                |
|  MFS header                          |
|  Fixed number of blocks              |
+--------------------------------------+
```

| Section | Purpose |
|---|---|
| `DATA` | storage of file and directory data |
| `META` | indexing tables, access rights, etc. |
| `INFO` | the most important section: data about the FS itself (block size, section offsets, etc.) and about the root directory |

## 2. Sections and their addresses

MFS computes the sections from the drive capacity when it formats a volume,
then writes the result into the header; the mount checks the header against
the design of the volume instead of trusting a fixed address. Each section is
described by three parameters:

| Field | Meaning |
|---|---|
| `start` | number of the first block |
| `blocks` | number of blocks |
| `size` | size in bytes |

Section layout inside the volume:

```
block 0                                         block N-1
   |                                                |
   v                                                v
   +-----------------------+---------------+--------+
   |         DATA          |     META      |  INFO  |
   +-----------------------+---------------+--------+
   0                  META_start      INFO_start    N
```

Input parameters:

| Symbol | Meaning |
|---|---|
| `N` | total number of blocks in the volume |
| `I` | number of blocks in the `INFO` section |
| `M` | number of blocks in the `META` section |

Offset and size formulas:

| Quantity | Formula |
|---|---|
| `INFO_start` | `N - I` |
| `META_start` | `N - I - M` |
| `DATA_start` | `0` |
| `DATA_blocks` | `N - I - M` |

> **Advantage:** the formatter works for any capacity without a table of
> sizes per volume: it computes the sections from `N`, and the mount later
> refuses a header whose sections do not fill the device it was given.

## 3. Example for different sizes

The rules of the format of today (version 3, ADR 0038):

* a block is 1024 bytes;
* `INFO` is the last erase sector of the volume: up to 4 blocks of 1 KB, so
  that the header is alone in its sector;
* `META` is the bitmap of the free blocks **and the table of the records, and
  nothing else**: the table is sized by the number of records the format asks
  for, and the map by the size of `DATA`;
* `DATA` takes all the remaining space.

The table holds the records asked for plus a reserve of a quarter of them,
64 at the least, and grows by four blocks into the data section when the
reserve runs out. The mount of a blank device and the builder of an image ask
for 256 records, so the table of such a volume is 20 blocks on every volume,
small or of 5 GiB.

`META` of a version 3 volume keeps the map of the free blocks at its **top**
and the table below it, so that the table grows downwards without moving the
map and a record keeps its address for the life of the volume. A volume of
version 1 and 2 keeps the map at the start of `META` and the table after it.
For a volume formatted by the mount:

| Volume size | Total blocks | DATA | META | INFO | of it: map | of it: table |
|---|---|---|---|---|---|---|
| 64 KiB | 64 | 39 | 21 | 4 | 1 block | 20 blocks (319 records) |
| 256 KiB | 256 | 231 | 21 | 4 | 1 block | 20 blocks (319 records) |
| 1 MiB | 1024 | 999 | 21 | 4 | 1 block | 20 blocks (319 records) |
| 16 MiB | 16384 | 16358 | 22 | 4 | 2 blocks | 20 blocks (319 records) |
| 5 GiB | 5,241,856 | 5,241,192 | 660 | 4 | 640 blocks | 20 blocks (319 records) |

The table is twenty blocks on every volume; what changes with the volume is
the map of the free blocks, one bit per block of `DATA`. The table of a
version 1 and 2 volume took the whole of `META` after the map, so the same
16 MiB volume had 656 blocks of it and the 5 GiB volume had 209,674.

## 4. Sizing META by record count

The table of the records is sized by count, not by percentage (ADR 0038):

```math
META_table = ceil(R * 64 / 1024)
R          = E + max(E / 4, 64)          (E = the records the format asks for)
META_map   = ceil(DATA / 8192)
META       = META_table + META_map
```

where 64 is the bytes of a record of version 3. The mount passes `E = 256`,
so `R = 320` and the table is twenty blocks on every volume. A table that
runs full grows by four blocks (64 records) into the data section: the blocks
of the top of `DATA` are taken, a block that an object holds is moved to a
free block with its chain pointer patched, and the header is written last,
which is the commit of the growth (ADR 0038).

The growth is what makes the table belong to the volume's tree and not to the
volume's size: a volume of 5 GiB starts with 320 records and takes the 321st
file through one growth, and a volume whose data section is full answers "no
space" the way it always did. The growth moves the blocks of the files at the
top of the data section into free space, so it waits for the files to close:
a create that needs records while a descriptor of the volume is open answers
"no space", and the one after the close grows the table (ADR 0038 §5).

## 5. Data stored in INFO

Instead of fixed addresses, `INFO` stores a universal description of the regions. The header additionally holds `magic`, `version`, `block_size`, `total_blocks`, `root_inode`, `features` and `crc`.

The `version` says which record the table holds: 1 is the 16-byte record of
the volumes before the times, 2 is the 32-byte record with `mtime` and
`ctime`. `features` has the bit `MFS_FEATURE_CRC` (the header carries its
checksum) and the bit `MFS_FEATURE_TIME` (the records carry the two moments);
the two bits go together with the version, and a header that disagrees with
itself is refused at the mount. A version 1 volume has neither the bit nor
the room, and it is read as it always was (ADR 0013).

```
INFO
+--------------------------------+
| magic                          |
| version                        |
| block_size                     |
| total_blocks                   |
+--------------------------------+
| data_start                     |
| data_blocks                    |
+--------------------------------+
| meta_start                     |
| meta_blocks                    |
+--------------------------------+
| info_start                     |
| info_blocks                    |
+--------------------------------+
| root_inode                     |
| features                       |
+--------------------------------+
```

## 6. The record of an inode

A record of MicroFS of today is 64 bytes: four words of the design, the two
moments of a change, the group and a checksum. A volume of version 2 has a
record of 32 bytes (the four words and the moments) and a volume of version 1
one of 16 (the four words); the mount reads all three and the version of the
header decides which one the table holds (ADR 0013, ADR 0038).

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | `flags`: the type, the mode and the uid |
| 4 | 4 | `first_block` |
| 8 | 4 | `size` |
| 12 | 4 | `entries` |
| 16 | 8 | `mtime`, the last change of the content |
| 24 | 8 | `ctime`, the last change of the record |
| 32 | 4 | `group`, the gid of the object |
| 36 | 24 | reserved and padding, zero |
| 60 | 4 | `crc`, CRC32 of the first 60 bytes |

The first 32 bytes are the record of ADR 0013 unchanged, so the two
machines of the record agree field by field. `group` is the gid of the
object (ADR 0038); a record of version 1 and 2 has no room for it and reads
as zero, the group of root. `crc` is CRC32 of the first 60 bytes: a record
is written and read as one block of 16 records, and a record that a power
cut left half written is one whose checksum does not match, which the check
of the volume names and repairs (ADR 0038 §6).

`mtime` is the last change of the content (create, write, truncate) and
`ctime` the last change of the record (the same moments, and also `chmod`
and `chown`). Both are the seconds of Unix in 64 bits, fed by the clock of
the kernel, not by a library of the caller. A version 1 record has no room
for them and the reader answers zero, which means "unknown" and not 1970.
There is no access time: a read does not rewrite a record.

The four first words have not moved, and they still have no field of their
own for the mode and the owner: the two numbers are packed into the upper
bits of `flags`, while the two low bits keep the type they always had. The
volume of 0.3.x therefore mounts unchanged (ADR 0005).

```
bit  31          17 16        8 7      2 1  0
     +-------------+-----------+--------+----+
     | owner (15)  | mode (9)  | unused |type|
     +-------------+-----------+--------+----+
```

| Bits | Field | Meaning |
|---|---|---|
| 0-1 | type | `0x01` the record is used, `0x02` a directory; a file is `0x01`, a directory `0x03` |
| 2-7 | - | unused and zero |
| 8-16 | mode | nine bits, `rwx` for the user, the group and the others (`MFS_MODE_MASK`) |
| 17-31 | owner | fifteen bits, a uid (`MFS_OWNER_MASK`) |

The rules that go with the layout:

* **The masks are masks of the values, not of the fields.** A number is moved
  by its shift before it is put into `flags`, so `0755` is stored as
  `0755 << 8` and a uid of 42 as `42 << 17`; the constants are
  `MFS_MODE_SHIFT`/`MFS_MODE_MASK` and `MFS_OWNER_SHIFT`/`MFS_OWNER_MASK` in
  `mfs/mfs.h`.
* **Two readers and two writers, no more.** `mfs_inode_mode()` and
  `mfs_inode_owner()` are the only places that shift the word down;
  `mfs_chmod()` and `mfs_chown()` are the only places that change a number in
  a record that exists (the creation of a record writes the defaults). The
  directory entry, the VFS adapter and the shell see the two numbers, not the
  bits.
* **A number that does not fit is refused, not cut.** A mode outside the nine
  bits or an owner outside the fifteen is `MFS_EINVAL`, so a wide number can
  never spill into the type of the object and turn a file into a directory.
  The door of the syscall layer refuses the same numbers before the
  filesystem is asked.
* **Mode zero means "not set", not "nobody".** A record written before the
  modes existed has nothing in those bits, and the reader answers with the
  mode of the design for its type: `0755` for a directory, `0644` for a
  file. Nothing is rewritten to make that true, a listing reads the same
  fallback, and a mode that is on the volume wins over it. The price is that
  `chmod 000` cannot be expressed: it is refused instead of being stored and
  read back as `0644`.
* **New objects are born with the mode of the design**: `0644` for a file and
  `0755` for a directory, owner 0. The create path carries no uid of the
  caller, so a task of another uid makes a file of root until `chown` moves it
  (`docs/debts.md`).
* **The numbers cross the VFS as they are.** `struct vfs_stat` carries `mode`
  and `owner`, and `vfs_chmod()`/`vfs_chown()` are operations a filesystem may
  have or not; a virtual filesystem without them answers "not permitted"
  rather than storing a number that nothing would read.

## 7. Possible improvements

The state of the automatic layout:

- [x] Determine the `META` size at format time: the map of the free blocks
      and the table of the records, and nothing else (ADR 0038; 4% was the
      first guess of the format before it).
- [x] Compute the `FILE TABLE` capacity from the records: the table is the
      records the format asks for plus a reserve, and it grows into the data
      section when the reserve runs out (ADR 0038). The table of a 5 GiB
      volume is 20 blocks (319 records) and the walk of the count at the
      mount, in `df` and in `fsck` is 0.07 s instead of 14.8 s.
- [x] A checksum in every record of a version 3 volume: a write that a power
      cut stopped is named by the check and repaired (ADR 0038).
- [x] The group of an object in a record of version 3, answered through
      `statx`: the triad of the group in the check of the access is still
      `docs/debts.md`.
- [x] Account for the free-block bitmap in the calculation (the map is the
      memory of the volume: 615 KiB of 5 GiB, 2 KiB of 16 MiB).
- [x] Store the actual `DATA`, `META` and `INFO` boundaries in the header.
- [x] Validate the layout when mounting: the sections have to fill the
      volume, `INFO` has to end at its end, and the root has to be a
      directory.
- [x] Store the moments of a record and read them through `statx`: the
      record of version 2 is 32 bytes, a version 1 volume keeps its 16
      bytes and mounts without a conversion (ADR 0013).
- [ ] An access time: a read would have to rewrite the record, and the
      system does not spend a write on a read yet.
- [ ] A journal, or a checksum of the content of a file: a record of the
      format has a checksum since ADR 0038, the bytes of a file do not, and a
      record written twice is still one copy in its place (`docs/debts.md`).

## 8. The device under the volume

MicroFS does not talk to the flash: it asks a block device
(`kernel/blockdev.h`) for a sector of 1 KB, and the device is a partition of
the flash on the board or a file in the simulator. The port of the ESP32
keeps a cache of the **erase sector** of the flash, 4 KB, because NOR flash
can only be programmed after an erase and the filesystem has no notion of
one: the writes collect in the cache and `sync` erases and programs the
whole sector.

Two consequences a person should know before trusting the volume with the
only copy of something:

* the unit of loss of a power cut is 4 KB, not 1 KB. The map of the free
  blocks and the block of the root inode live in the same erase sector - the
  map sits at the top of `META` next to `INFO` and the table below it in a
  version 3 volume, and the record 1 is the last slot of the top block of the
  table - so a cut during `sync` can take them together — the volume then
  mounts (its header is in the INFO section, apart) and every lookup fails.
  A record of version 3 has a checksum since ADR 0038, so a half-written
  record is named by `fsck`; the map has none, and a map that a cut left half
  written is what `fsck -y` rebuilds from the records;
* an interrupted `mkfs -y` on a volume that had a header leaves a volume
  that mounts with an empty table of the inodes: the header is written
  last, but it is also the old one that survives.

The mount refuses a volume whose header is damaged or whose geometry does
not match the device, and it keeps the data: it formats only a medium whose
header block *and* first block are blank, so a partition that grew or moved
is not mistaken for a fresh one.
