# ADR 0041 - A disk this system did not format is read-only

* Status: accepted
* Date: 2026-11-24
* Related: ADR 0008 (block devices), ADR 0019 (ext2), ADR 0027 (the root is a
  fact of the medium), ADR 0029 (ext2 write), ADR 0032 (the address of a
  buffer a device reads), ADR 0037 (the second disk), ADR 0038 (a table of
  records), `docs/plan-1.8.md` (M2), `docs/debts.md`

## Context

Until this release the disks of the x86-64 port were the ones a test brings:
the raw image of `make x86-disk` on the IDE channel, the same image on a card
of virtio, or the volume in the memory of a machine without a disk. Every one
of them is an image this tree made: the kernel mounts it, writes its own
`/etc`, and the smoke tests count on that - a file written in one boot is read
by the next one.

A machine of a person is not that. The laptop that starts from a stick has a
disk of its own: a table of partitions written by an installer this kernel has
never heard of, a boot area, a volume of btrfs or ext4 or NTFS with the life of
somebody in it, and - most of the time - a volume of another kind in a
partition nobody labelled. The SATA controller of a chipset (M2 of the 1.8
plan) finds that disk as a matter of course, in every boot of every machine
that has one. A driver that treats it like the image of `make x86-disk` is not
a slow driver; it is a boot that can erase a life.

The tree is also *able* to write an alien volume: since ADR 0029 the ext2
writer is complete and `x86-root-smoke` writes a FAT32 volume of a second
partition. The ability is exactly why the rule has to exist before the driver
does. `docs/plan-1.8.md` puts it first: "**the rule that comes before the
code**: a disk this system did not format is read-only, and a partition the
table of the medium does not name is not touched at all."

## Decision

### 1. A disk is read-only until the bytes and the list say otherwise

The platform registers a disk it found, and the rule decides what may be
written to a volume of it. A volume is writable only when **both** are true:

1. its superblock carries the identifier of this system, and
2. the volume is named in `/etc/volumes` of the medium.

The identifier is the header block of MicroFS - the magic `0x4D465331` in the
last block of the volume, the same word the mount reads - and it is not a
guess about the bytes: it is the mark this system leaves when it formats a
volume. A volume of another system (ext2/ext4, btrfs, NTFS, XFS, FAT32) is not
this system's, whatever this kernel can read, and a medium that was never
written is nobody's.

`/etc/volumes` is the list `mkfs` writes and nothing else does: one device
name per line (`ahci0p1`), `#` starts a comment, whitespace around a name is
not part of it. It is read through the VFS at the moment of the decision - the
list of the mounted root, which for the case this lane is about, a machine
that boots the disk of a person, is the list of the medium itself. A file that
is not there names nothing, and "not named" is a refusal, not a prompt to make
one.

The rule is the kernel's, not a driver's: it is `components/kernel/volume.c`
behind `kernel/volume.h`, it reads the head of a device and the list, and it
writes nothing. Every controller that arrives asks it, and the sentence of a
refusal is the same on every controller.

```
k_volume_identify(dev, &id)   what the medium is: a volume of ours, one of
                              somebody else's, blank, or nothing recognised;
                              and where the table of its partitions ends
k_volume_may_write(dev, &id)  the two conditions above, with a sentence
k_volume_write_above_table()  the geometric half of the rule, below
```

### 2. A write is a decision of a mount, not a consequence of one

A controller registers its disk **read-only**. Mounting a volume does not
change that: the rule is asked again for the volume, and a mount that was not
asked for the write (`mount -w`, a lane of its own) leaves the disk as it
found it. The reason is the one a person would give - "I have not said that
this volume may be written" - and not "the driver does not support writes
yet", because the second sentence would become false without anybody deciding
anything.

This step of the lane implements the *refusal*: the AHCI driver answers a
write with the sentence of the rule, once in the log of the boot and the error
of the medium to the caller, and the write path of the release stays closed.
The decision above is complete and tested; opening the write path is the next
step of M2 (a writable root), and it will be a mount that asks.

### 3. The table of the partitions is never written

