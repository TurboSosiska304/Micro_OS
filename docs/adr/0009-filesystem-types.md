# ADR 0009 - Types of filesystems, and FAT as the second one

* Status: accepted
* Date: 2026-11-21
* Updated: 2026-11-21 — the reader became read-write: create, write,
  truncate, append, unlink, mkdir, long names that are made, `statfs`, and a
  read-only check of the volume (`fsck` of FAT), and a repair of the
  damage the medium itself explains under `-y` (lost clusters, a loop, a
  cross-link, a size against its chain).
* Updated: 2026-11-21 — the same `fsck -y` repairs MicroFS: the orphan
  blocks, the entries and the names nobody can reach, the chains, the sizes,
  the counter of a directory and the records no name reaches, with the rules
  in `mfs/mfs.h` and the note in ADR 0013 (a damaged header, the data of a
  half-written file and a shared directory stay for a person).
* Updated: 2026-11-21 — ext2 arrived as the next reader: read-only, with
  inodes, modes, owners and the moments of `statx` (ADR 0019). The paragraph
  below about it being only a name is the history of this decision.

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
The kernel must read it, and it must not corrupt it. The first reader that
writes is how a volume that another system owns is lost, so the reader came
first and the writer came after it had its tests.

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
| `MICRO_OS_FS_FAT32` | ON (tests and simulator) | `n` by default | OFF by default | not linked |
| `MICRO_OS_FS_EXT2` | OFF, not written | - | - | not linked |

`MICRO_OS_FS_MFS` cannot be turned off in the host build: the simulator and
the tests mount MicroFS at `/`, and a host image without it has no `/etc`
and no program. The option exists so that the matrix is written down and a
firmware build can say what it contains.

The FAT filesystem is a component of its own (`components/fs_fat32/`). On the
board it is registered only when `CONFIG_MICRO_OS_FS_FAT32=y`; when the
option is off, the component registers no sources, so no object of it
reaches the firmware. The x86-64 project has the same choice as a
CMake option, off for `make x86-smoke` and `make x86-run`, because the disk
of that machine is one MicroFS volume.

The host build turns the option on: the simulator and the tests link the
component as a library. A build that turns it off (`make test-minimal`)
compiles the four source files into the test binary directly, so the reader
stays covered and a filesystem without a test is not left in the tree
(`AGENTS.md`). What the option controls is what an *image* links, not what
the tests can reach.

### 3. What FAT means in this kernel

The component speaks FAT12, FAT16 and FAT32. The type follows the boot sector:
a zero `FATSize16` with a non-zero `FATSize32` is FAT32; otherwise the count
of the clusters decides between 12 and 16, as the specification does.

**Read and write.** The component speaks the whole life of a file: `open`
with `O_CREAT`, `O_TRUNC` and `O_APPEND`, `write`, `close`, `unlink` and
`mkdir`; `read`, `list`, `readdir` and `stat` are the reader it always was.
`chmod` and `chown` are `KERR_NOTSUP`, and `write` on a descriptor that was
opened for reading is `KERR_BADF`: the operation exists, this descriptor was
not opened for it. `unlink` removes a file or an empty directory; a directory
that still holds an entry is `KERR_NOTEMPTY` (the VFS has no `rmdir` of its
own yet). `rename` is not in the operations of the VFS and is absent here.

A write either fits whole or does not touch the file. The clusters of the new
length are taken from the free map before the first byte goes down, and a
volume that runs out in the middle of that says `KERR_NOSPC` and gives back
what it had just taken. A file stops below 4 GiB, the limit of the size field
of an entry.

Every change reaches the medium before the call returns, and the device is
asked to write through its own cache (`ops->sync`) when it has one. There is
no journal and no cache of the changes: a power cut in the middle of a write
leaves a cluster that no chain names or a directory entry that is half
written. `fsck fat` finds both; `fsck fat -y` gives the first back to the
free map and cuts or removes the second where the medium still says what it
meant, and the rules are below.

**No modes, no owner.** `stat` answers `mode = 0` and `owner = 0`, the same
way procfs and devfs answer for an object that has no such attribute. A zero
mode is not "nobody may touch it": the VFS already reads a zero as "this
filesystem keeps no modes" for the search bit of a directory and for
`open`/`execve`. `chmod` and `chown` are `KERR_NOTSUP`; the shell prints
`(not set)` where it prints a mode.

