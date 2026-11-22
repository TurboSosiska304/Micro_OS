# Migrating from 0.x to 1.0

This guide is written against the tree of `0.8.0-rc1`, the last 0.x release
candidate, and it describes what 1.0 changes and how to move a machine. It
does **not** promise what the tree does not do: a line that is still a gate of
1.0 in [`docs/releases/1.0.md`](releases/1.0.md) is marked as such here, not
turned into a feature.

Before 1.0 the minor number was a phase, not a compatibility promise
([ADR 0011](adr/0011-versioning-and-1.0.md)). 0.5, 0.6, 0.7 and 0.8 could and
did change a format. 1.0 freezes the system call numbers, the MOX1 format and
the MicroFS volume; after it, a breaking change is 2.0.0.

## What changes

### The MicroFS volume

A version 1 volume has 16-byte records; a version 2 volume has 32 bytes and
the time feature (`mtime` and `ctime`, read by `statx`, call 26). `mfs` mount
reads either, sets the record size from the header and keeps writing the size
it found.

So an old volume **mounts unchanged**, read and write, and its objects report
the time `0`, which means "unknown". That is the migration the tree has
today: mount, do not reformat. A volume that claims the time feature without
the room for it, or the room without the feature, is refused.

There is no in-place conversion command in this tree yet. To get the times,
copy the files onto a volume formatted by 1.0 (`mkfs -y` destroys the old
one). The shell can `cat` and `write` a file, and `microos_mkfs` — the host
tool behind `make x86-disk` — writes a file of the host into a fresh image.
The gate "migration that does not reformat" is still open
(`releases/1.0.md` §1); when it closes it will be named here.

### `/etc` and the store

The files of `/etc` may be backed by the key-value store of the port (NVS on
the board). The store survives `mkfs -y` and a flash of the application;
`config sync` saves the files or restores the missing ones. On the board,
`idf.py erase-flash` wipes the store as well.

`/etc/fstab` is mounted by the boot after the root: one line is
`device point type [options]`, a broken line is reported and skipped, and a
volume mounted at the console is not written back to the table.

### Programs on the disk

A program is a file of a volume, not a blob of the kernel. `make x86-disk`
writes `/bin` and `/etc` into a MicroFS image with a host tool, the boot
mounts the volume and `execve` reads the program; no boot installs one.

On the board the programs are files of the `progs` partition, mounted at
`/bin` (ADR 0022); a volume of an older release may still hold a `/bin`, and
the mount hides it. The layout of 1.1 is a one-time move that loses the old
volume: back it up, then flash with `--erase-volume` or run `mkfs -y` at the
shell.

### The syscall ABI

The ABI grew by adding: `statx` 26 in 0.6, then `pipe` 27, `dup2` 28,
`kill` 29, `signal` 30 and `sigreturn` 31 in 0.7. The existing numbers did
not move. `KSYS_COUNT` is 32: there is no call 32 or 33, and a new call would
take the next free number, never reuse one.

A program built for the numbers of 1.0 runs on every later 1.x. A program of
0.x that uses only the old numbers keeps working if it is an image of the
same machine and obeys the rules below.

### MOX1 version 2

Version 2 is version 1 with the machine in the field at offset 28
(`arch`: 0 x86-64, 1 Xtensa). The kernel reads version 2 and refuses version 1
(and a later generation) with `EINVAL`; an image of the wrong machine is
`ENOEXEC` before a byte runs.

Rebuild an old program with `tools/mkprog.sh`. The script defaults to
`--arch x86-64`; the board's programs use `--arch xtensa` and the windows of
its port. Keep the source or the object file, not only the `.mox`.

### NX on x86-64

0.8 makes the x86-64 map W^X: the text of the kernel and of a program is
read-only code, and every data page, bss, stack, heap and the screen is NX
([ADR 0007](adr/0007-paging-and-user-mode.md)). A program that writes its own
text, puts code in its data or jumps into a buffer now takes a page fault
instead of running.

Fix the program: code belongs in the text of the image, data in the data.
There is no `--no-nx` boot flag and no compatibility switch.

### A program on the board

A `.mox` of arch 1 (Xtensa) runs on the board **without protection**
([ADR 0018](adr/0018-esp32-programs.md)):

* the text is 8 KiB of instruction RAM and the data 8 KiB of DRAM;
* one program runs at a time, and a program cannot start another;
* a bad pointer can read or write kernel memory and a fault reboots the
  board.

The header and the machine of an image are checked before a byte runs, but a
valid image is trusted like the firmware. Do not run code you would not
flash.

### `fsck` and FAT

`fsck` of MicroFS is a read-only check. `fsck fat <point|device>` is
read-only without a flag; `-y` repairs the damage the FAT medium itself
explains (lost clusters in both copies of the table, a loop, a cross-link, a
size against its chain). There is no `fsck -y` for MicroFS.

## How to upgrade

### Before anything

1. Note what you run: `cat /proc/version` and `uname -a` in the shell, and
   the version of the bundle you have.
2. **Back up MicroFS.** It is the data; the firmware is replaceable.
   * board: dump the `mfs` partition with esptool, which needs no shell
     command in the tree:

     ```bash
     python -m esptool --chip esp32 -p /dev/ttyACM0 \
         read_flash 0x210000 0x5F0000 mfs-backup.bin
     ```

     The offsets are the ones of `partitions.csv`. Keep the file next to the
     firmware you flash, and restore it only over a layout with the same
     offsets.
   * x86-64: `cp build-x86/disk.img disk-backup.img` — this is the disk the
     persistent machine writes.
   * simulator: copy the volume file (`/tmp/microos-sim.img` by default, or
     the path `make sim` was given).
