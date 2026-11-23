# The plan of 1.8

> The release after 1.7.0, which gave the desktop, the disk at the speed of the
> medium, MicroFS version 3 and a machine that starts on a laptop from a stick.

What 1.7 leaves open is where 1.8 begins. The order below is by what a person
with a laptop gets first, and each lane is a branch of `release_candidate` with
its own gates; a lane that does not fit moves to 1.9 without rewriting the
promise.

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

## M2 - the disk of a person: AHCI, then NVMe

* **AHCI**: the SATA controller of a chipset (PCI class 01/06), one command per
  port, IRQ per port, the identify data, a partition table;
* **NVMe**: the queues of a modern laptop (PCI class 01/08), `PRP` lists, one
  completion queue, MSI-X or the legacy line;
* **a writable root**: the volume of the release is a partition of the internal
  disk, the settings of `/etc` survive a reboot, and `pkg` installs for good.

Gates: `make x86-disk-smoke` on both controllers; a reset that keeps a file; the
first laptop whose `/etc` and `/bin` are not the image of a stick.

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