**No time.** A FAT entry has a date and a time of its change, and this
component ignores both. `struct vfs_stat` has no field for them yet, and
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

A name that is created is written the other way round: the 8.3 name is
generated from it (upper case, the characters FAT allows, `~N` when another
entry already has that short name) and a run of long-name entries is put in
front of the short one when the name is not already a plain 8.3 name. The
checksum of the short name goes into every long entry, as the specification
requires; a reader of this kernel or of Windows that finds the run broken
falls back to the short name, which is why the checksum is not optional.
Names are decoded and encoded as UTF-8, so a long name of the board can hold
any language the terminal writes.

**The geometry.** It accepts 512, 1024, 2048 and 4096 bytes per FAT
sector and a cluster up to 64 KiB. It refuses a volume bigger than the
device, a table that does not cover the clusters, a root cluster that is not
one, and a chain that is longer than the volume (a loop). The FAT12/16 root
is the fixed region of the boot sector; every other directory is a chain.
The block device may hand out sectors of 1 KiB while the volume speaks 512
bytes: the component places every read and every write in the sector of the
device and caches the one it touched last.

The volume is never scanned for a second one: one FAT volume at a time, the
same way MicroFS has one. A second card is a second mount of the same
component in a later version; the static slot holds one volume.

**The table and the free map.** A cluster is allocated by finding the first
zero entry of the table, marking it as the end of a chain and zeroing the
cluster. The 8.3 entry of the file names the first cluster only when the
file has a byte in it: an empty file has cluster 0 and no space at all. The
chain is extended before a write, never during it. Both copies of the table
of a volume with two are written on every change; a volume whose copies
disagree is a volume another system will complain about. The count of the
free clusters for `statfs` is a walk of the table, the price of having no
bitmap in the format.

**The check and the repair.** `fat_check()` walks the volume and reports; it
does not write, and `fsck fat <point|device>` is its command. `fat_repair()`
is the same walk with the safe writes in front of the check;
`fsck fat <point|device> -y` asks for it. The walk covers the boot sector and
the BPB (the bytes on the medium have to describe the volume that is
mounted), every copy of the table against the first, the chains (a loop, a
cross-link, a cluster outside the volume), the entries of the fixed root and
of every subdirectory, the short names, the checksum and the order of the
long-name runs, and the clusters that the table marks used while no chain
names them. A volume that is mounted is checked under its own lock; a device
that is not is mounted for the length of the walk and forgotten again. The
clusters that the walk has claimed live in one static map of 65536 bits, so
FAT12 and FAT16 fit whole and a bigger FAT32 volume is refused with a
message instead of being walked with a map that would hide a cross-link.

The repair decides nothing the medium does not say:

* a cluster that is marked used and that no chain names goes back to the
  free map, in every copy of the table;
* a chain that loops is ended at the first time the repeated cluster was
  named, a link that points at a free, retired or out-of-volume cluster is
  ended before it, and the clusters of the cut tail go back to the free map
  the same way;
* the second of two names of one cluster is cut before the shared cluster,
  so the name that came first keeps its data whole; when the *head* is the
  shared cluster the later file gives its claim up and becomes empty (only
  its name is left), and a directory in that position is not touched at all,
  because its subtree is not the checker's to throw away;
* a size that promises more than its chain holds is cut down to the chain,
  and a chain that reaches past its file gives the tail back;
* a run of long-name entries that does not follow the sequence and checksum
  rules is removed whole and the 8.3 name stays; a run in front of an entry
  that is removed goes with the entry;
* an entry whose first cluster is not in the volume is removed (the marker
  `E5`), with its long name, because nothing on the medium says where its
  data is;
* a file whose 8.3 name holds a character FAT does not allow is removed
  with its long name; a directory with such a name keeps it, because its
  subtree would go with the entry, and the report names it as left.

It refuses to guess, and the report leaves the problem as it is:

* a boot sector that disagrees with the mount (the writes would use a
  geometry the medium no longer has);
* two copies of the table that differ (the medium does not say which one is
  the volume);
* an empty file that names a cluster: the chain could be the one of a
  directory the walk has not entered yet, so it is named and left;
* a directory whose head cluster another chain holds (see above);
* a medium that does not answer: the pass stops at the first failed write.

