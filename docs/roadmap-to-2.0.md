# The road from 1.8 to 2.0

> The plan of the three releases that follow 1.7.0, written before the work
> starts and kept honest by the gates of each one. **2.0 is not published
> without the approval of the owner of the machine**: the autopilot cuts the
> candidates (`2.0.0-rcN`) and stops there.

## Where 1.7.0 leaves us

A machine that boots on a laptop of UEFI only from a stick, with a desktop of
two windows, a disk of 125 MiB/s of writing on ext2, MicroFS version 3 of 660
blocks of metadata on 5 GiB, and a keyboard of any scancode set. What it cannot
do: write to the internal disk of a laptop, talk to a USB anything, ask a
network for an address, run a program of the outside world without a port
layer, or protect a task from another.

## 1.8 - the machine of a person with a laptop

| Lane | What it gives | The gate |
|---|---|---|
| **USB** | xHCI, HID boot (a keyboard of the outside world), mass storage (a stick as a disk), CDC-ECM/RNDIS (a telephone as a network) | `make usb-smoke` with a keyboard, a stick and a CDC device; the first laptop whose keys are USB |
| **AHCI and NVMe** | the internal disk: read first, write only into a volume this system made, and the volume survives a reboot | `x86-disk-smoke` on both controllers; a foreign superblock refused with the SHA-256 of the image unchanged |
| **the libc of a program nobody rewrote** | `setenv`, `popen`/`system`, `fscanf`, `glob`, `realpath`, `strptime`, `getopt_long`, `malloc` from `brk`, `chdir` as a call, the wall clock | `make ports-smoke`: N upstream utilities with **no patch**, and a table of patched lines that reaches zero |
| **the grain of 4 KiB** | five sector operations of the block layer per kilobyte down to one and a quarter | 100 MiB/s of writing on a volume of 5 GiB, both filesystems, `e2fsck` clean |
| **the desktop, second half** | a terminal with `termios`, the panel that starts a program, a palette and a font with two sizes | `x86-vga-smoke` with a terminal and a palette |
| **the accounts, second half** | the setuid bit of `MOX1`, the group of a record with its triad, `/etc/group`, a session, `telnet` that reads its password from the socket | a task of uid 7 that cannot read what is not its own, and a `su` that works over the network |
| **the wire** | Ethernet of a laptop (I219-V or RTL8153 through USB) and **DHCP** | `wget` and `pkg` over a network with an address nobody typed |

If the release is cut by readiness, **USB and AHCI/NVMe come first**: they are
what a laptop is missing. The rest moves to 1.9 without rewriting a promise.

## 1.9 - the machine that owns its files

| Lane | What it gives | The gate |
|---|---|---|
| **the journal** | ext2 with a journal (ext3 compatible) and MicroFS with a write-ahead log: a power cut costs the last write and nothing else | `e2fsck` of a volume cut in the middle of a transaction, and a file that is whole before it and absent after it |
| **the page cache** | a file read by two programs read once; `MAP_FIXED`, `mprotect`, a shared mapping that is a shared page and not a copy | `make bench` of two readers; `mmap` shared between two programs byte for byte |
| **protection** | the guard page of every task of the kernel, and the check that a fault of a task cannot be a fault of the machine | a program that writes below its stack dies and the shell stays |
| **the scheduler** | a wake that preempts: a call of an empty pipe answers in microseconds and not in a tick (1.7 measured **1.006 ms**, one tick) | `lat_pipe_p50` under 100 µs |
| **the installer** | `pkg` with dependencies, an update of the packages as a transaction, and a repository that signs the index and the sources | a package installed, upgraded and rolled back |
| **the board** | the ESP32 as the compatibility port: its own volume format of version 3, its `fsck`, its update over the air with a rollback on the hardware | `device-test` and `make board-soak` green |

## 2.0 - the machine of its user

The major of the promises that a release of the 1.x line could not keep, and
the one this project has been walking towards since ADR 0005.

| Lane | What it gives | The gate |
|---|---|---|
| **the MMU** | a page table per program with a policy: no executing a writable page, no writing an executable one, a stack that grows, a heap with a limit, and a kernel that cannot be written from ring 3 | `x86-smoke` with the W^X audit of a program's space, and a suite of attacks that each fail with `EFAULT` |
| **`fork` of a real process** | the copy is a copy of the pages and not a re-run of the program: a program of 64 MiB forks in a millisecond | `make fork-smoke`: a child that sees the parent's memory as it was at the call |
| **the user's session** | `login` from `init`, `logout`, `who`, a home that is a directory of a volume, and the right of a person to their own files | a machine where two people log in one after the other and see only their own home |
| **the desktop of a person** | a window server that starts with the machine, a terminal, an editor, a file manager, a clock, and a 2D engine behind `fb_ops` when the compositor needs it | `x86-vga-smoke` that boots to the desktop and runs a program from it |
| **the network of a person** | Ethernet and Wi-Fi of a laptop: the BSDS path of `docs/plan-wifi.md` (`iwm` with `net80211`) when the other lanes have landed, and not a promise if they have not | the laptop of the owner on the network of the house |
| **the docs** | a guide that a stranger reads: how to write a stick, how to boot, what works and what does not | `make integrity` with every link, and a README that a person who has never seen the tree can follow |

**2.0.0 is not published by the autopilot.** The autopilot writes the notes,
cuts `v2.0.0-rc1` and each `rc` after it, runs every gate and reports; the tag
of `2.0.0` waits for the word of the owner of the machine. **The wait is ten
minutes**: a candidate taken for approval and answered by nothing is not a
reason to sit still - the autopilot goes on with the debts and the side tasks
of the same line (the rows of `docs/debts.md`, a patch for a defect found on
hardware, the documentation of what was built), and it keeps the candidate and
the report ready for the moment the word comes.

## The rules of the autopilot

* a release is cut when its gates are green and not before: **1.8.0** and
  **1.9.0** are the autopilot's own; **2.0.0** is not;
* a patch (`1.7.1`, `1.8.1`) may carry a defect found on hardware, and only
  then;
* a lane that does not fit moves to the next release, and the release notes say
  so instead of promising;
* nothing of the disk of the owner is written to without the rule of
  `docs/plan-1.8.md` (a volume this system made, named in `/etc/volumes`);
* the ARM port stays a later release with no date (`docs/plan-arm64.md`).
