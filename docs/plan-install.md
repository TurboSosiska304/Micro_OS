# The plan of the install: a system on the disk of a person

> The first release that puts the machine on the disk it is going to live on:
> a partition of 5-10 GiB on the NVMe of a laptop or a PC, beside a system that
> is already there, with its own boot loader and its own ESP. The stick boots
> the machine; this release makes the machine stay.

* Status: draft, a plan before the code
* Date: 2026-11-27
* Related: ADR 0008 (block devices), ADR 0026 (the ISO), ADR 0027 (the root is a
  fact of the medium), ADR 0037 (the firmware boot), ADR 0041 (a disk nobody
  formatted), ADR 0042 (NVMe), ADR 0047 (the journal of MicroFS), ADR 0056 (the
  stick of a person is a disk), `docs/debts.md` (K12, K13, K14, the USB-stick
  row), `docs/install.md`

The name `2.3` is the owner's working name for this release. `docs/release-names.md`
already reserves 2.3.0 "Pompeii" for the fault-injection release and
`docs/plan-2.3.md` describes that slice; the number and the name of the disk
install are an owner decision recorded in the questions at the end.

## 1. What "install" means for this machine

The target is a region of 5-10 GiB that the owner leaves **unpartitioned** on an
NVMe disk that already holds another operating system. The other system has its
own partitions, its own boot loader and, on a UEFI machine, its own EFI System
Partition. The installer is a guest on that disk: it appends one partition entry
in that gap, formats that partition, and leaves every other byte as it found it.

The install is complete when all of these are true:

* a GPT partition of the requested size exists on the chosen NVMe disk, with a
  type GUID that names a Linux data partition or a Micro OS partition type agreed
  in the ADR of the implementation; the partition is the only one the installer
  created or formatted;
* the partition contains a volume of the chosen filesystem with the tree of the
  system: the kernel, the loader or boot files the boot path needs, `/bin`,
  `/etc`, the empty mount points the boot expects, and `/etc/volumes` naming the
  installed volume;
* a boot entry starts Micro OS from that volume without removing or rewriting the
  boot entry of the other system;
* the machine boots from the installed volume, `fsck` reports a consistent
  volume, `/bin` runs, and a file written after the first boot survives a reset.

The install is **not** a copy of the live medium into RAM and not a persistent
overlay over the ISO. The ISO and the USB stick remain boot media; the installed
NVMe volume is the root that `k_root_mount()` mounts as a normal block device.

### The invariants

These are not conveniences; they are the safety contract of the installer.

1. **One appended entry, in a gap it verified.** Every write goes either to that
   new partition or to the two GPT copies. The installer never moves, resizes or
   deletes an entry, and the table write is bounded to the table area.
2. **No whole-disk format.** `mkfs` and `mfs_format` run only on the partition
   the installer created and verified; it is the only partition it ever formats.
