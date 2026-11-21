# ADR 0013 - The moments of a file, and the statx call

* Status: accepted
* Date: 2026-11-21

## Context

MicroFS kept no time. A record was 16 bytes and every bit of its `flags`
word already had a meaning: two bits of the type, nine of the mode and
fifteen of the owner (ADR 0005). The platform roadmap (K6) asked for the
timestamps of a file, and `struct vfs_stat` has no field for them. The two
questions were therefore one: how does a record that is full gain a moment,
and how does a program ask for it without breaking the `stat` that was
frozen in the ABI?

The clock was already a service of the kernel (`kernel/clock.h`): the RTC
driver of the board registers a setter, NTP and `date set` move the moment
through it. What did not exist was a reader: the filesystem needs "now",
not "set to".

## Decision

### 1. The format: version 2, a record of 32 bytes

A record of MicroFS is now

| Bytes | Field |
|---|---|
| 0-3 | `flags`: type, mode and owner, as before |
| 4-7 | `first_block` |
| 8-11 | `size` |
| 12-15 | `entries` |
| 16-23 | `mtime`: the last change of the content |
| 24-31 | `ctime`: the last change of the record |

The two moments are the seconds of Unix in 64 bits, so the year 2038 is not
a wall of this format. There is no `atime`: a read of this system does not
rewrite a record, and a field that is always zero would promise a moment
that nothing keeps. A later format that wants one takes a new field and a
version of its own; the ABI below has room for it when it comes.

The version in the superblock says which record the volume holds:

| `version` | Record | `features` | Read by |
|---|---|---|---|
| 1 | 16 bytes, no time | `MFS_FEATURE_CRC` only | this system and the one before it |
| 2 | 32 bytes, `mtime` and `ctime` | `MFS_FEATURE_CRC \| MFS_FEATURE_TIME` | this system |

A version 2 volume is refused by a kernel that only knew version 1, which
is the point of the version: an old reader must not read the second half of
a record as the first half of the next one. A version 1 volume is mounted
by this kernel exactly as it was: the record is decoded into the 32-byte
structure of the memory, `mtime` and `ctime` are zero, and a write of a
record writes the 16 bytes the volume has and nothing after them.

Zero is "this volume knows no time", not the first second of 1970. It is
the same rule the mode already follows (a zero mode means "the record
predates the modes"), and it is why a caller can tell an old object from a
new one without a second flag.

### 2. The migration: none is needed, and none is done

An old volume is **not reformatted, converted or rewritten at the mount**.
Its data, its directories and its records stay the bytes they were, and the
new fields simply do not exist for it: the reader of version 1 leaves the
two moments at zero. A write to an old volume keeps the record 16 bytes
wide, so the time of that write is lost; a format is the only way to give
the volume the room.

An in-place conversion was considered and rejected. The table of the inodes
is a fixed region of `META`; doubling a record either overlaps the data
section or needs a second table and a walk of every chain, and a power cut
in the middle of that walk leaves a volume with two half-tables. The format
is cheap and the data can be copied off first; a silent move of a user's
volume is not.

The 4% first guess of `META` still has to hold the table, now twice as
large in bytes for the same number of records. The formatter grows `META`
until the bitmap and the table fit, so a new volume of 64 KiB or of 6 MiB
is laid out correctly; `mkfs -y` is the only operation that changes the
layout of an existing volume.

### 3. When the moments move

| Operation | `mtime` | `ctime` |
|---|---|---|
| create a file or a directory | now | now |
| write, the close of a dirty file | now | now |
| open with `O_TRUNC` | now | now |
| add or remove an entry of a directory | now | now |
| `chmod` or `chown` | unchanged | now |
| `stat`, `statx`, `read`, `open` without `O_TRUNC` | unchanged | unchanged |

The write stamps at the close because that is when the record of the file
(size, first block) reaches the medium; a stamp at every `write` would
rewrite the record of the inode for every buffer and would make a stat in
the middle of an open show a time for a size that is not on the volume yet.
The parent of an object is stamped when its entry is added or removed, so a
listing of a directory can tell that it changed.

The moment comes from `k_clock_now_unix()`, never from a library of the
caller and never from a number the caller passes. The kernel clock is the
moment of the last set (RTC, NTP, `date set`) plus the uptime since that
set; a machine that was never set - the host build, the x86-64 machine
without a battery - falls back to the seconds since its boot. That is not a
date, but it orders the changes, and a filesystem is not the place to
decide whether a machine has a battery. The header of `kernel/clock.h`
says so; `k_clock_present()` is still the answer to "can this number be
printed as a date?".

### 4. The ABI: `statx` is the next free number

`struct vfs_stat` and `KSYS_STAT` are frozen and unchanged. A program that
wants the moments calls the next free number:

```c
#define KSYS_STATX 26
#define KSYS_COUNT 27
```

and passes a `struct vfs_statx`:

```c
struct vfs_statx {
    k_u32_t inode;
    k_u8_t  type;
    k_u64_t size;
    k_u32_t mode;
    k_u32_t owner;
    k_u64_t mtime;
    k_u64_t ctime;
};
```

The first five fields are the five fields of `struct vfs_stat`, in the same
order and with the same meaning, so a caller of `stat` that needs the time
changes one word of its call and one structure. The two moments are the
last fields and cannot be confused with the numbers of the old contract.

The VFS has a new optional operation, `statx(ctx, path, out)`. A
filesystem that has it answers the moments; one that does not (procfs,
devfs, the FAT reader) answers through its `stat` and leaves the two
moments zero, which is the same "unknown" as an old MicroFS volume. The VFS
does not invent a time for a filesystem that keeps none.

The library of the system gains `microos_statx()` and `struct
microos_statx`, the same fields with the types of `<stdint.h>`; `stat` is
not added retroactively, because the programs of this tree did not have one
and a new name is cheaper than a second guess at the shape of the old one.

### 5. The checker

`mfs_check()` reads the version of the header and sizes its walk of the
table by the record of that version. It reports a header whose version and
`MFS_FEATURE_TIME` disagree, and it reads a version 1 volume with the
16-byte decoder, so an old volume is checked as an old volume and not as a
damaged new one. The `CRC` of the header covers the version and the
features; a record has no checksum of its own, the same as before, because
a checksum per record would take another four bytes from every object.

## Consequences

**Good**

* A file has a modification time and a change time, and `statx` exposes
  them without touching `stat` or the numbers 0-25.
* An old volume mounts, reads and is checked without a rewrite; the new
  fields add nothing to a volume that has no room for them.
* The clock is one service of the kernel: the RTC, NTP and the host process
  all feed the same number, and the filesystem does not link a platform.

**Bad**

* An old volume never gains a time without a format; that is the price of
  not converting a volume behind the back of its owner.
* `mtime` moves at the close of a write and not at the `write` itself, so a
  stat of a descriptor that is still open sees the record of the last close.
* There is no `atime`, so a tool that expects all three moments of POSIX
  gets two and a zero it must read as "unknown".
* The table of a new volume is twice as large for the same record count, so
  a small volume has fewer data blocks than it did.
