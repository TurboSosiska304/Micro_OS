# ADR 0027 - The root of the machine is the filesystem it finds

* Status: accepted
* Date: 2026-11-23
* Related: ADR 0008 (block devices), ADR 0009 (filesystem types), ADR 0019
  (ext2), ADR 0022 (the layout of 1.1), `docs/plan-1.4.md` (M2)

## Context

The root of a machine is, today, the first partition of the MBR of the first
block device, mounted as **MicroFS**. The port knows the type because the
kernel mounts with the type it was built with: `mfs_vfs_mount.c` asks for
`"mfs"` and a volume of another filesystem is a volume that does not mount.
The other types are there and are tested - `fat32` reads and writes, `ext2`
reads (ADR 0019) - and the shell mounts them by hand:

```text
microos> mount /data fat32 /dev/ata0p2
```

A person with a FAT32 or an ext2 volume cannot boot it, and the machine of
1.3 has no way to say why: `mount` of the root is a call of the port, not a
decision of the kernel.

The volume of MicroFS has a ceiling of 16 MiB, and the ceiling is the
bitmap of the free blocks kept in the memory of the kernel (ADR 0022, and
the debts of K6): a filesystem of 5 GiB is a filesystem with 640 KiB of
bitmap resident, which the board cannot pay and the x86-64 machine can. A
root that may be ext2 has no such ceiling: ext2 keeps the bitmaps of its
groups on the medium and reads the group it needs.

## Decision

### 1. The type is read, not built in

The mount of the root asks the medium. The order of the questions is:

1. the partition type of the MBR (`0x83` Linux, `0x0B`/`0x0C` FAT32,
   `0xEF` EFI, and the rest of the table of ADR 0008);
2. the magic of the superblock of the partition - `MFS1` at the header,
   `0xEF53` of ext2 at 1024 + 56, `FAT32` in the BPB - because a partition
   type is a hint a person may have written by hand;
3. the list of the registered types of `vfs`: a type that says "yes" to the
   bytes wins, and the first one that says so is the root.

The kernel calls `vfs_mount("/", device, type)` with the type it found, and
the type name reaches the boot line (`the root is ext2 on ata0p1`) and
`mount` with no argument. A partition whose type no registered filesystem
recognises is refused with a sentence that names the partition and the
bytes - not with a kernel panic and not with a root that is silently empty.

### 2. The port passes the device, not the type

`k_port_root_device()` returns the device and the index of the partition;
the type is the kernel's business. A port that has no partition table (the
board, whose root is the `progs` partition of the flash) keeps answering
with MicroFS and says so in the boot line. The board does not change.

### 3. The release carries the same tree twice

The tar of the x86-64 machine carries `x86-all.img` (MicroFS, the default)
and `x86-ext2.img` (the same `/bin` and `/etc` written into an ext2 volume
of 64 MiB, and an option of `tools/x86_image.py` writes 5 GiB when asked).
`run.sh` boots the first unless `X86_ROOT=ext2` names the second, and
`make x86-disk` builds both. The smoke boots both and checks the same
commands through each; a machine whose root is ext2 boots to the same shell
as one whose root is MicroFS.

### 4. The other filesystems stay mount points

A mounted volume is a point of the tree (`/data`), of any registered type,
on any device whose driver exists (`ata0p2`, `boot0p1`, and in time a
stick). `mount` alone prints the type of every mount, the root included.

## Consequences

**Good.** A person may format a volume with `mkfs.ext2` on another machine,
put a program in `/bin` and boot it. The volume of a machine is no longer
bounded by 16 MiB: ext2 is 2 TiB with 4 KiB blocks and its bitmaps live on
the medium. The information the machine prints about itself is true, because
the type is what the bytes say.

**Bad.** Three implementations of "is this volume mine" must agree, and the
order of the questions is a contract of its own. A damaged superblock may be
recognised by two types at once: the first of the registered list wins, and
the list is the order of `vfs_register` - a fact a person has to be able to
read (`abi`, the shell's `modules`, this ADR).

**Cost.** ext2 must be writable for a root that a person uses: a root is a
`/tmp`, a `/var`, a log. That is M2.2 of `docs/plan-1.4.md`, and until it
lands the ext2 root is a root for reading - which is enough for an ISO and
for a test, and not enough for a machine.

## What this ADR does not do

* It does not add a partition table type: GPT is a later ADR, and the MBR of
  ADR 0008 is what the release writes.
* It does not boot from a network or an ISO: the ISO mounts its `boot0p1`
  read-only and the root is that, which is the same decision with a device
  that refuses writes.
* It does not make a filesystem writable: ext2 write is M2.2, FAT32 is M2.4.
* It does not promise a root above 16 MiB before M2.1: the ceiling of
  MicroFS is the bitmap, and the bitmap is the next step of the plan.
