# ADR 0009 - Types of filesystems, and FAT as the second one

* Status: accepted
* Date: 2026-11-21

## Context

One filesystem was one hard-wired volume of the kernel. `mfs_vfs_mount()`
found a block device by name, formatted it when the medium was blank, mounted
the volume into the singleton of MicroFS and handed the VFS one table of
operations. Nothing in the VFS said what a filesystem *is*: the type was the
name of the adapter, and a second adapter would have been a second branch in
the kernel.

The block layer is ready for more than one consumer: a disk and its
partitions are devices of their own (ADR 0008), the parser of MBR and GPT is
portable, and the x86-64 machine already boots from a partition. The
platform roadmap (K6) asked for a second filesystem behind the VFS. The
question is not only "which one" but "what does the kernel promise about a
filesystem it did not format".

FAT is the answer to that question because it is the volume a person brings:
a card from a camera, a disk from a laptop, a stick from another machine.
The kernel must read it, and it must not be the second writer of it. The
first reader that writes is how a volume that another system owns is lost.

## Decision

### 1. The type registry

The VFS keeps a small table of types:

```c
typedef int (*vfs_mount_fn)(const char *point, const char *device);

struct vfs_fs_type {
    char name[VFS_TYPE_NAME_MAX + 1];
    vfs_mount_fn mount;
};

int vfs_register_type(const char *name, vfs_mount_fn mount);
int vfs_unregister_type(const char *name);
int vfs_mount_type(const char *point, const char *type, const char *device);
```

* A type is a **name** and **the call that mounts a volume of it**. The VFS
  does not know the strings `"mfs"` or `"fat32"`; an image that wants a type
  registers it, and an image that does not want it never links its code.
* The mount call mounts the volume itself by calling `vfs_mount(point, ops,
  ctx)`. The VFS then holds the operations and the context, exactly as it
  did for the singleton; the resolution of a path, the check of the search
  bit and the listing of the mount points are unchanged.
* The registry is copied, not referenced: the caller may hand a local string.
  A type that is registered again keeps its slot and takes the new call,
  which is what a port does after a re-initialisation. `VFS_MAX_TYPES` is
  four, the same order as the mount table; the number is static because the
  kernel does not allocate for a configuration that a build already knows.
* `vfs_mount()` stays a public call. It is the primitive the mount function
  of every type uses, and MicroFS keeps using it: the root of the board is
  still the volume `mfs_vfs_mount("/", "flash0")` mounts (ADR 0008), not a
  type lookup.
* `struct vfs_operations` gained an optional `umount(ctx)`. The VFS calls it
  before it forgets the mount, so a filesystem that keeps a volume in a
  static slot can release it. Nothing else changed in the operations: a
  filesystem may still leave any call empty.
* An unknown type is `KERR_INVAL`, not `KERR_NOENT`: the type is a part of
  the configuration of the image, and a boot that asks for one the image
  does not have has a broken configuration, not a broken medium.

A filesystem that is not on a block device (procfs, devfs) can stay with the
direct `vfs_mount()` call. A type is for the volumes of a medium, where the
name of the device and the name of the filesystem are two different facts.

### 2. The build flags

| Option | Host build | ESP32 (Kconfig) | x86-64 (CMake) | Null port |
|---|---|---|---|---|
| `MICRO_OS_FS_MFS` | ON | always (the root) | always (the root) | not linked |
| `MICRO_OS_FS_FAT32` | OFF, tests compile it | `n` by default | OFF by default | not linked |
| `MICRO_OS_FS_EXT2` | OFF, not written | - | - | not linked |

`MICRO_OS_FS_MFS` cannot be turned off in the host build: the simulator and
the tests mount MicroFS at `/`, and a host image without it has no `/etc`
and no program. The option exists so that the matrix is written down and a
firmware build can say what it contains.

The FAT reader is a component of its own (`components/fs_fat32/`). On the
board it is registered only when `CONFIG_MICRO_OS_FS_FAT32=y`; when the
option is off, the component registers no sources, so no object of the
reader reaches the firmware. The x86-64 project has the same choice as a
CMake option, off for `make x86-smoke` and `make x86-run`, because the disk
of that machine is one MicroFS volume.

The host tests always cover the reader: `tests/CMakeLists.txt` compiles the
three source files of the component into the test binary directly. A test
that depended on the firmware option would leave the reader untested in the
default build, and a filesystem without a test is unfinished work
(`AGENTS.md`). What the option controls is what an *image* links, not what
the tests can reach.

### 3. What FAT means in this kernel

The reader speaks FAT12, FAT16 and FAT32. The type follows the boot sector:
a zero `FATSize16` with a non-zero `FATSize32` is FAT32; otherwise the count
of the clusters decides between 12 and 16, as the specification does.

**Read-only.** No call of the component writes to the medium, not even to
"repair" a table. There is no `write`, no `create`, no `unlink`, no `mkdir`,
no `chmod` and no `chown` in its operations. A caller that asks for one gets
`KERR_NOTSUP` (`ENOTSUP`, 95 of POSIX), not `KERR_PERM`: the operation is
not forbidden by the mode of an object, the filesystem does not have it.
`open` refuses the write flags (`O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`,
`O_APPEND`) with the same code. A `write` on a descriptor that was opened
for reading is `KERR_NOTSUP` through the empty operation of the VFS.

**No modes, no owner.** `stat` answers `mode = 0` and `owner = 0`, the same
way procfs and devfs answer for an object that has no such attribute. A zero
mode is not "nobody may touch it": the VFS already reads a zero as "this
filesystem keeps no modes" for the search bit of a directory and for
`open`/`execve`. `chmod` and `chown` are `KERR_NOTSUP`; the shell prints
`(not set)` where it prints a mode.

