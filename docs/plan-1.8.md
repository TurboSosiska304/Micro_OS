# The plan of 1.8

> The release after 1.7.0, which gave the desktop, the disk at the speed of the
> medium, MicroFS version 3 and a machine that starts on a laptop from a stick.

What 1.7 leaves open is where 1.8 begins. The order below is by what a person
with a laptop gets first, and each lane is a branch of `release_candidate` with
its own gates; a lane that does not fit moves to 1.9 without rewriting the
promise.

## Where the lanes stand

| Lane | State |
|---|---|
| M1 USB | **the controller, the keyboard, a stick and a wire are in.** xHCI with a command ring, an event ring and an ERST, the slots and the ports (ADR 0040); a keyboard of the HID boot protocol whose reports the portable kernel decodes (`kernel/input_hid.c`) and whose events are the ones of PS/2; a stick of class 08/06/0x50 as a disk of the block layer; CDC-ECM as the second network device. The second iteration adds the hub of the class 09 (the power of its ports, the route string of a device behind it, four tiers of eight ports), the hot-plug of the root ports and of the hub ports, the HID mouse of the boot protocol as the pointer of `/dev/input0`, and the line of the controller (MSI, then MSI-X, then the legacy pin). What is not there: RNDIS, the notify endpoint of CDC, the transaction translator of a high-speed hub, the port status of a SuperSpeed hub, a stick unplugged while mounted, and a bridge of PCI. |
| M2 disk | **AHCI and NVMe read, and a volume the rule names is written.** ADR 0041 is the rule and it came before the code: a medium this system did not format is read-only, a volume is writable only when its superblock carries our mark *and* `/etc/volumes` names it, and a write below the first sector of a partition is refused. ADR 0042 is the pair of rings of NVMe. The write path is open: the root mount asks for the writes after its own `/etc/volumes` is readable (`vfs_mount_root_writes`), a second volume asks with `vfs_mount_type_write` or the `rw` option of `/etc/fstab`, `microos_mkfs --volumes` names the volume of the image, the two drivers carry `WRITE DMA EXT`/`FLUSH CACHE EXT` and the NVMe `WRITE`/`Flush` behind the same translation of ADR 0032, `/proc/mounts` says `rw` or `ro`, and `make x86-disk-smoke` proves a file that survives a reset on the IDE channel, the SATA controller and the NVMe namespace. What is not there: the `-w` flag of the shell's `mount` (the command belongs to the lane of `components/kshell`), MSI-X, a PRP list, more than one queue pair or SATA port, a bridge of PCI, several namespaces, and hot-plug. |
| M3 libc | **`popen`/`pclose`/`system` are in**, and so is the gate of the programs nobody rewrote: thirteen utilities of sbase build with the toolchain of `make sdk` with not one line changed and answer on the image of the release (`make ports-smoke`, 62 checks), the digest of every vendored file is held in `tests/ports/upstream.txt`, and `docs/ports-table.md` counts the lines of source, the lines of a port layer and the patches per program (577 / 0 / 0 for the thirteen). The lane found a defect of the port that the release carried since 1.5: the descriptors 1 and 2 were taken by the first `pipe()` or `open()` of a program. What is not there: `/bin/sh` (the one change that removes four limits of `popen` at once), `uname`, `double` in `printf` and `seq`, the second set of utilities, and the fifteen lines of `xargs` that `fork` now pays. |
| M4 grain | **a volume of 4 KiB is in** (ADR 0043): the grain is a property of the volume, not of the medium, and ext2 opens a view of the device whose sector is the one of its superblock; `e2fsck -fn` is clean on every path of a write at 4 KiB, and a write of 16 MiB at that grain read 2.5 MiB of the disk instead of 36 after the defect of read ahead was found and fixed. MicroFS stays at 1 KiB: a grain of its own is version 4 of the format, and it waits in the debts. |
| M5-M7 | not started. |

## M1 - USB, the door of a real laptop

A machine of these years has no PS/2 port the firmware will share, no COM
port, and no way to reach a stick but through xHCI. This is the lane that turns
a laptop that boots into a laptop that works.