The first sectors of a disk are not a filesystem's: the MBR, the header of the
GPT and its array belong to the medium. The driver refuses a write whose
*sector* is below the first sector of the first partition of the table, and
the reader of the rule computes that boundary while it reads the table (the
first entry's start for an MBR, the first usable LBA of the GPT header). A
partition is a child device that adds the offset of its first sector to every
request, so a write through a partition has left the table behind before it
reaches the driver; the check protects the whole-disk device and the day a
filesystem is mounted on it by mistake.

There is no exception and no "repair": a table that contradicts itself is a
refusal (ADR 0008), and a repair belongs to a tool of the person who owns the
disk.

### 4. A foreign superblock is refused at the mount

`k_root_type()` reads the type of a volume out of its bytes (ADR 0027). A
volume no type of the image claims and the rule recognises as another
system's is refused with the name of that system:

```
root: ahci0p1 carries the volume of another system (btrfs),
      this system did not make it and does not mount it
```

The boot then stops with the sentence it has always had ("there is no root
filesystem to run from") instead of running a shell over somebody else's
bytes. An ext2 volume the image can read is still mountable as the root - the
port has booted one since ADR 0027 - but it is not *writable* by this rule,
and a root that is written needs the two conditions above.

### 5. The first boot of a new controller lists what it found

The first time a machine boots with a controller it did not have, the disk is
described and nothing is written:

```
ahci0: 6 port(s) of the controller, AHCI 1.0, 64-bit addressing
ahci0: 16 MB on the port 0 of 6
ahci0: ahci0p1: a volume of this system (mfs), read-only
ahci0: the disk is read-only: the volume is not named in /etc/volumes
x86-64: the root volume is ahci0p1 (1 partition(s))
ahci: 1 disk(s) on the controller, read-only, nothing written
```

The sentence is not a promise of the driver: it is the answer of the rule,
printed with the numbers the controller and the table answered.

### 6. The proof is a hash

A log line says what a boot meant to do. The proof that a disk was not touched
is the SHA-256 of the whole image before and after the boot, and it is the
gate of this lane (`make ahci-smoke`): the same in both boots of the tool - the
one that mounts a volume of this system and the one that is refused a volume
of btrfs - while the boot itself asks the disk for the three files of `/etc`
it would have written on a volume of its own.

## Alternatives

**Trust the partition type byte.** The MBR says `0x83` for Linux volumes; a
volume of ours is one whose entry says so, and the format does not have to
change. It is a hint a person can write by hand, the GPT has no such byte at
all, and it says nothing about who made the filesystem inside. The magic of
the superblock and a list written at the format are facts; the byte is not.

**Make every disk writable and let the filesystem refuse.** The write paths of
MicroFS and ext2 are complete; a mount of a foreign volume fails in the
driver. That is a refusal *after* the first sector was written - the
superblock of a btrfs volume is a valid place for a MicroFS header as far as a
bug in a probe can tell - and it leaves the inside of a person's disk at the
mercy of every defect of every write path. The rule says no before the first
byte moves.

**A read-only mount flag in the VFS, and nothing in the driver.** A mount
option protects the tree, not the medium: the shell and `mkfs` reach the
block device without a filesystem above them, and a second boot without the
flag would mount the same volume writable. The guard belongs where every write
goes through - the driver - and the mount is a second door.

**A list in the image instead of `/etc/volumes`.** The list has to travel with
the medium: a disk moved to another machine is still the one that said which
of its volumes may be written, and an image rebuilt for a new release must not
inherit a permission from the old one. `/etc/volumes` of the mounted root is
the only list that travels with the bytes it describes.

## Consequences

* The read-only refusal is a `-1` of `struct k_blockdev_ops::write`, which the
  filesystem above reports as the error of the medium (MicroFS: `MFS_EIO`,
  the syscall: `KIO`). The reason is the driver's line in the log, written
  once per boot; a program that retries does not bury the boot in sentences.
* The cache of the block layer above such a disk is **write-through**. A
  write-back cache would answer "yes" to a filesystem and refuse the sector at
  the next sync, which is the wrong order for a rule about writes.
* The volume of the release is read-only on the SATA disk until M2's writable
  root lands: `/etc` of the image is not rewritten, the three missing files of
  the settings are reported as refused, and the machine boots and runs its
  programs. A person who wants a writable root keeps the IDE or the virtio
  disk of the release, or waits for the mount that asks.
* `kernel/volume.c` is portable and host-tested (`tests/cases/test_volume.c`):
  every superblock the rule knows, the parser of `/etc/volumes`, the boundary
  of the table, and the promise that reading a medium never writes it.
* A controller that arrives later - NVMe is the second half of M2 - asks the
  same three calls and gets the same answer. Nothing of the rule is in the
  AHCI driver.

## What this ADR does not do

* it does not open the write path: `mount -w` and the write of a volume that
  the two conditions allow is the next step of M2;
* it does not write `/etc/volumes`: `mkfs` of a release names the volume it
  formatted, and the image tool of this tree does not yet;
* it does not repair, format or claim a foreign disk, and it does not give a
  person a command to lift the rule for one boot;
* it does not cover a volume that arrives after the boot (a hot-plugged card):
  the rule is asked at the boot and at a mount, and a device that appears later
  is a lane of its own.