**No time.** A FAT entry has a date and a time of its change, and this
reader ignores both. `struct vfs_stat` has no field for them yet, and
inventing one here would make the first filesystem with a real timestamp
a second change of the same contract. The bytes are on the medium and
the reader does not lose them; it does not report them either.

**Names.** A long name is a run of entries of 32 bytes that precedes the
8.3 entry, in UTF-16. The scanner translates it to UTF-8, validates the
sequence and the checksum against the short entry, and falls back to the 8.3
name when the run is broken (a crash of another writer, a removed entry).
Lookup is case-insensitive, and the comparison is ASCII: `README.TXT`,
`readme.txt` and `ReadMe.Txt` are one file, while a non-ASCII name is
compared byte for byte. Unicode case folding is not in this iteration.

A `struct vfs_dirent` name is `VFS_NAME_MAX` bytes; a longer long name is
cut at a UTF-8 character boundary in a listing. The *lookup* compares the
whole name, so a path of the tree still reaches a file whose listed name is
shorter. The path itself is bounded by `VFS_PATH_MAX`, as everywhere else.

**The geometry.** The reader accepts 512, 1024, 2048 and 4096 bytes per FAT
sector and a cluster up to 64 KiB. It refuses a volume bigger than the
device, a table that does not cover the clusters, a root cluster that is not
one, and a chain that is longer than the volume (a loop). The FAT12/16 root
is the fixed region of the boot sector; every other directory is a chain.
The block device may hand out sectors of 1 KiB while the volume speaks 512
bytes: the reader places every read in the sector of the device and caches
the one it last read.

The medium is never written and never scanned for a second volume: one FAT
volume at a time, the same way MicroFS has one. A second card is a second
mount of the same reader in a later version.

### 4. Several filesystems at one time

The VFS resolves a path by the longest mount point that is a whole
directory prefix, as it always did. A mount point does not have to exist in
the parent filesystem: `vfs_list()` adds the children of a directory from
the mount table, which is how `/proc` and `/dev` appear in the root. A FAT
volume mounted at `/mnt/fat` is therefore visible in `ls /mnt` even though
the MicroFS volume of `/` has no `fat` entry.

The operations of a filesystem receive the whole path of the tree, not the
part below the mount point, because procfs and devfs were written that way.
A filesystem mounted below the root strips its own point (the FAT adapter
compares the prefix and returns the rest), exactly as those two do. MicroFS
is mounted at `/`, so for it the two paths are the same.

The order of the mounts is the order of `vfs_mount()` calls: the root first,
then the others. Unmounting the root of a machine that runs from it is a
call the shell does not offer; the VFS itself does not forbid it, and the
port decides.

`/etc/fstab` and a `mount` command are **not** in this iteration. A boot
that wants a FAT volume calls `vfs_mount_type()` after it has registered the
type, and the image that contains the reader calls `fat_vfs_register_type()`
once at the boot. The root of MicroFS is mounted the way it was, without the
registry.

On x86-64 the reader can be linked (`-DMICRO_OS_FS_FAT32=ON`), but the boot
does not mount a FAT partition yet: the disk of the smoke test has one
MicroFS volume, and a second partition with its call of the boot is the next
step of that port. The tests are the place where the registry and the mount
of a second volume are proven today.

### 5. What is deliberately absent

* **ext2**: the next reader, and the first one with inodes, blocks of a
  variable size and a superblock that a checker can walk. `MICRO_OS_FS_EXT2`
  is a name and a configure error, not a component.
* **write support for FAT**: a journal, a cache of the changes and a
  decision about the owner of a volume are the price, and none of them is
  paid here.
* **timestamps**: see above; the field of `struct vfs_stat` waits for a
  decision of its own.
* **exFAT**, FAT64, the FAT32 extended flags and the backup boot sector:
  a volume that needs them is not one of the volumes this reader promises.
* **fsck of FAT**: the reader reports `KERR_IO` for a chain it cannot
  follow; it does not repair a volume and it does not walk the whole medium
  to count the lost clusters.
* **a second FAT volume, hotplug and eject**: one static volume, like
  MicroFS. A card that comes and goes is a driver question first.
* **Unicode case folding and normalization**: the lookup is ASCII, the
  bytes of a name are otherwise compared as they are.
* **`statfs` of FAT**: the count of the free clusters is a walk of the
  table, and a reader that only reads a file does not need it. The call is
  `KERR_NOTSUP` today.

## Consequences

What it bought: the VFS has a type registry instead of a filesystem, an
image can contain one reader or two, and the second one is the volume of the
world outside the board. The tests build FAT12, FAT16 and FAT32 images in
the memory and mount them next to MicroFS, which proves the registry, the
path resolution below a mount point, the long names, the fixed root of the
old cards and the read-only refusals without a card or a `mkfs.fat`.

The price: the reader is a second user of `kernel/blockdev.h` with its own
sector cache (the block layer still has no cache of its own); a FAT volume
cannot express a mode, an owner or a time, so a program that asks for them
is told "not supported" and not "here is a default"; and the mount of a
second volume is still a call of the boot, not a file of `/etc`.

Next, in an order that makes sense: the `mount` command and `/etc/fstab`
(they need the registry and a parser of the configuration), the timestamps
of `struct vfs_stat` and the writers that fill them, and ext2 as the second
reader with a format the project can also write.

## References

* ADR 0005 - programs and protection: the mode and the owner of an object.
* ADR 0008 - block devices, transports and partitions.
* `docs/memory.md` - the format of MicroFS and the mode stored in a record.
* `components/fs_fat32/` - the reader; `tests/cases/test_fs_fat.c` - the
  images and the properties.
