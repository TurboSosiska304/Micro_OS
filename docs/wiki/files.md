# Files

Micro OS has one filesystem of its own, **MicroFS**, two virtual
filesystems, a FAT12/16/32 volume it can both read and write, and an ext2
volume it reads and writes. The root `/` may be any of the three - MicroFS,
FAT or ext2 - and is MicroFS on the board; `/proc` and `/dev` are generated
by the kernel. Everything a person writes at the
console lands in MicroFS unless a FAT volume is mounted, and the
configuration of `/etc` has a second copy in the store of the port, which is
why a format does not have to lose it.

The layout of the volume, the sections and the flags of a record are described
in [`docs/memory.md`](../memory.md); this page is about using it.

## The tree after a boot

The simulator, right after a boot (the board adds `/log`; x86-64 has the same
tree and adds `/bin` with the programs of its disk image):

```text
microos> tree /
/
|-- etc/
|   |-- console
|   |-- display
|   |-- font
|   `-- init.d/
|-- proc/
|   |-- version
|   |-- uptime
|   |-- meminfo
|   |-- tasks
|   |-- modules
|   |-- self/
|   |-- 1/
|   `-- ...
`-- dev/
    |-- tty0
    `-- ...
```

| Path | What it is |
|---|---|
| `/` | MicroFS on the volume of the port |
| `/etc` | the configuration: a file name is the key and its content the value for a single setting, `key=value` lines for a system (`display`, `font`, `console`) |
| `/etc/passwd` | the accounts: `name:uid:gid:home:shell`, one line per person (ADR 0033) |
| `/etc/shadow` | the hash of a password: `name:$sha256$<salt>$<hex>`, `*` for an account without one; mode `0600` |
| `/etc/init.d` | the scripts of the init process, one command per line |
| `/etc/fstab` | the volumes the boot mounts after the root, when the file is there |
| `/etc/greeting.gz` | on x86-64: the gzip fixture of `/bin/zcat`, written by `tools/x86_image.py` with a fixed moment |
| `/etc/licenses/zlib` | the license of zlib, beside the program that carries the library |
| `/etc/licenses/sbase` | the MIT notice of sbase, beside the ten commands it was built into |
| `/proc` | the kernel: `version`, `uptime`, `meminfo`, `tasks`, `modules`, one directory per task |
| `/dev` | the devices: `tty0`, `null`, `input` and the ones the port adds |
| `/log` | on the board: `start.log`, the log of the boot |
| `/bin` | on x86-64: the programs of the disk image (`hello`, `crash`, `fork`, ...), written by `make x86-disk` and read by `execve`; no boot installs them |

`/bin/ls` is the `ls` of sbase: one name per line, `-l` for the mode, the
owner, the group, the size and the date, `-a` for the two names `.` and `..`,
`-R` for a tree and `--color` for a directory in blue and an executable in
green. A bare `ls` is that program on x86-64 (the shell looks in `PATH`
first); the simulator, whose port has no programs, has the command of the
shell that prints the names two spaces apart. `tree` draws the levels below a
path, and `stat` is the detailed view of one object.

## MicroFS

MicroFS is a consumer of blocks, not of a flash chip: the device under it is
described by `kernel/blockdev.h` and registered by the port. On the board it is
the `mfs` partition of the internal flash (`flash0`, 5.56 MiB); in the simulator
a file of the host (`file0`, 512 KB by default); on x86-64 the first partition
of the ATA disk (`ata0p1`) when one is attached, and a volume in the RAM
(`ram0`, 512 KB) when it is not.

A volume has three sections and a block is 1 KiB:

| Section | Holds |
|---|---|
| `DATA` | the content of the files |
| `META` | the table of the records and the map of the free blocks |
| `INFO` | the header of the volume and the root record, in the last erase sector |