3. **Back up `/etc`.** `config` lists the store, `config sync` writes the
   files there. The store is outside MicroFS: it survives `mkfs -y` and a
   flash of the application, but not `idf.py erase-flash`.
4. **Check first, repair second.** `fsck` on MicroFS is read-only and safe.
   `fsck fat <point>` is read-only as well; run `fsck fat <point> -y` only
   after copying what matters, because a repair changes the medium.

### The board, over USB

```bash
make idf-build
make idf-flash IDF_PORT=/dev/ttyACM0
```

`idf.py flash` writes the bootloader, the partition table, `ota_0`, the
initial otadata and `progs`. It does **not** touch the NVS or the `mfs`
partition, so a volume of the same layout stays where it is and is mounted by
the new kernel. A release installs with `make bundle` and `dist/flash.sh`,
which writes the parts at their offsets and leaves the NVS and MicroFS alone.

`1.1.0` is the layout change (ADR 0022): the volume moves from `0x210000`
(5.94 MiB) to `0x2C1000` (5.25 MiB), the second application slot takes the old
region and the volume cannot be read at the new offset. **Back it up before
the flash** (the command above), then:

```bash
./dist/flash.sh --erase-volume /dev/ttyACM0
```

or flash normally and run `mkfs -y` at the shell. The old bytes are still on
the flash until the new volume is written, but the new table does not point at
them; copy the files you need from the backup with a tool that reads the old
MicroFS, or from the board before the move. A volume of 1.1 is never restored
at `0x210000`: restore it at `0x2C1000` only if it was taken from the new
layout.

Do **not** run `idf.py erase-flash` for an upgrade. It wipes NVS and MicroFS
and turns the upgrade into a factory install. Use it only when that is what
you want.

### x86-64 in QEMU

```bash
make x86-disk           # a fresh programs.img: /bin and /etc of MicroFS
make x86-run-disk       # the persistent disk.img, made from it on first run
```

`build-x86/programs.img` is the image `make x86-disk` writes; `disk.img` is a
copy made on the first `make x86-run-disk` and kept between runs. Copy
`disk.img` aside before the first run of a new tree, because the target only
fills it when it is missing. `make x86-run` and `make x86-smoke` boot the
build image directly and lose what they write when they exit.

An old `disk.img` with a version 1 volume still mounts. Its `/bin` is the one
written by the old tools; to get the programs of 1.0, either copy the files
you want to keep into a new image, or run `fsck` first and then use the new
`programs.img` as the source of `/bin`.

### The simulator

`make sim` uses a volume in a file and formats it when the file is missing;
`make smoke` uses a volume of its own and removes it. Copy the file before
the first run if it has data worth keeping. The host port mounts a version 1
volume the same way the board does.

### After the upgrade

```text
microos> cat /proc/version
microos> fsck
microos> ls /bin
microos> exec /bin/hello
microos> stat /etc/setting
```

A version 1 volume keeps the modes and sizes it has and reports the time as
`0`; a version 2 volume prints the moments of the change. `fsck` must say
`the volume is consistent`. Then `config sync` if `/etc` is not the one you
expect.

## What cannot be undone

* **A version 2 volume is not for 0.x.** A 0.x kernel does not know the
  32-byte record; do not point an old kernel at a volume formatted by 1.0.
  Keep the backup until the new machine has booted and `fsck` is clean.
* **MOX1 is one-way.** A version 2 image is refused by a 0.x kernel and a
  version 1 image is refused by 1.0. Rebuild from the source; keep the object
  files or the source, not only the `.mox`.
* **NX is not a switch.** A program that relied on writable text or
  executable data fails on x86-64 after 1.0. Fix the program; there is no
  boot flag to turn the rule off.
* **`erase-flash` is final.** It removes the store and the volume; the
  recovery is the backup taken before it.
* **After 1.0 the contracts are frozen, not the bugs.** The syscall numbers,
  MOX1 and the MicroFS volume change by adding only; a breaking change is
  2.0.0. A program built against 1.0 is expected on 1.0.7 and 1.9.0, and the
  changelog says what each release added.

## If something goes wrong

* `exec: /bin/hello: not a MOX1 image of this system` — the file is a version
  1 image, or one built for another machine. Rebuild it with
  `tools/mkprog.sh` (version 2, the right `--arch`).
* `the program faulted: vector 7` — the program wrote its text under NX.
  Move the data to a writable section; do not patch code.
* `the program faulted: vector 14` — the program fetched code from an NX
  page (data, bss, stack). The code has to be in the text of the image.
* `stat` prints `mode: (not set)` and no time — `/proc`, `/dev`, or a version
  1 volume. Expected: those records keep no mode, and an old volume keeps
  the time `0`.
* the board reboots when a program runs — the program touched memory that is
  not its own, and the board has no MMU. That is the documented price of a
  program without protection; fix the pointer.
* `/etc` is empty after an upgrade — `idf.py erase-flash` wiped NVS, or
  `mkfs -y` ran before `config sync`. Restore the store from the backup;
  `config sync` writes the files back.
* the old files are gone after a new `make x86-disk` — `programs.img` was
  rebuilt and `disk.img` is a copy of it. Use the `disk.img` you copied
  aside; do not remove it before the backup.
* `fsck` names a problem on MicroFS — a write was cut by a reset or a power
  loss. The check is read-only: copy the files you can read, then format
  (`mkfs -y`) and restore.
* a FAT volume has lost clusters — a write without a journal. Copy what
  matters first, then `fsck fat <point> -y`; what the medium cannot explain
  stays `(not repaired)`.

The full list of symptoms is in the wiki
([troubleshooting](wiki/troubleshooting.md)); the gate of 1.0 and what is
still open is in [`docs/releases/1.0.md`](releases/1.0.md).
