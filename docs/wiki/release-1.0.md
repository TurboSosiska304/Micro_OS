# The 1.0 release

This is the page of the first major release of Micro OS. It says what `1.0.0`
is, what it promises, what it deliberately does not, and how a machine is
updated from a 0.x release. The checklist behind it is
[the gate of 1.0](../releases/1.0.md), and the decisions are ADR 0011
(versioning and the gate) and ADR 0012 (the second core, after this release).

> This page is the record of 1.0. The board changed with `1.1.0`: two
> application slots, an update over USB with a rollback and the programs in
> the `progs` partition (ADR 0022). The current layout and the one-time move
> of a board of 1.0 are in [the notes of 1.1](../releases/1.1.md).

## What 1.0 is

Micro OS is a small operating system of its own: a portable C11 kernel, a
syscall ABI of its own, MicroFS and FAT and an ext2 reader, a shell, and the
same sources on two machines - the M5Stack StickC Plus2 (ESP32) and bare metal
x86-64 in QEMU - with the host substrate (a Linux process) that runs the tests
and the simulator, not a machine of its own. `1.0.0` is the point where the contracts
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
  both machines (and in the host substrate). The second core of the board is a project of 1.x, and
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

## The signature of an image

The x86-64 machine boots a kernel only if its trailer says who signed it
(ADR 0020). The image is the kernel and 120 bytes after it: the magic, the
size, the SHA-256 of the bytes and an Ed25519 signature of those fields. The
public key is compiled into the loader, so the image cannot bring a key of
its own; the private key is not in this tree, and a release sets it with
`MICROOS_SIGN_KEY`.

The loader checks the trailer before it copies a byte of the kernel and before
it turns the long mode on. A changed byte, a missing trailer and a signature
of another key each print `Micro OS: the image is refused: <reason>` and halt;
`make x86-smoke` proves all three, and the normal boot prints `Micro OS: image
signature ok (sha-256, ed25519)`. `tools/microos_sign sign` and `verify` are
the tools; `make bundle` signs the application of the board and leaves the
`.sig` files and `checksums.txt` beside it.

The check is a setting, not a wall. On x86-64 the CMake option
`MICRO_OS_REQUIRE_SIGNATURE` is on by default; with it off the loader still
verifies the image, prints `Micro OS: unsigned or damaged image: not
guaranteed` and loads it. `make x86-open-build` builds that policy and
`make x86-smoke` boots an unsigned and a damaged kernel on it.

The board has the same setting in its Kconfig (`MICRO_OS_REQUIRE_SIGNATURE`,
on by default). At the beginning of `app_main`, before MicroFS and the shell,
the firmware reads the factory partition: the ESP header says how long the
image is, `esp_app_desc_t` says which version it is, and the trailer in the
last 4 KiB of the partition carries the SHA-256 and the Ed25519 signature.
The public key is compiled into the firmware and the flash layout does not
change; `tools/bundle.sh` writes `micro_os.bin.sig` and `flash.sh` puts it at
`0x20F000`. With the requirement on, a missing or wrong trailer stops the
boot with `boot: the image is refused: <reason>`; with it off, the console
carries `boot: unsigned or damaged image: not guaranteed` and the board
continues. `tools/device-test.sh` proves the signed image, the warning
(`DEVICE_POLICY=unsigned`) and the stop (`DEVICE_POLICY=require-unsigned`).

**What "signed" does not mean on the board.** The ROM and the ESP-IDF
bootloader have already copied and entered the application when the check
runs, so the first instructions of a damaged image execute before the
sentence; it is an electronic check of the boot, not secure boot. A signature
in the boot chain itself means secure boot v2, which burns eFuses, or a custom
bootloader that does not fit the 28 KB bootloader partition. Neither is in
1.0. The check cannot stop a local attacker who can replace the firmware or
the checker, and the signature does not hide the image.

The anti-rollback is a second setting, `MICRO_OS_ANTIROLLBACK`, off by
default. When it is on, the board keeps the newest version it has seen in the
NVS and refuses a lower one (or prints a warning when the flag is off). It is
not the eFuse counter of secure boot: a person who can flash the board can
erase the NVS. x86-64 has no store that survives a reset, so a rollback there
is allowed and there is no flag. Section 7 of [the checklist](../releases/1.0.md)
is closed with the limits above written beside it.

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
  (ADR 0011, updated). The `MICRO_OS_ANTIROLLBACK` setting of the board
  refuses an old image, but it is not an update path and it does not put the
  old image back;
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
it, and `1.0.0` follows at least two candidates without a change of an ABI, a
format or a default. The four weeks on the board that the rule first asked
for were waived by the owner: the session of 600 seconds of a candidate is
the evidence, and it is what makes the number mean something.