* **xHCI**: the controller, the rings (`TRB`), the ports, the reset of a device,
  the enumeration and the descriptors; one driver, three classes on top of it;
* **HID boot protocol**: a keyboard of the outside world (and the internal one
  on a machine whose firmware keeps it on USB), and the mouse beside the PS/2
  one - the events of `/dev/input0` do not change;
* **mass storage (BOT/SCSI)**: a stick as a disk - the second medium of the
  release and the first writable one that is not the internal disk;
* **CDC-ECM / RNDIS**: a dongle or a telephone as the network of a laptop
  without a wire.

Gates: `make usb-smoke` under QEMU (`qemu-xhci` + a USB keyboard, a stick and a
CDC device); a laptop with a stick; `x86-smoke` and every smoke of 1.7 green.

The stick of the owner (a medium of 58 GB with one vfat partition) went through
the whole door: `make usb-stick-smoke USB_STICK=/dev/sdc` attaches it read-only,
the machine boots, `mount usb0p1 /mnt fat32` lists the tree of the partition and
reads a file of it, `write` is refused by the rule, and the medium is unchanged
- the digest of its head (the table and the first partition) and the counter of
the sectors the kernel gave the device (131 072 before, 131 072 after). The
digest of a medium of tens of gigabytes is bounded for the same reason the
counter is read: half an hour of reading proves nothing that the counter and
the head do not.

## M2 - the disk of a person: AHCI, then NVMe

* **AHCI**: the SATA controller of a chipset (PCI class 01/06), one command per
  port, IRQ per port, the identify data, a partition table;
* **NVMe**: the queues of a modern laptop (PCI class 01/08), `PRP` lists, one
  completion queue, MSI-X or the legacy line;
* **a writable root**: the volume of the release is a partition of the internal
  disk, the settings of `/etc` survive a reboot, and `pkg` installs for good.

**The rule that comes before the code** (an ADR of the lane): a disk this
system did not format is **read-only**, and a partition the table of the
medium does not name is not touched at all.

* a controller registers its disks **read-only**; a write is a decision of a
  mount (`mount -w`), not a consequence of a mount;
* a volume is writable only when its superblock carries the identifier of this
  system **and** the volume is named in `/etc/volumes` of the medium - the
  list `mkfs` writes and nothing else does;
* the driver refuses a write below the first sector of a partition: the table
  of the partitions is never written, and neither is the boot area;
* a superblock that is not ours (btrfs, ext4, NTFS, XFS) is refused at the
  mount, and no path opens it for writing;
* the first boot of a machine with a new controller lists what it found and
  writes nothing.

Gates: `make x86-disk-smoke` on both controllers; a reset that keeps a file; the
first laptop whose `/etc` and `/bin` are not the image of a stick; and a test
with a **foreign superblock** - the mount is refused, a write fails, and the
**SHA-256 of the whole image is the same before and after**, which is the only
proof that not one byte was touched.

## M3 - the libc of a program that nobody rewrote

The pass promised in 1.6 and not done: a program of POSIX builds **without a
port layer**.

* `setenv`/`unsetenv` (now `ENOSYS`), `popen`/`system`, `fscanf`/`scanf`,
  `glob`, `realpath`, `strptime`, `getopt_long`, `basename`/`dirname` where
  they are missing;
* **`malloc` from `brk`**, so that the arena of a program stops being a
  constant of its build;
* **`chdir` as a call** of the ABI, and **the wall clock** (now `time()` is the
  seconds since the boot);
* **buffered streams** for the formats that still read a byte at a time.

Gates: a new `make ports-smoke` that builds **N upstream utilities with no
patch at all** (the list is the gate: sbase, toybox-sized single-file tools),
and a table of "patched lines per program" that has to reach zero for the
console set. The libc without a port layer is a number, not a feeling.

## M4 - the grain of the disk: 4 KiB blocks

The five steps of `docs/debts.md`, the lane 1.7 named and did not take:

1. a view of a disk with a sector larger than the medium's, or a partition that
   names its own sector;
2. the image tool with `mke2fs -b 4096`, and the test volume after it;
3. every write path of ext2 against `e2fsck -fn` and `debugfs`;
4. MicroFS with a block of 4 KiB as a decision of the format (an ADR, a
   version 4 of the volume);
