# The plan of 2.0

> The major of the promise this project has been walking towards since
> ADR 0005: **the machine of its user** - a program that cannot be written by
> another, a `fork` that is a copy and not a re-run, two people on one machine,
> a desktop that starts with the machine, and a network with an address nobody
> typed.

1.9 gave the machine its files: a journal, a guard page, a terminal and the
order of a clean end. 2.0 is the release that lets the machine be **shared and
trusted**: one program cannot reach another, a copy of a process is a copy of
its pages, the accounts carry a group and a `setuid` bit, and a person who has
never seen this tree can install it and use it.

`2.0.0` is not cut by the autopilot: it writes the notes, cuts `v2.0.0-rcN`,
runs every gate and reports; the tag of `2.0.0` waits for the word of the owner
(`docs/roadmap-to-2.0.md`, "The rules of the autopilot").

## M1 - the MMU policy (x86-64)

The page tables exist since 1.0 (ADR 0007): a program is a task of ring 3 with
tables of its own. What the machine has not had is a **policy**:

* **W^X by the hardware**: no page both writable and executable, in the image
  and in the mappings a program makes; the audit of `exec` (1.9) becomes a
  refusal of the page table and not only of the image;
* **a stack that grows**: the guard page under a stack is a demand-paging
  start, and a program that uses more than its first page gets more, up to a
  limit that is a setting of the task;
* **a heap with a limit**: `brk` and `mmap` cannot pass a ceiling the task
  carries (`ulimit -m` is the shell side of it);
* **a kernel that ring 3 cannot write**: SMEP/SMAP if the processor has them,
  and the supervisor bit of every kernel page checked by a case;
* **a fault that names the reason**: the answer of a bad access is `EFAULT`
  with the address in the log, and the shell stays.

Gates: `make mmu-smoke` (new) with the attack suite - a program that writes its
text, executes its stack, writes a kernel address, grows past its heap limit,
and touches a page it does not own - each ends the **program** with the reason
in the log; `x86-smoke` with the W^X audit over every program of the image;
`soak` and `soak-smp` of 600 s.

## M2 - the fork of a real process (x86-64)

`fork()` copies the pages of the parent today (ADR 0005). The promise is that
the copy is a **copy of the pages** and not a re-run of the program, and that it
is cheap: a program of 64 MiB forks in a millisecond.

* copy-on-write for the pages of a program, with a reference count per page;
* a child that reads its parent's memory as it was at the call, and a write on
  either side that the other does not see;
* the pages are shared until one of them writes them - the counters say so;
* `fork` of a program of 64 MiB measured: `make fork-smoke` with the time in
  the output and a ceiling of a millisecond.

Gate: `make fork-smoke` (new): a program of 64 MiB, a fork, a write on each
side, a read of the other side, the time of the call and the count of the
shared and copied pages; `x86-smoke` with the small cases.

## M3 - the group and the setuid bit (ADR 0044)

The six steps the accounts of 1.6 left open, in the order of ADR 0044:

* `chgrp` under the next free number of the ABI, and in `vfs_operations` with
  the adapters of MicroFS, ext2 and FAT32;
* the group of a record in `struct vfs_stat` (the hole after `type` keeps
  `struct statx` at 40 bytes) and the triad of the group in the check of the
  access;
* `/etc/group`, `getgrgid` and `getgrnam`, the group of a new object from the
  gid of its maker;
* the **setuid bit of MOX1**: a flag in the header, the real/effective/saved
  uid, and the rule of `pkg` for the files that carry it;
* the matrix of the check, case by case, in the host tests.

Gates: `x86-smoke` (a file of a group that another account reads and does not
write; `su guest` over the socket of telnet; a setuid program of the image that
runs as its owner and cannot be written by the caller); `x86-root-smoke`;
`test` with the matrix; `idf-build` (the board's records grow by a byte and the
budget of the image is checked).

## M4 - the desktop of a person

* the window server starts **with the machine**: `init` starts the session that
  starts `wm`, and a logout returns to a fresh session;
* the programs of the desktop are the ones of 1.7 (editor, file manager) plus a
  **terminal** window that runs the shell of the machine on a pty;
* a clock, and the date of the machine from `/etc`;
* the palette of 1.7 in the theme, and a second size of the font.

Gates: `x86-vga-smoke` that boots to the desktop, opens a terminal window, types
a line and reads the answer; a screenshot of every window; the frame of the
compositor measured (full screen against a damaged one).

## M5 - the network of a person

* **Ethernet of a laptop**: the I219 of a ThinkPad (the `e1000e` of Intel) or
  the RTL8153 over the USB of 1.8, behind the contract of the port's `netdev`;
* **DHCP**: an address, a gateway and a resolver nobody typed, the lease in
  `/etc/network`, and a fallback to the fixed address of 1.8;
* `wget`, `httpd` and `pkg` over it, and the session of `telnet`.

Gates: `x86-net-smoke` with a DHCP server of QEMU (`-netdev user` answers
DHCP): the address is taken, `wget` reads a file of the host, `pkg` installs
from a local repository; the laptop of the owner on the network of the house
when a NIC is there.

## M6 - a guide for a stranger

* `docs/install.md`: from a released image to a booted machine in ten minutes,
  for a person who has never seen this tree - the stick, the firmware, the
  first boot, the login, the editor, the disk;
* every page of `docs/wiki` checked (`make wiki-check`), and a README that a
  stranger can follow;
* the known limits in one place, with the version that will fix them.

Gate: `make integrity` with every link, and a session of a person (the
cleanroom agent) who follows the guide and reports what was missing.

## The gate of the release

Everything of 1.9, plus: `make mmu-smoke` (the attack suite), `make fork-smoke`
(64 MiB in a millisecond), the matrix of the group and the setuid bit, the
desktop that boots to a terminal window, DHCP on the wire, and `docs/install.md`
followed by the cleanroom. The two sessions of 600 s, the board, a Release from
an empty directory and `integrity --expect-tag` as always.

## What is not in 2.0

* **Wi-Fi** (`docs/plan-wifi.md`, ADR 0039) - it follows the wire, and it is a
  lane of its own: `iwm` with `net80211` is a stack of a stack;
* the **ARM port** (`docs/plan-arm64.md`, a later release, no date);
* **3D**, a driver of a real GPU, and the 2D engine of `virtio-gpu` behind
  `fb_ops` if the compositor of M4 does not need it;
* **MQTT**, stopped for the x86 port in 1.6.
