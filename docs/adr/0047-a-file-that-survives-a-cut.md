# ADR 0047 - a file that survives a cut is whole or is not there

* Status: accepted, implemented
* Date: 2026-11-24
* Related: ADR 0009 (the format of a volume and its check), ADR 0013 (the
  moments of a change), ADR 0024 (the names of a program), ADR 0038 (a table
  of records: the format of MicroFS), ADR 0041 (a disk nobody formatted),
  `docs/debts.md` (the journal of MicroFS), `docs/plan-1.8.md`

## Context

ADR 0038 gave every record of a version 3 volume a CRC32 of itself, and that
closed one half of the debt: a record that a power cut left half written is
told from a whole one, and `fsck` reads it as a free record instead of
believing its fields. What the checksum did not give is the other half, and
the ADR said so in as many words:

> The record of an object is written in place, so a record that a power cut
> tore is the *whole* object's record lost, not its previous version - there
> is no journal and no second copy.

The shape of the failure that matters is not a torn record. It is the
**file**: the boot of the board writes `/etc/setting`, a person writes a file
over the console, a download of `pkg` lands in `/usr/bin`, and the battery
dies somewhere between the first byte and the close. MicroFS writes the data
of a file block by block into its final place, patches the links of the
chain, takes blocks in the map and writes the record of the object last. A
cut between any two of those writes leaves a prefix of the new file with the
record of the old one, blocks that the map says are used and no record
mentions, or a record that names a chain the medium never finished. `fsck`
puts the volume back into a state that mounts and walks - that is what it
promises - but the file a person was writing is a mixture of two contents,
and the file it replaced is gone.

The gate of this release says the promise has to be stronger: **a file is
either whole or it is not there**. The volume may forget the write, it may
not present a half of it.

## Decision

### 1. The version: 4, and the old volume is read as it was

A version 4 volume is a version 3 volume with a **journal** in `META`:

```
version 3:                        version 4:

+--------+----------+             +--------+---------+----------+
| table  |   map    |             | table  | journal |   map    |
+--------+----------+             +--------+---------+----------+
meta_start        INFO           meta_start        INFO
```

The map stays anchored at the top of `META` next to `INFO`, the table keeps
growing downwards, and the journal is a fixed run of blocks between them.
The layout of the table, of a record, of a directory entry and of the three
sections of the volume is unchanged, so a version 3 volume mounts and is
written exactly as it was; the mount accepts 1, 2, 3 and 4, and nothing of a
version 1, 2 or 3 volume is rewritten to make it a 4. The header gained no
bytes: the three words that were `reserved` are now `journal_start`,
`journal_blocks` and `journal_slots`, zero in every older volume. The
checksum of a version 4 header covers them like the rest of the header.

`mfs_format()` still writes version 3; `mfs_format_ex(dev, records, flags)`
writes version 4 when `MFS_FORMAT_JOURNAL` is asked for, and the builder of
the image of the release is the caller that asks. A tool that does not know
about the journal therefore makes the volume it always made.

### 2. The journal: a header, two blocks per slot, a commit block

```
block 0          the header of the journal
block 1 + 2i     the header of slot i: home block, sequence, checksum
block 2 + 2i     the image of the block of slot i, as it must be
```

```c
struct mfs_journal_header {
    k_u32_t magic;     /* "MFSJ" */
    k_u32_t version;   /* 1 */
    k_u32_t sequence;  /* the number of the transaction */
    k_u32_t state;     /* 0 empty, 1 committed */
    k_u32_t slots;     /* the slots of the transaction */
    k_u32_t map_crc;   /* CRC32 of the slot headers, in order */
    k_u32_t crc;       /* CRC32 of the words above */
};

struct mfs_journal_slot {
    k_u32_t home;      /* the block of the volume the image belongs to */
    k_u32_t sequence;  /* the transaction the slot belongs to */
    k_u32_t crc;       /* CRC32 of the 1024 bytes of the image */
    k_u32_t reserved;
};
```

The number of slots is a property of the volume and is set by the format:

```
slots = bitmap_blocks + 16
```

A transaction can change every block of the map of the free blocks (a write
of a large file allocates blocks all over it) and a handful of other blocks:
one record of an object, the four blocks of a directory, the header of the
volume. Sixteen is the reserve for those, and a transaction that needs more
- no operation of the filesystem does - is committed in parts (see §4).

### 3. The order of a transaction

A transaction is written ahead of the blocks it changes, in this order:

1. the header is written with a new `sequence` and `state = 0` (active);
2. every block that will change is written into a slot: the slot header
   with the block it belongs to and its checksum, then the image;
3. the header is written again with `state = 1` (committed), the number of
   the slots and the checksum of their headers, and the device is flushed;
   **this write is the commit**;
4. the images are read back from the slots and written to the blocks they
   name, and the device is flushed;
5. the header is written with `state = 0` and no slots (the checkpoint).

The home blocks are **not touched before the commit**, so a cut in steps 1
or 2 leaves the volume exactly as it was: the recovery reads a header that
is not committed and forgets the transaction. A cut in step 4 leaves some
of the images applied and some not, and the recovery writes all of them
again: a transaction of this shape may be replayed any number of times and
the volume ends in the same state. Step 5 costs no flush, because the next
transaction writes the header before its own slots and its commit flushes
them; a header that is still "committed" at the next mount makes the
recovery write the same images once more, which is why the step can be
skipped by a crash without a cost.

The `sequence` and the checksum of the slot headers are what makes a
half-overwritten journal harmless: a header of transaction *n* whose slots
already belong to *n+1* does not match itself and is ignored, and the home
blocks are then the blocks the commit of *n* left, which is a whole volume.

### 4. What goes through the journal

Every block that is **metadata** is written through the transaction:

* a block of the map of the free blocks (only the blocks a change dirtied);
* a block of the table of the records (one record of an object);
* a block of a directory, and the link of the chain that a growing
  directory patches;
* the header of the volume (the growth of the table).

The **data of a file and the links of its chain are not journaled**: a
journal of the bytes would write the file twice, and the price of the
journal is the price of the metadata. What protects the data is §5: the
object is not published until its close, and the commit of the close is one
transaction.

An operation that changes several of these blocks - a create writes a
record, the parent directory and the map - is one transaction, and the
public call opens and commits it. A helper that is called outside an
operation (the repair pass, a table growth) commits each block on its own,
which is the state the medium already had to survive before this ADR.

### 5. A file under a write is a draft

The record of an object has a new flag: `MFS_INODE_PENDING` (bit 2 of
`flags`, next to `MFS_INODE_USED` and `MFS_INODE_DIR`). It means "a program
opened this object for writing and has not closed it".

* `mfs_open()` with `O_CREAT` and a write marks the new record pending
  before it returns; `O_TRUNC` resets the record and marks it pending in
  the same write.
* The first `mfs_write()` or `mfs_truncate()` of an object that is not
  pending marks it pending, in a transaction of its own.
* `mfs_close()` writes the record again without the flag, with the size and
  the head of the chain the descriptor built, and the whole dirty map, in
  **one transaction**. That is the publication of the file.

A reader ignores the flag: a program that writes a file and reads it back
before the close sees what it wrote. What the flag changes is the recovery:

* a cut before the publication leaves the record of the object **pending**
  on the medium, and the mount removes it: the name goes from its parent,
  the record becomes free and the blocks go back to the map. The file is
  **not there**;
* a cut after the commit block of the close is replayed by the mount, and
  the file is **whole**: the record and the map are the two images of one
  committed transaction, and the bytes of the file reached the medium
  before the commit was flushed (the commit's flush covers every write that
  came before it);
* a cut between them is the first case.

So a file a program was writing is whole or absent, which is the promise.
Three consequences are conscious and are not hidden:

* a file that is **modified** - appended to, or rewritten without a
  truncate - is a draft too, and a cut in the middle of the modification
  removes the whole file instead of restoring the version before it. The
  journal knows how to restore metadata; restoring the *bytes* would need
  the old chain to be kept and every block of the write to be copied into a
  new place, which doubles the space a write needs and is not in this
  release (`docs/debts.md`). A program that wants the old version to
  survive a cut writes a second file and renames it, the way `pkg` installs
  a program (ADR 0024 §5);
* a record that a write tore while it was pending is not a record: the
  recovery frees the name through the directory (an entry whose record is
  pending is removed) and the map through the bit of the block, and the
  record itself is freed as an orphan;
* a **single block** of data that the medium tears is not detected: the
  format has no checksum of the content, and a whole block written whole is
  what the block layer and the drivers promise. A checksum per data block
  is a decision of the format and stays in the debts.

### 6. The repair of a volume that a cut left

The mount runs, in this order:

1. the header is read and validated (§1), and the **journal is replayed**
   (§3); the number of the blocks the replay wrote is logged;
