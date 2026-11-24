# The plan of 1.9

> The release after 1.8.0, which gave the door of USB, a disk of a person under
> the rule that a medium this system did not format is read-only, the library of
> a program nobody rewrote, a volume whose grain is its own and the accounts of
> a session.

1.8 made a laptop work with the machine it is: a keyboard, a mouse, a stick, a
disk that keeps a file, a program that was not rewritten. 1.9 is the release
that makes the machine **own its files** - a file that survives a power cut, a
page that two programs share, a fault that is one program's and not the
machine's, and a wake that costs microseconds and not a tick. The lanes below
are the ones of `docs/roadmap-to-2.0.md` §1.9, with the two that 1.8 handed
over (the terminal and the wire) and the half of the accounts it left open.

Each lane is a branch of `release_candidate` with its own gates; a lane that
does not fit moves to 2.0 without rewriting the promise.

## L1 - the wake that preempts

The one defect of the ABI of the kernel that 1.7 measured and 1.8 made safe
without curing: `k_wait_wake` moves `wake_tick` to the present and the waiter
becomes runnable at the next tick, so a round trip of a pipe is **1.006 ms** -
one tick - on the host and on the board. K15 of the debts names the cause of a
lost wakeup: the waiter registers **after** the caller has looked at the
object.

* `k_wait_arm` / `k_wait_sleep` / `k_wait_disarm`: register while the lock of
  the object is held, look, then sleep - the wake cannot fall into the window
  because the window is closed by the lock;
* a wake that can be answered: the port's sleep is cut by the wake itself (the
  queue of the core is run, or the IPI of K8 sends it), and `WAIT_CHUNK_MS`
  stops being the resolution of a wait;
* the pipes of `kfile.c` and `ksys_poll` stop waiting twenty milliseconds at a
  time (`KFILE_WAIT_CHUNK_MS` was the bandage of 1.8);
* the numbers: `make bench` and `tools/x86_bench.py`, `lat_pipe_p50` under
  **100 µs**, and the medians of three runs in `docs/bench.md`.

Gates: `make test test-asan test-tsan test-smp test-minimal`, `x86-smoke`,
`soak` and `soak-smp` of 600 s, the bench of the two ports.

## L2 - the journal: a file that survives a cut

* **MicroFS version 4**: a write-ahead log of blocks, a header that says where
  the log is and how far it was written, the replay of the log at the mount,
  and a volume of version 3 that still mounts as it always did (the reader of
  the three versions);
* **ext2 with a journal** (the subset of ext3 that a volume of this tree needs:
  a journal inode, a descriptor block, a commit block, the replay at the
  mount), so that a volume of 4 KiB blocks and a volume of 1 KiB are both
  whole after a cut;
* the gate is a machine that is **killed in the middle of a write**: the image
  is copied, the copy is reset at a random point of a session, `fsck` is clean
  and the file is either whole or absent - never half.

Gates: `mfs-smoke`, `x86-volume-smoke` with the cut, `e2fsck -fn` after every
cut, `idf-build`, and the sessions of 600 s.

## L3 - the protection: a fault of a task is not a fault of the machine

* a **guard page** under the stack of every task of the kernel, and the fault
  of one that overflows it is the end of that task and not of the machine;
* a program that writes below its stack dies with `EFAULT` and the shell stays
  - on x86-64 and on the host;
* the audit of the space of a program: no page both writable and executable,
  and the check that the loader refuses an image that asks for one.

Gates: `x86-smoke` with the fault cases, `soak` (the shell survives), a new
`guard-smoke` if the cases do not fit the smoke of the machine.

## L4 - the terminal (the second half of the desktop, from 1.8)

* `/dev/tty` with `termios` (`tcgetattr`, `tcsetattr`, `VMIN`, `VTIME`), raw
  mode and the escape sequences of a screen: cursor, clear, scroll;
* so that a program of the outside world - `vi`, `less`, a shell of its own -
  owns the screen and the keys, and `/bin/edit` is no longer the only editor;
* the many colours: the palette of the console beyond the sixteen of CGA, a
  second size of the font, and the window of a program in the panel of `wm`.

Gates: `x86-vga-smoke` with a terminal, two sizes and a palette; `ports-smoke`
with a `less` of the outside world on a file of the volume.

## L7 - the machine that switches itself off (asked for on the way)

A machine that cannot be switched off is a machine whose files are whatever the
power cut left. The lane is the one the person asked for between the others,
and it is in: **ADR 0050**, the clean end of `kernel/power.c` (the sessions
closed, the volumes synced and the disks flushed **before** any task is
signalled, a second sync for what the tasks wrote, the unmount of every volume,
the power), the refusal of a shutdown whose flush failed, `k_port_power()` in
the contract of the port (1.13) with ACPI S5 on x86-64, deep sleep on the board
and a documented exit code on the host, and the commands `shutdown`,
`poweroff`, `halt`, `reboot`.

Gate: `make power-smoke` - the process of QEMU ends by itself on `shutdown`,
the same image boots again with a clean volume and its file, `reboot` boots the
same process a second time, and a block device whose `sync` fails refuses the
shutdown and leaves the machine running.

## L5 - the accounts, second half (from 1.8, ADR 0044 has the six steps)

* `chgrp` under the next free number of the ABI, `chgrp` in
  `vfs_operations` with the adapters of mfs, ext2 and FAT;
* `group` in `struct vfs_stat` and the triad of the group in `caller_bits()`;
  `struct statx` stays 40 bytes (the hole after `type`);
* `/etc/group`, `getgrgid`/`getgrnam`, the group of a new object from the gid
  of its maker, and the matrix of the check;
* the **setuid bit of MOX1**: a flag in the header, the real/effective/saved
  uid, and the rule of `pkg` for the files that carry it.

Gates: `x86-smoke` (a file of a group that another account reads and does not
write), `x86-root-smoke`, the host tests of the check.

## L6 - the wire (from 1.8)

* Ethernet of a laptop: the I219 of a ThinkPad (the e1000e of Intel) or the
  RTL8153 over the USB of 1.8, behind the contract of the port's `netdev`;
* **DHCP** - the address of a network and not a fixed one - and the lease in
  `/etc/network`;
* `wget`, `httpd` and `pkg` over it, and the session of `telnet` of 1.8.

Gates: `x86-net-smoke` with DHCP (a server of QEMU), a laptop of the owner on
the network of the house when a NIC is there.

## The gate of the release

Everything of 1.8 (host 76 000+, `x86-smoke` 404, vga 208, disk 57, net 103,
root 52, volume 32, smp 52, usb 58, ports 62, ahci 33, nvme 35, uefi 29, iso
14, sim 69, soak and soak-smp 600, the board, `idf-build`, a Release from an
empty directory and `integrity --expect-tag`), plus the bench of L1, the cut
of L2, the guard pages of L3, the terminal of L4, the group of L5 and DHCP of
L6.

## What is not in 1.9

* the **MMU policy of 2.0** (a page table per program with W^X enforced by the
  hardware policy, a guard page of every task of ring 0 on a machine whose
  kernel has an address space of its own) - the guard page of L3 is the half
  the kernel can do without an MMU;
* a **real `fork`** of a program of 64 MiB, which is 2.0;
* **Wi-Fi** (`docs/plan-wifi.md`), which follows the wire;
* the **ARM port** (`docs/plan-arm64.md`, no date).