3. **No silent ESP rewrite.** On UEFI, the installer may write only
   `\EFI\MICROOS\` (or the agreed vendor directory) inside an existing ESP. It
   must not replace `\EFI\BOOT\BOOTX64.EFI`, must not delete another vendor
   directory, and must not change BootOrder without recording the previous value
   and showing the exact change. The NVRAM entry is always a separate question
   with no default answer; `--nvram=yes|no` is required when nobody is there to
   answer it.
4. **The other OS stays bootable.** Before any write, the installer records the
   current partition table, ESP directory listing relevant to boot, and NVRAM
   boot entries it can read. After the install it prints the command or menu
   action that returns to the other system. A failed install cannot leave the
   other OS unbootable by an action the installer took outside the named target
   and the agreed ESP directory.
5. **Dry-run is the default.** A run without `--yes` and without the explicit
   target confirmation performs no write. It prints the plan: disk, partition,
   filesystem, bootloader destination, files to copy, and the exact commands or
   writes that would happen.

## 2. The filesystem of the installation

The root filesystem is the most visible decision because it decides what
survives a power cut, what the boot loader can read, and what `k_root_mount()`
already accepts. The candidates are the filesystems this tree already has:
MicroFS, ext2 and FAT32.

| Property | MicroFS v4 | ext2 | FAT32 |
|---|---|---|---|
| Write support | yes | yes | yes |
| Mount as `/` | yes | yes, probed by `k_root_mount()` | possible, wrong model |
| Power-cut story | metadata journal; whole/absent | no journal; fsck | no journal |
| `fsck` | `mfs_check` / repair | `ext2_check` / repair, e2fsck | safe cases only |
| POSIX metadata | uid, gid, mode, times | uid, gid, mode, links, times | no uid/gid/mode |
| Boot readability | loader must read MFS or modules | ext2 readers common | easy for firmware |
| Fit for 5-10 GiB | small-volume format | good fit | exchange/ESP only |
| ADR 0041 identity | native "ours" | writable by rule, not "ours" | poor trusted-root fit |

### The choice of the owner

**The installer offers both ext2 and MicroFS v4 and asks; ext2 is the default
answer. FAT32 is not a root.**

The owner decided that the filesystem is a decision of the person installing,
not of the build: the installer presents the two candidates with their cost in
one line each, and the interactive answer defaults to ext2. A non-interactive
install names the filesystem explicitly (`--fs=ext2` or `--fs=mfs`) and there
is no silent default there - a script that forgets the flag is a script that
must be fixed, not one that gets ext2 by luck.

Ext2 is the default because the kernel already mounts it, writes it, checks it,
and repairs it; its on-disk format is stable, sized for this volume class, and
readable by common boot loaders. The cost is explicit: no journal, so a power
cut can lose the last write and `fsck` is the recovery path.

MicroFS v4 is a first-class choice because ADR 0047 gives it a metadata journal
and the "whole or absent" file promise. What it costs is stated where it is
chosen: its 1024-byte block format, image tooling, and boot-loader story are
tuned to small system volumes, and a 5-10 GiB volume on it has not been
measured yet - the installer therefore warns on a volume above the measured
size and says that a first install of that shape is a test, not a daily root.

FAT32 is rejected as `/`. It cannot represent owners, groups, modes, or the Unix
metadata the user/session model expects. It remains the right filesystem for the
ESP and for exchange volumes.

The plan therefore treats "installed root filesystem" as a parameter with no
hidden value, records the choice in `/etc/fstab` or an equivalent boot/config
record, and requires `k_root_mount()` to identify the installed volume by bytes,
not by a hard-coded device name.

## 3. Boot

The installer must make the machine boot Micro OS without damaging the boot path
of the existing OS. **UEFI is the only boot path of this release** - the owner
decided that a foreign MBR is not worth touching for the first install, so BIOS
is a non-goal (section 7) and not a constrained mode.

### UEFI

The installed machine boots through an EFI application. The source of truth for
that application is the existing UEFI loader built by the x86 port, not a new
boot protocol invented by the installer.

The install lays out boot files as:

* `\EFI\MICROOS\BOOTX64.EFI` - the Micro OS loader, copied into the existing ESP;
* `\EFI\MICROOS\microos64.bin` or the signed kernel name used by the loader;
* `\EFI\MICROOS\limine.conf` or a small loader config only if the selected loader
  needs one; the preferred path is our own UEFI loader with an explicit kernel
  path and root identifier;
* optionally `\EFI\MICROOS\recovery\` files for rollback of the boot directory.

Rules:

* The installer never overwrites `\EFI\BOOT\BOOTX64.EFI`. That path belongs to the
  removable-media/fallback contract and may be owned by another OS or firmware
  policy.
* The installer never deletes or renames another vendor directory under `\EFI\`.
* **The NVRAM entry is always asked, never assumed.** The installer copies the
  files, then asks a separate yes/no question with the detected entries and the
  current `BootOrder` printed above it: "add a Micro OS entry at the front of
  the boot order?". There is no default answer in the prompt and no flag that
  turns the question off; a non-interactive install must say `--nvram=yes` or
  `--nvram=no`, and saying neither is an error. When the answer is yes, the
  installer saves the old `BootOrder` to a file on the target and in the log,
  appends the new entry, and never removes an existing one. When the answer is
  no, it prints the exact firmware command or menu path that would add it later.
* Secure Boot is out of scope for this release unless the loader and kernel are
  signed by a key the firmware trusts. The plan reports this as a preflight
  failure, not as a silent fallback.

The ESP is mounted read-write only for the duration of the copy and sync. The
copy is atomic at file granularity: write to a temporary name in
`\EFI\MICROOS\`, flush, rename into place, flush the directory, then sync the
ESP volume.

### BIOS

Not in this release. A classic MBR disk cannot be safely shared with an unknown
foreign boot layout by writing boot code into sector 0, and the owner chose the
clean contract over a constrained mode: the installer refuses to start on a
machine that did not boot it through UEFI, prints why, and changes nothing. A
BIOS path returns only with its own plan and its own gate on a disk image whose
MBR it owns entirely.

## 4. The steps of the installer

The installer is a shell command, `install`, or a small user program with the
same contract. It runs from the live ISO/stick. It does not run from the target
disk.

### 4.2 Discover

* Enumerate block devices and partitions: `nvme0`, `nvme0p1`, ..., `ahci0`,
  `usb0`, `boot0`. Use the kernel blockdev/partition view, not a hand-rolled
  parser over a cached string.
* Read GPT/MBR tables and print a table: device, size, type, filesystem guess,
  label/UUID if known, mount point, and flags: ESP, foreign system, Micro OS,
  blank, unknown.
* Read UEFI boot entries when available; otherwise report "boot entries not
  readable".

### 4.3 Select

* Require the disk and the size. The target is `--disk nvme0 --size 8G`; there is
  no default disk and no "pick the biggest one" behaviour, because both target
  machines have a foreign system on the same NVMe.
* Require a confirmation line typed by hand that repeats both, for example
  `INSTALL nvme0 8G`. `--yes` alone is not a confirmation; a non-interactive
  install must pass `--confirm "INSTALL nvme0 8G"` literally.
* Refuse dangerous disks:
  - the disk that carries the running live medium or any mounted filesystem of
    the running system;
  - a disk whose GPT is damaged, whose backup copy is missing, or that carries a
    hybrid MBR;
  - a disk without an ESP, or whose ESP has less free space than the boot files
    need;
  - a disk with no gap of the requested size, or whose largest gap is smaller
    than 5 GiB;
  - a disk that another installer or partitioning tool has touched in this boot
    (detected by comparing the table read at discovery with the one read again
    at prepare).
* An install onto an existing partition is **not** offered in this release. The
  installer only writes into space that no entry claims, which removes the
  question "is this partition free enough to erase" from the program entirely.

### 4.4 Plan

Produce a dry-run plan before any write:

```
disk:   nvme0 (476.9 GiB, GPT, 4 entries, ESP nvme0p1 512 MiB, 104 MiB free)
space:  gap at sector 1000215216, 8.0 GiB free, aligned, unclaimed by any entry
action: append entry "MICROOS_ROOT" (Linux data GUID) of 8.0 GiB,
        format as ext2, mount as /, install Micro OS 2.3