A record of today is 32 bytes: it keeps the type of the object, its mode and
its owner in the upper bits of `flags`, and the two moments of a change
(`mtime` and `ctime`) in the seconds of Unix after them. A volume of an
older release has 16-byte records with no room for a time and mounts
unchanged; its objects report zero, which is "unknown" (ADR 0013). Moving a
0.x machine, and what a version 1 volume keeps, is in
[`docs/migration.md`](../migration.md). The formatter asks for 256 records
(`MFS_MOUNT_RECORDS`) and sizes `META` from the volume; the capacity is
fixed at format time and cannot grow later.
There is **no journal**: a power cut in the middle of a write leaves whatever
it leaves, and `fsck -y` is the way back (see
[`mkfs` and `fsck`](#mkfs-and-fsck)); a content that was half written is not
recoverable, and the check says so ([`docs/debts.md`](../debts.md)).

The calls of the filesystem take one recursive lock, so two tasks can use the
volume at the same time without corrupting it: each open file keeps its
position in its descriptor, and the volume itself is one piece of state behind
a mutex. The codes of the filesystem are translated into the codes of the
system calls in one place, `components/mfs/src/mfs_vfs.c`.

## Working with files

```text
microos> write /hello.txt one two three
14 bytes written to /hello.txt
microos> cat /hello.txt
one two three
microos> mkdir /data
microos> df /data
blocks: 482 of 487 free (1 KB each)
inodes: 1274 of 1279 free
microos> rm /hello.txt
```

`write` replaces the file and appends a newline; the text is one line of at
most 127 bytes. The count of the answer has the word of the number (`1 byte`,
`2 bytes`), and a text that is cut says how many bytes of it were kept. `rm`
removes a file, not a directory, and there is no `-r` without the flag.
`df` prints the free blocks and inodes of the filesystem of the path, and a
path that is not there is `df: /nope: no such file or directory` instead of the
numbers of the root. The block is the unit of the volume and the line names it:
MicroFS counts kilobytes (`1 KB each`), FAT32 its 512-byte sectors
(`512 bytes each`), and a size below a kilobyte is written as bytes and not as
`0 KB`. A volume that keeps no table of inodes says
`inodes: not kept by this volume` instead of a zero nobody read:

```text
microos> mount ata0p2 /data fat32
microos> df /data
blocks: 16098 of 16100 free (512 bytes each)
inodes: not kept by this volume
```

A name that is already there is refused with the sentence of what it is:
`mkdir /etc` over a directory says `is a directory`, and over a file
`file exists`. A path that is not there says `no such file or directory`
everywhere, the same words the rest of the shell uses. A name longer than the
volume holds is `file name too long`: MicroFS keeps 32 bytes of a name (ext2
and FAT keep 255), and the volume answers that limit with a code of its own
(`invalid argument`, or `file too big` for an entry of a directory), which the
shell names as the limit it is.

`stat` shows the mode, the owner and the size; `chmod` and `chown` change the
first two. A program asks for the moments of a change with `statx` (number 26
of the ABI) and `microos_statx()` in the library; a volume of version 1 has
none, so the two times come back as zero and not as 1970. The rules are in
[kernel-abi.md](kernel-abi.md) and in
[`docs/syscall-abi.md`](../syscall-abi.md): the uid of the task decides the
triad, root passes the read and the write, no record stores a group, and a
zero mode means "this record predates the modes", so `chmod 000` is refused.

## `mkfs` and `fsck`

```text
microos> mkfs -y
mkfs: the volume is formatted
```

`mkfs -y` unmounts `/`, formats the volume and mounts it again. Without `-y` it
does nothing but print its usage; with `-y` everything in MicroFS is gone. It
is registered on the board (`main/src/main.c`) and in the simulator
(`ports/port_host/src/sim_main.c`); the x86-64 port has no such command: its
volume comes with the disk image (`make x86-disk`), and a boot that finds
only the memory volume formats that one.

`fsck` of MicroFS is the read-only check:

```text
microos> fsck
fsck: 0 file(s), 3 directory(ies), 484 of 487 block(s) free
fsck: the volume is consistent
```

A broken volume prints `N problem(s), the first: ...` and the command exits
with an error. `fsck: no volume is mounted` means that `/` is not MicroFS on
this machine at the moment.

With `-y` (or `--repair`) MicroFS is repaired where the medium itself proves
the change, and the volume is checked again:

```text
microos> fsck -y
fsck: 0 file(s), 3 directory(ies), 484 of 487 block(s) free
fsck: 4 repair(s) applied
fsck: 0 problem(s) after the repair
fsck: the volume is consistent
```

The pass gives an orphan block back to the map of the free blocks, removes an
entry that no path can reach (the name of a free or of a vanished record, a
name that holds a slash, the later of two equal names), takes the type of the
record when the entry and the record disagree about it, cuts a chain that
loops or leaves the data section, ends the second of two objects that share a
block (the first owner in the walk of the tree keeps it, and the other keeps
its name as an empty file - the rule of FAT), puts the size of a file right
against its chain in both directions, counts the entries of a directory
again, and frees a record that no name reaches - a create or an unlink that
stopped half way - with its blocks. A directory whose first block is not in
the data section is given an empty block: its entries could not be reached,
and nothing on the medium says where they were.

A record has no checksum and neither has the content of a file, so a record
or a *content* that a power cut wrote half way cannot be told from a whole
one: the repair promises a volume that mounts and walks, not the data of an
interrupted write. It does not guess either: a damaged header (a volume the
mount would refuse), a header that disagrees with the mount, and a directory
whose first block another object holds are left as they are, counted in
`problem(s)` and printed with `(not repaired)`. When the tree cannot be
walked whole, no block and no record is given back: a block of a subtree the
walk did not enter looks exactly like an orphan.

Without the flag nothing is written at all: the bytes of the medium are the
same before and after `fsck`, which the tests compare byte by byte.

The second filesystem is checked by the same command, by the point or by the
device: `fsck fat /mnt/fat` reads a volume that is mounted, `fsck fat sd0`
mounts the device for the length of the walk. The report names the cluster,
the directory or the copy of the table that is wrong, and the counters say
how much of the volume is free and how many clusters no chain names. A
volume with more than 65536 clusters is refused with a message. Without a
flag it is read-only; with `-y` it repairs the damage the medium itself
explains. See [the second filesystem](#the-second-filesystem).

```text
microos> fsck fat /mnt/fat -y
fsck: FAT32, 6 file(s), 2 directory(ies), 1975 of 1984 cluster(s) free
fsck: 1 lost cluster(s) found, 1 freed
fsck: 1 repair(s) applied
fsck: 0 problem(s) after the repair
fsck: the volume is consistent
```

`-y` (or `--repair`) gives a lost cluster back to the free map in both
copies of the table, cuts a chain that loops or leaves the volume, ends the
second of two chains that share a cluster (an entry whose *own* first
cluster is the shared one becomes an empty file, so the name stays and the
data stays reachable through the first name), removes a broken run of
long-name entries and keeps the 8.3 name, and puts the size of a file right
against its chain: a chain shorter than the size cuts the size down, a chain
longer than the file gives the tail back. An entry whose first cluster is
not in the volume is removed with its long name, because nothing on the
medium says where its data is, and so is a file whose 8.3 name holds a
character FAT does not allow. A directory with such a name is left for a
person: its subtree would go with the entry.

The volume is checked again after the pass, so `0 problem(s)` is about the
medium and not about the plan. The pass does not guess: a boot sector that
disagrees with the mount, two copies of the table that differ (the medium
does not say which one is the volume), an empty file that still names a
chain (it could be a directory the walk has not reached), and a directory
whose first cluster another chain holds are left as they are, counted in
`problem(s)`, and the first one is printed with `(not repaired)`. When the
tree could not be walked whole (a shared directory head, a depth beyond a
path), the lost clusters are not freed either: a cluster of the subtree that
was not entered looks the same. A medium that refuses a write stops the pass
at the first change. The same command and the same flag repair MicroFS, with
the rules of the section above.

## `/etc` and the store

The files of `/etc` are the configuration of the system, and each file is one
key of the key-value store of the port (NVS on the board, a file per key in
the simulator). The boot restores the files that are missing and saves the
ones it finds; the file wins when the two disagree.

```text
microos> write /etc/setting hello
microos> config sync
config: 0 file(s) restored, 1 saved
microos> config
setting=hello
```

This is why `mkfs -y` does not take the settings away: the store survives the
volume. `idf.py erase-flash` destroys both. In the simulator the store is the
directory `MICROOS_KV_DIR` (`/tmp/microos-kv` by default), and a run with a
volume of its own should have a store of its own too.

The x86-64 machine has no medium that survives a reset, so it has no store:
`config` says `this machine has no store; the files of /etc are the settings`,
and `config sync` is an error rather than a "0 saved" that saved nothing. The
files are still the configuration there, and `config get`, `set` and `list`
work on them.

## `/etc/init.d`

Every file of `/etc/init.d` is a script of the init process: one shell command
per line, `#` starts a comment, and each script runs in a task of its own. The
directory is created at the boot when it is missing. The commands are the ones
of [shell.md](shell.md), so a script can only use what the machine registers:

```text
microos> write /etc/init.d/clock date
microos> reboot
```

## Disks and partitions

The kernel has a portable parser of the partition tables
(`components/kernel/src/partition.c`, [ADR 0008](../adr/0008-block-devices.md)):
it reads the MBR or the GPT in the first sectors, holds every entry against the
size of the disk, and registers a child device named after the parent plus `p`
and the number of the entry. A child adds the first sector of its partition to
every request and refuses what runs past its end.

| Device | Where | What |
|---|---|---|
| `flash0` | board | the internal flash; the `mfs` partition mounted at `/` |
| `file0` | simulator | a file of the host |
| `ram0` | x86-64 | memory, 512 KB, not persistent |
| `ata0`, `ata0p1` | x86-64 | the IDE disk and its first partition |
| `vd0`, `sd0`, `nvme0n1`, `mmcblk0` | planned | virtio, AHCI, NVMe, SDHCI |

Three rules of the layer matter at the console:

* the parser **never writes** to the medium: a broken table is a refusal, not a
  repair;
* a disk without a table is not a volume, and there is no "the whole disk is
  the filesystem" special case;
* what a filesystem sees is a device, so MicroFS on `ata0p1` does not know that
  a partition table exists.

The x86-64 disk is a raw 8 MB image, made by `tools/x86_disk_smoke.py
--mkimage` and kept in `build-x86/disk.img`; what is written to it survives a
reset, which `make x86-disk-smoke` proves with two boots. See
[`docs/ports/x86_64/README.md`](../ports/x86_64/README.md#the-disk).

## The second filesystem

The VFS knows filesystem **types** ([ADR 0009](../adr/0009-filesystem-types.md)):
`vfs_register_type()` puts a name and a mount function in the table, and
`vfs_mount_type(point, "fat32", device)` mounts a type on a point. MicroFS is
one type; `components/fs_fat32` is a second one, FAT12/16/32 with the long
names, the fixed and the cluster root and the subdirectories.

The volume is **read and written**. It speaks the whole life of a file:
`open` with `O_CREAT`, `O_TRUNC` and `O_APPEND`, `write`, `close`, `unlink`
and `mkdir`, on FAT12, FAT16 and FAT32 alike. The clusters are taken from
the free map and given back on removal, both copies of the table are kept
together, and a long name that is created gets its run of long-name entries
with the checksum of the generated 8.3 name. `df` works on it (`statfs`
walks the table), and so do the shell commands that go through the VFS:

```text
microos> write /mnt/sd/note.txt hello
6 bytes written to /mnt/sd/note.txt
microos> cat /mnt/sd/note.txt
hello
```

A volume is mounted by `mount <device> <point> <type>` or by `/etc/fstab` at
the boot (see [Mounting a volume](#mounting-a-volume)); an image that wants a
FAT volume registers the type and mounts it, and the board's kernel carries
the component behind `CONFIG_MICRO_OS_FS_FAT32`, off by default. The host
tests always build it.

What a FAT record cannot carry is still absent: `stat` answers `mode = 0`
and owner 0, `chmod` and `chown` are `ENOTSUP`, and there is no time of the
change. `unlink` removes a file or an empty directory and refuses a
directory that holds an entry with `ENOTEMPTY`; `rename` is a call of the
VFS (ADR 0024 §5) and FAT implements it: a name is a run of entries, so the
call writes the run of the new name and marks the old one removed, and a file
already at the destination is replaced. A directory is renamed only inside
its own parent. The mount does not ask whether the medium can be written: a
card with its physical write switch on mounts and is refused at the first
write, because reading it is still useful.

Writes go to the medium before the call returns and the device is synced
when it has a `sync` of its own, but there is **no journal**: a power cut in
the middle of a write can leave a cluster no chain names or a half-written
entry. `fsck fat <point|device>` walks the volume: the boot sector and the
BPB, the copies of the table, the chains, the entries and their long names,
and the clusters no chain names. Without a flag it only reports, and a
damaged volume is left the way it was found so that it can be copied off
the medium first. With `-y` the safe repairs are applied and the check runs
again; the rules are in [the section above](#mkfs-and-fsck), and what the
medium cannot explain is left and named. That is the price of writing a
volume this kernel did not format, and it is written in
[`docs/debts.md`](../debts.md).

The host test mounts MicroFS at `/` and FAT at `/mnt/fat` at the same time,
reads and writes through the VFS, and mounts the same image again after an
unmount to prove the bytes are on the medium. On a board or an x86-64 image
the type is registered at the boot when `MICRO_OS_FS_FAT32` is on; the board
has it off by default.

MicroFS registers its own type (`mfs`) as well, so the volume of the flash
can be named like any other.

## The third filesystem: ext2

`components/fs_ext2` is a second reader of a format of the world, and the
first one with inodes ([ADR 0019](../adr/0019-ext2.md), [ADR 0029](../adr/0029-ext2-write.md)). The type is
`ext2`; the component is behind `MICRO_OS_FS_EXT2`, off by default on the
board and on x86-64, and linked by the host build and its tests.

The reader walks the superblock at byte 1024, the table of the group
descriptors, the inode table of every group, the twelve direct blocks of an
inode and one, two or three levels of indirection. A directory is a stream
of records of a variable length, the names are compared byte for byte (ext2
is case sensitive) and a name of up to 255 bytes is found whole even when
the listing cuts it at the 32 bytes of `struct vfs_dirent`. A hole of a
sparse file reads as zeros.

The attributes are the ones of the inode, not zeros: `stat`, `statx` and
`ls -l` report the nine bits of `i_mode`, the uid of the owner and the
moments of the change:

```text
microos> mount ata0p1 /mnt/linux ext2
mount: ata0p1 is mounted at /mnt/linux
microos> ls -l /mnt/linux
-rw-r--r-- 1000        11 21 Nov 12:00 hello.txt
drwxr-xr-x root      1024 21 Nov 12:00 sub/
microos> cat /mnt/linux/hello.txt
hello ext2
microos> df -h /mnt/linux
/mnt/linux: 63.9 MB total, 7.4 MB free
```

The volume is **read and written**, and it is a root of the machine
([ADR 0027](../adr/0027-a-root-that-is-not-microfs.md)). A write allocates the
block in the bitmap of its group, a new name inserts a record in the
directory, and a file grows through the twelve direct blocks and the three
tables of pointers of the format - a file of 128 MiB of a 5 GiB volume lives
in the double and the triple table. `truncate`, `unlink`, `mkdir`, `rmdir`,
`rename`, `chmod` and `chown` are there; `fsck ext2 [-y] <point>` checks the
mounted volume and repairs the counters, the bitmaps, the link counts and a
lost file, there is no journal (an ext3 or ext4 volume is refused with
`ENOTSUP`) and a link is reported but not followed. A damaged volume is
described as it is: a superblock or a geometry that cannot be walked is
`EINVAL`, a block or an inode outside the volume is `EIO`.

An image this machine wrote passes `e2fsck -fn` of the host and reads back
in `debugfs`, because every operation that allocates or frees settles the
counters of the superblock when it is done - the root is never unmounted,
and the numbers are what the tools of the world compare with the bitmaps.
The check of the machine has the walk (`ext2_check`) and no command of the
shell reaches it yet.

The tests build the image themselves - a superblock, one group, a table of
inodes, two directories and a file that uses all three levels of
indirection around a hole - and mount it next to MicroFS; `mke2fs` is not a
dependency of the suite. The board's boot does not register the type yet,
which is in [`docs/debts.md`](../debts.md).

## A volume of 5 GiB

A volume of 5 GiB is an ordinary volume of this system: the image is sparse
on the host (7 MB of a disk of 5 GiB), the machine mounts it as `/`, `df`
counts its blocks, a file of 128 MiB goes in at about 1.2 MiB/s and comes
back at 4 MiB/s, and `e2fsck -fn` of the host finds nothing to repair in
the image of the session. The gate is a tool and a target of its own:

```bash
make x86-volume-smoke     # about four minutes, not a part of make ci
```

```text
ext2: the image of 5120 MB is 6 MB of the host (0.0 s of mkfs)
ext2: wrote 128 MiB at 1.20 MiB/s (106.8 s), read it at 4.00 MiB/s (32.1 s)
ext2: e2fsck -fn of the host finds nothing to repair
mfs: the image of 5120 MB is 208 MB of the host (1.2 s of mkfs)
mfs: wrote 16 MiB at 1.81 MiB/s (8.9 s), read it at 12.93 MiB/s (1.3 s), fsck 15.7 s
mfs: heap free 13488 KB on 5 GiB and 14101 KB on 16 MiB (613 KB of map)
mfs: fsck of the machine says the volume is consistent
RESULT: PASS
```

The two volumes pay for their size in different places, and the numbers are
the honest part of the answer:

* **ext2** costs the filesystem: the image is sparse because the volume is a
  hole in it, and a write is about a megabyte a second, because every block
  of the file is a write of the bitmap of its group, its descriptor and the
  table of pointers, with no cache of the group and no run of blocks.
* **MicroFS** costs the metadata: the map of the free blocks is the memory
  of the volume (615 KiB of 5 GiB, 2 KiB of 16 MiB), which is the point of
  the change of 1.3 - and the `META` section is 4% of the volume, so the
  table of the records is 214 MB of which 66 records are used, and the walk
  that counts the used records (`df`, `fsck`, the mount) is 14 s of the disk
  of QEMU. The image is 209 MB on the host for that reason.
  [`docs/debts.md`](../debts.md) has the row and what it needs: a `META`
  sized by the records the format asks for.

## Mounting a volume

`mount` without a word lists the points of the tree, in the order the boot
mounted them. With the three words `device point type` it mounts a volume of a
registered type; `umount` gives the point back:

```text
microos> mount
/
/mnt/fat
microos> mount ata0p1 /mnt/fat fat32
mount: ata0p1 is mounted at /mnt/fat
microos> ls /mnt/fat
...
microos> umount /mnt/fat
umount: /mnt/fat is out of the tree
```

The point need not exist in the filesystem above it, and neither does its
parent: a mount is shown in the listing of the directory above, which is how
`/proc`, `/dev` and `/mnt/fat` appear in `ls`. What is already there has to be
a directory, so a mount over a file is `not a directory`; a point that already
holds another filesystem is `device or resource busy`, and a point with a
mount below it stays until the smaller one leaves. `/` is the exception: the
mounts of `/proc` and `/dev` do not live on its volume, so `mkfs -y` may take
the root out while they stay. `umount /` is refused, and so are `/proc` and
`/dev`: the boot mounts the two and no type of the table can mount them
again. The message is about the image, not about a formatter this machine may
not have: the volume is formatted by `mkfs -y` where there is one, and by the
build of the image everywhere.

## `/etc/fstab`

The boot mounts the volumes of `/etc/fstab` after the root, when the file is
there. Each line is `device point type [options]`, `#` starts a comment and an
empty line is not a line:

```text
# the volumes of the machine: the device, the point and the type
vfsfat0  /mnt/fat  fat32

# a line of Linux with the options of a mount is read as well
ata0p1   /mnt/card fat32  defaults
```

Only `defaults` is a known option for now: an option that the kernel cannot
honour (`ro`, a comma-separated list) is named in the log and its line is
skipped. A line that does not mount for any other reason - a type the image
does not have, a device that is not there, a point that is busy - is skipped
the same way: a broken table does not stop the boot, and the shell still
starts with `mount -a` to read it again after a repair.

The file lives in MicroFS, so it is one of the files of `/etc` that the store
of the port keeps across a format. A volume mounted at the console is not
written back to the table: `mount` changes the tree, not the configuration.

## The settings of a machine

A few settings are files of `/etc`: the name of the machine, the console, the
table of the mounts. The systems of parameters - `/etc/display` with
`rotation=90`, `/etc/font` with `scale=1`, `/etc/console` with `theme=dark` -
are one file per system, with `key=value` lines and comments; the boot applies
them before the first frame, and a key the machine cannot honour is kept and
ignored with a warning ([ADR 0021](../adr/0021-settings-in-etc.md)).

```text
microos> cat /etc/display
# the screen
rotation=0
invert=1
brightness=100
theme=dark
microos> config set display.rotation 90
display.rotation=90
microos> config get display.rotation
display.rotation=90
```

The files stay data of the volume: `cat`, `write` and `config` edit them, and
the store of the port brings them back after a format. The keys, the defaults,
the limits of a file and the table of what each machine honours are in
[settings.md](settings.md).

## What is not there yet

* **No journal under a write.** A write of MicroFS, of FAT or of ext2 is not
  logged: `fsck -y` repairs the damage the medium explains (lost clusters, a
  loop, a cross-link, a size against its chain), and leaves what it cannot
  prove for a person. `fsck fat -y` does not touch the boot sector, two
  copies of the table that differ or a directory whose head is shared. The
  shell's `fsck` speaks of MicroFS, FAT and ext2 (`fsck ext2 [-y] <point>`),
  so a machine whose root is ext2 checks and repairs itself; `rename` and
  `rmdir` are in the VFS and in every filesystem of the tree.
* **No access time**, so `statx` has two moments of three; a read does not
  rewrite a record.
* **The options of `/etc/fstab` are not acted on**: only `defaults` is
  accepted, and a volume mounted at the console is not written back to the
  table.
* **MicroFS is one volume at a time**: the type `mfs` mounts the medium the
  filesystem keeps, so a second MicroFS volume is a change of the filesystem.
* **No journal under a write**: `fsck -y` repairs the damage that the medium
  of MicroFS or of FAT explains, and leaves what it cannot prove for a
  person; the data of a write that was in flight is not recovered.
* **No `mv`, no `cp`**: `rm -r` and the other commands of the shell are there.
* **One user**, so the owner of a file is a number that is stored and checked
  but not a second party.
* **No `mv`, no `cp`**: `rm -r` and the other commands of the shell are there.
* **One user**, so the owner of a file is a number that is stored and checked
  but not a second party.

The register with the next step of each row is [`docs/debts.md`](../debts.md);
the plan is [`docs/roadmap.md`](../roadmap.md), section K6.

[Back to the wiki index](README.md)