A volume that carries one of the first two is not repaired at all, not even
the lost clusters: the two stories have to agree before the kernel writes.
MicroFS follows the same rule: `mfs_repair()` writes nothing when the header
is damaged or disagrees with the mount, and it gives nothing back when the
tree could not be walked whole.
The same caution covers a tree the pass could not walk whole: when a
directory head is shared or a tree is deeper than a path can name, the free
map is not touched either, because a cluster of the subtree it refused to
enter looks exactly like a lost one.

`fat_repair()` runs the check once more after the pass with a map of its own,
so a repaired volume is proven whole rather than assumed whole: `errors` is
what is left, `unfixed` counts it, `fixed` counts the changes, and
`lost_found`/`lost_freed` count the clusters the free map took back. Without
`write` the call is exactly the check it was.

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
* **a journal or a cache of the changes**: every write goes to the medium
  before the call returns. The price is the wear of the card and a volume
  that a power cut can leave inconsistent; the alternative is a third copy
  of the data to replay, which this kernel does not have the memory for.
* **`rename`**: the VFS has no such call. When it gets one, FAT can move an
  entry between two directories of the same volume; across volumes is a copy.
* **a file that is removed while a descriptor is open**: FAT has no inode to
  keep alive. A write through the old descriptor finds that the entry no
  longer names the file and is refused; the caller is expected not to use a
  descriptor after the name is gone.
* **timestamps**: see above; the field of `struct vfs_stat` waits for a
  decision of its own. The date and time fields of a directory entry are
  written as zero, which a reader shows as an unknown time.
* **exFAT**, FAT64, the FAT32 extended flags and the backup boot sector:
  a volume that needs them is not one of the volumes this reader promises.
* **repair beyond what the medium explains**: `fat_repair()` fixes the lost
  clusters, the broken chains, the sizes and the names, but it does not
  choose between two table copies that differ, does not rewrite a boot
  sector, and does not touch a directory whose head another chain holds. A
  volume whose damage is one of those needs a person and, for the boot
  sector, another formatter.
* **a second FAT volume, hotplug and eject**: one static volume, like
  MicroFS. A card that comes and goes is a driver question first.
* **Unicode case folding and normalization**: the lookup is ASCII, the
  bytes of a name are otherwise compared as they are.
* **a bitmap of the free clusters**: `statfs` walks the table, and that is
  the whole cost of not keeping a second copy of the free map.

## Consequences

What it bought: the VFS has a type registry instead of a filesystem, an
image can contain one reader or two, and the second one is the volume of the
world outside the board. The tests build FAT12, FAT16 and FAT32 images in
the memory and mount them next to MicroFS, which proves the registry, the
path resolution below a mount point, the long names, the fixed root of the
old cards and the write path: a file is created, rewritten, appended,
removed, a directory is made, the free map goes down and comes back, and a
volume that is mounted again still has the bytes. The check is proven the
same way: a clean image of each of the three formats passes whole, and an
image with a table copy that differs, a loop, a cross-link, a cluster
outside the volume, a broken long name, a lost cluster or a chain too short
for its file is named and left untouched. The repair has its own images: one
kind of damage each is repaired, the check then reports zero problems and
the bytes of a read-only call are compared around it, and the damage the
pass refuses (a table copy that differs, a medium that does not write) is
still named and still there.

The price: the component is a second user of `kernel/blockdev.h` with its own
sector cache (the block layer still has no cache of its own); a FAT volume
cannot express a mode, an owner or a time, so a program that asks for them
is told "not supported" and not "here is a default"; a write has no journal
under it, so the volume is as safe as the medium and the moment, and the
repair can put back only what the medium still explains; and the mount of a
second volume is still a call of the boot, not a file of `/etc`.

Next, in an order that makes sense: the `mount` command and `/etc/fstab`
(they need the registry and a parser of the configuration), the timestamps
of `struct vfs_stat` and the writers that fill them, a journal or a cache
under a FAT write, and ext2 as the second reader with a format the project
can also write.

## References

* ADR 0005 - programs and protection: the mode and the owner of an object.
* ADR 0008 - block devices, transports and partitions.
* `docs/memory.md` - the format of MicroFS and the mode stored in a record.
* `components/fs_fat32/` - the filesystem; `tests/cases/test_fs_fat.c` -
  the images and the properties.