boot:   copy \EFI\MICROOS\BOOTX64.EFI to ESP nvme0p1; the NVRAM entry is asked
        after the copy
source: /boot/kernel, /boot/loader, /bin, /etc, /lib if present
risk:   8.0 GiB of unpartitioned space becomes a partition; nvme0p1 gains
        \EFI\MICROOS\; no existing entry is modified
```

The plan is machine-readable in a log and human-readable on the console. A dry
run exits after printing it.

### 4.5 Prepare

* Re-read the partition table and verify the gap still matches the plan (start
  sector, free length, both GPT copies, no new entry). If anything moved, abort:
  the disk changed under the plan, and the plan is what the person confirmed.
* Save both GPT copies and a hash of the table without the new entry.
* Check free space in the ESP for the boot files.
* Check that the source image contains every required artifact and that their
  SHA-256 manifest is present and signed.

### 4.6 The partition, the filesystem and the copy

* Append the entry to both GPT copies, flush, re-read, and verify: the new entry
  is there, and every other entry is byte-identical to the saved table. On a
  mismatch, restore the saved copies and stop before anything is formatted.
* Create the filesystem on the new partition. The installer makes it on the
  target rather than writing a host-built image, so the partition UUID and the
  size are the real ones; the choice of ext2 or MicroFS v4 is the one made in
  section 2. A failure leaves the entry in place and says so - removing it is a
  separate, explicit step, not a silent rollback of the table.
* Mount it at a temporary mount point, for example `/mnt/target`.
* Copy the system tree:
  - kernel and boot artifacts;
  - `/bin` and required runtime directories;
  - `/etc` defaults, without overwriting user data because the target is newly
    formatted;
  - `/dev`, `/proc`, `/tmp`, `/mnt` as empty mount points where needed;
  - `/etc/volumes` with the installed partition name.
* Write an install record: `/etc/install` or `/var/lib/microos/install` with
  version, source hash, target UUID, date, and boot mode.

### 4.7 Boot loader

* Mount the ESP read-write.
* Copy boot files into `\EFI\MICROOS\`.
* Sync the ESP.
* Ask the NVRAM question (section 3), then either create the entry and print
  the old and the new `BootOrder`, or print the path that would add it.
* BIOS is not an option here: an install that did not start through UEFI never
  reaches this step.

### 4.8 Verify

* Sync the target filesystem.
* Run `fsck` in read-only mode on the installed volume.
* Mount it read-only and verify required paths exist.
* Verify SHA-256 of copied boot artifacts and a manifest of critical files.
* Print the result: installed, verified, boot entry state, and how to boot the
  old OS.

### 4.9 Finish

* Unmount target and ESP.
* Offer reboot only after an explicit confirmation. The default is to return to
  the shell with the next command printed.

## 5. Safety and rollback

The installer has one job more important than installing: not damaging the other
system.

### Before any write

* Dry-run by default.
* Print the full plan and require the disk and the size, typed by hand, as the
  confirmation.
* Read and save both GPT copies before the entry is appended, and a copy of the
  ESP directory state.
* Save the ESP Micro OS directory state if it exists.
* Save NVRAM boot order if it will be changed.
* Verify source manifest signatures/hashes. Unsigned or mismatched source
  artifacts abort the install before partition changes.

### During writes

* Append one GPT entry in a verified gap; format only that new partition.
* Write ESP files only under `\EFI\MICROOS\`.
* Sync after each critical stage: partition table change, filesystem create,
  file copy, boot file rename, NVRAM update.
* Keep a small on-disk install journal in the live medium's RAM or in a
  temporary file on a writable medium; it records completed steps so the
  operator knows what happened after a failure.

### After copy

* `fsck` the target volume.
* Verify a file manifest for kernel, loader, and critical `/bin`/`/etc` files.
* Verify that the ESP contains the expected Micro OS files and that
  `\EFI\BOOT\BOOTX64.EFI` was not modified unless the operator explicitly chose
  that advanced mode.

### Rollback rules

* If preflight fails, no write is performed.
* If formatting fails, abort; do not touch ESP or NVRAM.
* If file copy fails, leave the target partition marked as incomplete and do not
  install a boot entry. The old OS is unaffected.
* If ESP copy fails after target copy, report "system files copied, bootloader
  not installed"; do not change NVRAM. The old OS still boots.
* If NVRAM update fails after ESP copy, keep the copied files, restore previous
  BootOrder if possible, and print manual firmware instructions.
* The installer never "fixes" a foreign boot problem it did not cause.

### User-visible warnings

The plan must always print:

* which disk and partition will be erased;
* that other partitions will not be touched;
* that Secure Boot may block unsigned boot;
* how to boot the previous OS from firmware menu;
* where the install log and rollback record were saved.

## 6. The gate

The release is not allowed to touch the owner's NVMe until the image gate is
green.

### QEMU disk gate

Create an 8 GiB raw disk image:

1. `qemu-img create -f raw disk.img 8G`.
2. Write a GPT with an ESP, one foreign partition with known content, and an
   unpartitioned gap of 5 GiB - the shape of the owner's disk.
3. Boot the Micro OS ISO in QEMU with the disk attached.
4. Run `install --dry-run --disk nvme0 --size 5G --fs ext2`; verify no writes.
5. Run `install --disk nvme0 --size 5G --fs ext2 --confirm "INSTALL nvme0 5G"`;
   verify that one entry was appended, that the foreign entry is byte-identical,
   and that `\EFI\MICROOS\` is the only new thing in the ESP. Run the gate twice:
   once answering the NVRAM question yes, once no.
6. Shut down, boot QEMU from the installed disk (OVMF, no ISO).
7. Run an `x86-smoke`-like installed check: root mounted read-write, `/bin`
   present, `fsck` clean, `uname`, file write/read survives reboot, network not
   required.

The gate compares SHA-256 of untouched partitions before and after install, and
a hash of the partition table minus the appended entry. It also boots a disk
that contains a fake foreign ESP entry and proves it still exists. A damaged
backup GPT, a gap that is too small, and a disk with no ESP are three refusals;
the gate has a case for each.

### Owner NVMe checklist

Only after the QEMU gate:

1. **Backup**: confirm the owner has a current backup or accepts the risk.
2. **Identify disks**: photograph or save `lsblk`/firmware disk list; confirm the
   NVMe device, ESP, and target partition by size and UUID.
3. **Dry-run**: run installer dry-run and read the plan aloud; abort on any
   unexpected target.
4. **Install**: name the disk and the size, read the gap the installer found,
   and answer the NVRAM question yourself.
5. **Verify boot**: boot Micro OS; then reboot into firmware menu and boot the
   previous OS.
6. **Return path**: record the exact firmware menu entry and, if NVRAM changed,
   the command to restore BootOrder.

## 7. What is not in 2.3

* No disk encryption, no LUKS, no TPM key management.
* No LVM, no RAID, no btrfs/zfs, no multi-disk root.
* No swap partition and no hibernation image.
* No automatic update of an installed system; updates belong to the 3.0 line.
* No graphical installer.
* No unattended install over the network.
* No secure-boot key enrollment.
* No BIOS at all: the installer refuses a machine that did not boot it through
  UEFI, because a foreign MBR is not worth touching for the first install.
* No resize or move of a foreign partition, ever. The only table change is one
  appended entry in space no entry claims.
* No install onto an existing partition: only into unpartitioned space, so the
  installer never has to judge whether a partition "looks free".
* No support for installing from an already installed Micro OS root in this
  first cut; the live ISO/stick is the supported source.

## 8. The decisions of the owner

The five questions are answered, and the answer is the contract above.

1. **Release identity**: this is **2.3 "Pompeii"**, and the install on a disk is
   its main theme; the fault injection that already landed is part of the same
   release.
2. **Filesystem**: the installer **offers ext2 and MicroFS v4 and asks**; ext2 is
   the default answer, and a non-interactive install must name one.
3. **Boot entry**: the NVRAM entry is **always asked** - no default answer, no
   flag that skips the question silently.
4. **BIOS**: **UEFI only** in this release.
5. **Partitioning**: the owner leaves an **unpartitioned region of 5-10 GiB**; the
   installer creates the partition itself, in that gap, and formats only it.

## 9. Work lanes

The lanes are ordered by dependency. A lane is done only when its gate is green;
no lane may claim "works on my NVMe" as evidence.

* **L0 Spec** (docs, 1d): freeze the ADR - target rules, ESP rules, fs default,
  boot-entry policy, non-goals. Gate: ADR reviewed; owner answers the five questions.
* **L1 Inventory** (kernel, 2d): stable partition inventory - GPT/MBR, type, size,
  ESP flag, filesystem guess, mount state, **free gaps with alignment**. Gate:
  host tests; QEMU mixed disk listed with its gaps.
* **L2 Dry-run** (installer, 2d): `install --dry-run --disk --size [--fs]`
  validates the gap, refuses the dangerous cases, prints the plan, writes nothing.
  Gate: no sector writes in tests; every refusal has a test.
* **L2b GPT append** (kernel/tools, 2d): append one entry to both GPT copies, CRC
  both, verify the untouched entries by hash, restore the saved copies on a
  mismatch. Gate: host tests on synthetic tables (valid, damaged backup, hybrid
  MBR, gap too small, gap unaligned); QEMU: foreign entries identical before and
  after.
* **L3 Format** (fs/tools, 3d): create the root fs (ext2 and MicroFS v4), the
  `/etc/volumes`, install record. Gate: both volumes mount; `fsck`/`mfs_check`
  clean; required tree present.
* **L4 Copy** (installer, 2d): copy tree with manifest, SHA-256, sync, read-only
  verify. Gate: QEMU install passes; corrupted copy fails.
* **L5 UEFI boot** (boot, 3d): mount ESP, copy `\EFI\MICROOS\`, ask the NVRAM
  question; never touch foreign boot files. Gate: OVMF boots with the entry the
  question added and without it; foreign ESP byte-identical.
* **L6 Root boot** (kernel, 3d): boot the installed disk as a normal writable root,
  without regressing live ISO/USB boot. Gate: rw root; file survives reboot.
* **L7 QEMU gate** (qa, 2d): `x86-install-smoke` - 8 GiB disk, install, boot,
  smoke subset, untouched-partition SHA-256. Gate: green in QEMU CI.
* **L8 NVMe checklist** (docs/owner, 1d): owner dry-run, backup, identify disk,
  return path. Gate: owner accepts plan; old OS boots after install.

### Estimate

The estimate is in focused engineering lanes, not calendar promises:

* L0-L2b: 1-2 lane-weeks - contract, inventory with gaps, dry-run, GPT append.
* L3-L4: 1-2 lane-weeks - filesystem creation, copy, manifests, fsck.
* L5-L6: 1-2 lane-weeks - UEFI boot install and installed-root boot selection.
* L7-L8: 1 lane-week - QEMU gate, docs, owner checklist.

The critical risk is boot selection: the current x86 root selection prefers the
boot module in several live-medium cases, and an installed disk must be booted
without regressing ISO/USB behaviour. That risk belongs to L6 and blocks the
owner gate.