2. the map and the root are loaded, and the volume is mounted;
3. the table is walked for a record that is **pending**; a volume without
   one pays the walk of the table, which is 20 blocks on a volume of any
   size (ADR 0038), and nothing more;
4. if a pending record is there, the **repair pass of the check runs**
   before the mount answers: the pending names are removed, the records no
   name reaches are freed and the map takes the blocks no record holds;
   every change is a journaled write, so the repair itself survives a second
   cut and is finished by the next mount;
5. the mount logs what it discarded and how many blocks it replayed.

The rule of the repair is the one it had (ADR 0038 §6) plus one line: a
record with `MFS_INODE_PENDING` is not reached by its name, so its entry is
removed like an entry that names a free record, and the pass of the orphans
and of the map does the rest. `mfs_check()` names a pending record as a
problem - "a write that a close did not publish" - so a person who checks a
volume the mount refused to repair still reads what is there. Because the
mount repairs before it answers, a plain `fsck` after a cut finds the
volume consistent and **no problem**, which is the gate.

### 7. What the journal does not replace: `fsync`

There is no `fsync()` of a file in the system calls of the release. A write
of a program reaches the medium when the filesystem syncs - at the close,
at a create, at a directory, and at the end of a write of the shell - and a
cut loses what the program wrote and did not close. That is the same
promise a Unix-like system makes without `fsync`, and the journal does not
change it: it makes the close atomic, not earlier. The call and the flag of
the descriptor are in `docs/debts.md`.

### 8. The numbers

The journal costs the metadata, not the data:

| | version 3 | version 4 |
|---|---|---|
| `META` of the 16 MiB volume of `make x86-mfs-smoke` | 22 blocks | 59 blocks |
| journal of that volume | - | 37 blocks (18 slots) |
| `META` of the 5 GiB volume of `make x86-volume-smoke` | 660 blocks | 1,973 blocks (1.9 MB) |
| journal of that volume | - | 1,313 blocks (656 slots) |
| `file_write` of 16 MiB, `make bench` | 5.18 MiB/s | 5.25 MiB/s |
| the same, seconds | 3.09 s | 3.05 s |

The journal of a 5 GiB volume is 1.9 MB, which is 0.04 % of it; the
version 3 `META` grew from 660 to 1,973 blocks because a transaction on that
volume may dirty any block of its 640-block map. The bytes of a write of
16 MiB add the map blocks the allocation dirtied (two of them), one record
and the directory, and two flushes - about 10 KB of a 16 MiB write. The two
`file_write` rows are the median of three runs of `make bench` on the same
machine, one over a volume of version 3 (`--no-journal`) and one of version
4: the difference is inside the 3 % spread of the row, so the price of the
journal is the metadata and not the data. The commits themselves are
counted by `make x86-mfs-smoke` and the gate of the cut is
`make cut-smoke` (20 cuts).

## Consequences

**Good**

* A file that a program was writing is either whole or not there: the gate
  of the release cuts the power at twenty points of a write and every time
  the mount, `fsck` and the file say the same thing.
* Every metadata operation is atomic: a create, a remove, a rename, a
  chmod and a close either happened or did not, and the mount of a cut
  volume needs no `fsck -y` to be consistent.
* A version 3 volume mounts and is written as before, and the tool that
  does not ask for a journal makes the volume it made in 1.8. The format of
  the release is one flag of `mkfs`.
* The journal costs the metadata and the map blocks that a change dirtied,
  not the data, so a file of any size pays the same few kilobytes.

**Bad**

* A file that is modified in place is a draft, and a cut removes it rather
  than restoring its previous content. The next release that wants the
  other answer needs copy-on-write of the chain, which doubles the space a
  write needs while it is in flight.
* The journal is `META`, so a volume formatted with a small table for a
  small tree and then filled with files grows the table into the data
  section as before, and the journal is sized by the map, which grows with
  the volume. A volume smaller than about 128 blocks cannot hold a journal
  and `mfs_format_ex` answers `MFS_ENOSPC`; the default format of version 3
  does not.
* The journal does not make `mfs_sync()` a durability point of the bytes a
  program wrote: the publication of the file is the close, and the call
  that would say "these bytes are on the medium now" is still
  `docs/debts.md`.
* ext2 has no journal in this release (`docs/debts.md`, M4 of the plan):
  the subset of ext3 that fits the writer of 1.8 is named there, and the
  gate of a cut runs on MicroFS only.
