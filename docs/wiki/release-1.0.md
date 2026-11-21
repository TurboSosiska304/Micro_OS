# The 1.0 release

This is the page of the first major release of Micro OS. It says what `1.0.0`
is, what it promises, what it deliberately does not, and how a machine is
updated from a 0.x release. The checklist behind it is
[the gate of 1.0](../releases/1.0.md), and the decisions are ADR 0011
(versioning and the gate) and ADR 0012 (the second core, after this release).

## What 1.0 is

Micro OS is a small operating system of its own: a portable C11 kernel, a
syscall ABI of its own, MicroFS and FAT and an ext2 reader, a shell, and the
same sources on three machines - the M5Stack StickC Plus2 (ESP32), bare metal
x86-64 in QEMU and a Linux process. `1.0.0` is the point where the contracts
stop moving:

* the system calls keep their numbers and their meaning, and a program built
  for `1.0.0` runs on every `1.x` (the table is recorded in the tree and a
  test compares the record with the kernel on every build);
* the format of a program (MOX1 version 2) is frozen, with the machine of the
  image in its header;
* the format of a MicroFS volume is frozen; a volume of an older release
  mounts, and the release notes say what it cannot show (the times of a
  version 1 record are zero, which means "unknown");
* the contract of a port is version 1.8 and every port implements it.

## What it promises

* **A correct single-core kernel.** The scheduler, the allocator, the VFS, the
  console and the block layer are safe with one core running the kernel, on
  all three machines. The second core of the board is a project of 1.x, and
  this release does not pretend otherwise (ADR 0012).
* **A program cannot reach the kernel on x86-64.** Ring 3, page tables of its
  own, NX and W^X, `copy_from_user` and `EFAULT` at the edge of every call; a
  fault ends the program, not the shell.
* **The board runs a program too**, without an MMU: a `.mox` of Xtensa code,
  entered as a task with a door to the same system calls. There is no
  protection there, and the documentation says so in those words: a bad
  pointer or a fault in the program takes the board down.
* **A filesystem that survives a power cut** in the way the release notes
  describe: MicroFS has a check and a repair (`fsck -y`), FAT is written and
  repaired, ext2 is read.
* **One system of processes**: `fork` with copy on write on x86-64, `execve`
  with an environment, pipes and `dup2`, four signals, a shell with pipelines
  and redirections, and ten real programs built against the library of a
  program.
* **A network on both machines**: an HTTP client, a page of the state, NTP,
  MQTT and telnet on the board; its own IPv4/TCP, a server half and a DNS
  cache on x86-64. No TLS: 1.0 is a LAN system, and that is the sentence of
  the readme and the release notes.

## The session of 600 seconds

No `1.0.0` is cut before one session of one boot runs **600 seconds under a
mixed load without a bug**: x86-64 with a disk and a network, a shell that
runs programs, pipelines and redirections, `fork`, signals, files, `ENOSPC`
and a repair, timers, and the host port under TSan at the same time. The
board holds the same 600 seconds with programs, telnet, the network and the
display. A panic, a hang, a reboot, a lost wakeup, a leak or a command that
should pass and does not fails the session; a cosmetic line of the fourth
level may stay.

The campaign of 1.0 is in [the wiki of the tests](testing.md), and the tools
are `make soak`, `make soak-host`, `make board-soak`, `make fuzz-long` and
`make disk-stress`.

## What 1.0 does not have

The release notes of `1.0.0` say the same list, so a person does not have to
read the roadmap to learn it:

* **the second core**: the kernel is correct on one core; SMP is 1.x (ADR
  0012);
* **a safe update with a rollback**: a device is updated over USB with a
  backup of its volume; the update path is the first block after `1.0.0`
  (ADR 0011, updated);
* **TLS and IPv6**: 1.0 is a LAN system, without a certificate or a cloud
  account;
* **a writable ext2 and a journal under FAT**: ext2 is read, FAT is written
  without a log;
* **more than one user**: one uid, and the owner of a file is stored and
  checked but there is no a second party;
* **an ARM port with an MMU**: the first feature after the release;
* **a `progs` partition of the board**: the images of the board travel in the
  firmware and are written to `/bin` at the boot; a partition mapped as
  instructions is the next step (ADR 0018).

## Getting it

The image for the board and the bundle of x86-64 are on the releases page of
the repository. The board is written at `0x0` with `flash.sh`; the machine of
x86-64 is a loader and a flat kernel for QEMU, which boots from the disk image
the build writes. The wiki of a version is at the root of this site for the
latest stable, and under `/dev/` for the development line.

## Upgrading from 0.x

A volume of MicroFS, a `/etc` and a set of programs of a 0.x release are
described in [the migration guide](../migration.md): a version 1 record
mounts and reports no time; `/etc` and `/etc/fstab` keep their files; the
programs of `/bin` are files of the disk image, and an image built for 1.0
says its machine in the header (MOX1 version 2). The migration is tested on
the release candidate, and a volume that cannot be read is named by `fsck`
instead of being reformatted.

## The process behind the number

The gate is a document, not a feeling: every line of `docs/releases/1.0.md`
is `[x]` only when the code, the test and the run on the machine agree. A
release candidate is cut when the gate holds, only fixes are allowed after
it, and `1.0.0` follows at least two candidates and four weeks on the board
without a change of an ABI, a format or a default. That is what makes the
number mean something.