5. the numbers of `make bench` and `make x86-volume-smoke` on both grains.

Target: **100 MiB/s of writing** on a volume of 5 GiB, and the five operations
of the block layer per kilobyte down to one and a quarter.

**Taken in 1.8, and the target was the wrong number** (ADR 0043): the five
operations per kilobyte were closed by the run of ADR 0037 and not by the
grain - the block layer is at 0.10 per kilobyte at either grain - and the
grain of 4 KiB is the *slower* one on the disk of the port (122 against 141
MiB/s written on the volume of 5 GiB, 186 against 286 in `make bench`) because
the cost of a write is the command of the channel and the metadata of ext2 is
a record: at 4 KiB blocks the same file costs 22 384 sectors of the medium
where 1 KiB blocks cost 20 796. Both grains pass `e2fsck -fn` and both are
above 100 MiB/s. What the lane did deliver: the view of a volume at a grain of
its own, the ext2 root of `--block-size`, the two grains in the gate, and the
defect of the read ahead that the grain exposed (a 16 MiB write read 36 MiB of
the disk). MicroFS keeps its kilobyte: a 4 KiB block is version 4 of its
format, and `docs/debts.md` names the shape of that change.

## M5 - the desktop, second half

The cleanroom of 1.7 said: "it looks like the 90s". That is not a defect, it is
the absence of design, and it is cheap to fix.

* **a terminal**: a `/dev/tty` with `termios`, so that a program of the outside
  world - `vi`, `less`, a shell of its own - can own the screen and the keys;
* **the window of a program** in the shell: `wm` starts with the system and
  runs a program of `/bin` from its panel;
* **the many colours**: the palette of the console is 16 colours of CGA on a
  screen of 65 536; the theme of `/etc/console` grows a palette, the font a
  second size, and a cell a shadow - all of it drawing, and all of it cheap;
* **the 2D engine**: the driver of `virtio-gpu` 2D behind the contract of
  `kernel/fb_ops.h`, written in 1.7 with the software path as its only
  implementation. A lane of its own, after the compositor has numbers.

Gates: `x86-vga-smoke` with a terminal, two font sizes and a palette; the frame
of the compositor measured (a full screen against a damaged one).

## M6 - the accounts, second half

* the **setuid bit of `MOX1`** and a **group in a record** with its triad in
  the check of the access - both are format decisions, both need an ADR;
* `/etc/group`, `newgrp` or nothing (the ADR chooses), the owner of a new file
  from the uid of its maker (the open debt of 1.6);
* a **session**: `login` as the program `init` starts, `logout`, `who`;
* `telnet` **reads its password from the socket** and not from the console of
  the machine - a defect of 1.6 that the cleanroom wrote down.

## M7 - the wire

* **Ethernet** on a laptop: the I219-V of a ThinkPad (or an RTL8153 over USB
  from M1) and **DHCP** - the address of a network instead of a fixed one;
* `httpd` and `pkg` over it, and `wget`;
* **Wi-Fi stays a plan**: `docs/plan-wifi.md`, ADR 0039, the BSD `iwm` with
  `net80211`, after the disk, the wire and USB - and not in 1.8 unless the
  other six lanes land early.

## What is not in 1.8

* the **ARM port** - a later release, no date, `docs/plan-arm64.md`;
* **3D** (a GL stack, virgl, Vulkan) and a driver of a real GPU;
* **MQTT**, stopped for the x86 port in 1.6;
* the MMU policy of 2.0 (a guard page and protection for every task of the
  kernel is a lane of its own when the machine has an MMU to give).

## The gate of the release

Everything of 1.7 (`x86-smoke` 378, `x86-vga-smoke` 191, disk 57, net 90,
root 52, mfs 10, volume 20, smp 52, uefi 26, the three of the medium, sim 69),
plus `usb-smoke`, `ports-smoke`, the two controllers of M2, the two grains of
M4 and a terminal in the screen test; the two sessions of 600 seconds, the
board, a Release build from an empty directory and `integrity --expect-tag`.
