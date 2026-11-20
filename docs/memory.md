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

The rules of the current format:

* a block is 1024 bytes;
* `INFO` is the last erase sector of the volume: up to 4 blocks of 1 KB, so
  that the header is alone in its sector;
* `META` is 4% of the space below `INFO`, and grows when that cannot hold the
  bitmap of the free blocks and the table of the records; the mount asks for
  256 records (`MFS_MOUNT_RECORDS`);
* `DATA` takes all the remaining space.

The bitmap is one or two blocks even on the largest volume listed and the
table is four, so the growth decides only on a small volume; on a large one
the 4% is generous. For a volume formatted by the mount:

| Volume size | Total blocks | DATA | META | INFO |
|---|---|---|---|---|
| 64 KiB | 64 | 55 | 5 | 4 |
| 256 KiB | 256 | 241 | 11 | 4 |
| 1 MiB | 1024 | 979 | 41 | 4 |
| 6 MiB | 6144 | 5894 | 246 | 4 |
| 16 MiB | 16384 | 15724 | 656 | 4 |

`DATA` and `META` scale automatically, but a percentage split has a catch: on
a small drive 4% may not hold the table, and on a large one it is more space
than the table needs.

## 4. Sizing META by record count

The table of the records is sized by count, not by percentage:

```math
META_table = ceil(E * 16 / 1024)
```

where `E` is the maximum number of records (files and directories) and 16 is
the size of one record in bytes. The mount passes `E = 256`, so the table is
four blocks on every volume; the 4% is only the first guess of `META`, which
the format then grows until the bitmap and the table fit. A volume cannot ask
for more records after it was formatted, and the capacity does not follow the
size of the volume yet (section 7).

## 5. Data stored in INFO

Instead of fixed addresses, `INFO` stores a universal description of the regions. The header additionally holds `magic`, `version`, `block_size`, `total_blocks`, `root_inode` and `features`.

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

## 6. The flags of an inode

A record of MicroFS is 16 bytes and has no field of its own for the mode and
the owner of an object: the two numbers are packed into the upper bits of its
`flags` word, while the two low bits keep the type they always had. The volume
of 0.3.x therefore mounts unchanged (ADR 0005).

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
  `0755` for a directory, owner 0, which is the one user of the system.
* **The numbers cross the VFS as they are.** `struct vfs_stat` carries `mode`
  and `owner`, and `vfs_chmod()`/`vfs_chown()` are operations a filesystem may
  have or not; a virtual filesystem without them answers "not permitted"
  rather than storing a number that nothing would read.

## 7. Possible improvements

The state of the automatic layout:

- [x] Determine the `META` size at format time: 4% is the first guess, grown
      when the bitmap and the table do not fit.
- [ ] Compute the `FILE TABLE` capacity from the available space: the mount
      asks for 256 records, whatever the volume.
- [x] Account for the free-block bitmap in the calculation (one or two blocks
      of the section).
- [x] Store the actual `DATA`, `META` and `INFO` boundaries in the header.
- [x] Validate the layout when mounting: the sections have to fill the
      volume, `INFO` has to end at its end, and the root has to be a
      directory.

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

* the unit of loss of a power cut is 4 KB, not 1 KB. The bitmap of the free
  blocks and the root inode live in the same erase sector, so a cut during
  `sync` can take them together — the volume then mounts (its header is in
  the INFO section, apart) and every lookup fails;
* an interrupted `mkfs -y` on a volume that had a header leaves a volume
  that mounts with an empty table of the inodes: the header is written
  last, but it is also the old one that survives.

The mount refuses a volume whose header is damaged or whose geometry does
not match the device, and it keeps the data: it formats only a medium whose
header block *and* first block are blank, so a partition that grew or moved
is not mistaken for a fresh one.
