# MicroFS (MFS) filesystem

> Russian version: [memory_ru.md](memory_ru.md)

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

## 2. Relative addressing

MFS uses relative addressing: section addresses are not stored as fixed values but computed from the drive capacity. To make this possible, each section is described by three parameters:

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

> **Advantage:** for a volume of any capacity there is no need to store or hard-code the region addresses separately — the whole layout is computed on the fly, for example when a new drive is attached or an existing one is resized.

## 3. Example for different sizes

For now the following rules apply:

* a block is 1024 bytes;
* `INFO` is 1 block;
* `META` is 4% of the total block count, rounded up;
* `DATA` takes all the remaining space.

| Volume size | Total blocks | DATA | META | INFO |
|---|---|---|---|---|
| 64 KiB | 64 | 60 | 3 | 1 |
| 256 KiB | 256 | 244 | 11 | 1 |
| 1 MiB | 1024 | 982 | 41 | 1 |
| 6 MiB | 6144 | 5897 | 246 | 1 |
| 16 MiB | 16384 | 15727 | 656 | 1 |

`DATA` and `META` scale automatically, but a percentage split has a catch: on a small drive 4% for `META` may not be enough for a large number of small files, while on a large one it will be more than needed.

## 4. Sizing META by record count

A better approach is to size `META` not as a percentage of the volume but by the expected number of objects:

```math
META_table = ceil(E * 16 / 1024)
```

where `E` is the maximum number of records (files and directories) and 16 is the size of one table record in bytes. This approach is not implemented yet (see section 6).

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

## 6. Possible improvements

Automatic volume layout:

- [ ] Determine the `META` size automatically at format time.
- [ ] Compute the `FILE TABLE` capacity from the available space.
- [ ] Account for service structures and the free-block bitmap in the calculation.
- [ ] Store the actual `DATA`, `META` and `INFO` boundaries in the filesystem header.
- [ ] Validate the layout when mounting the volume.
