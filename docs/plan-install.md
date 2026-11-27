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

The target is one partition of 5-10 GiB on an NVMe disk that already holds
another operating system. The other system has its own partitions, its own boot
loader and, on a UEFI machine, its own EFI System Partition. The installer is a
guest on that disk: it may change only what the person names, and it must leave
every other byte as it found it.

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

1. **Named target only.** Every write goes to the partition the person named by
   device and partition path, for example `/dev/nvme0p1`. The installer never
   writes to a whole-disk node except the single GPT entry it adds or modifies,
   and that entry write is bounded to the partition table area.
2. **No whole-disk format.** `mkfs`, `mfs_format` and partition-table creation
   run only on the named partition or on a newly created partition table entry.
   The installer refuses `nvme0`, `sda`, or any node that is not a partition when
   a partition is required.
3. **No silent ESP rewrite.** On UEFI, the installer may write only
   `\EFI\MICROOS\` (or the agreed vendor directory) inside an existing ESP. It
   must not replace `\EFI\BOOT\BOOTX64.EFI`, must not delete another vendor
   directory, and must not change BootOrder without recording the previous value
   and showing the exact change. The default is "copy files into ESP, do not
   change NVRAM"; NVRAM changes are an explicit step.
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

### Recommendation

**Default root: ext2. Fallback/experimental: MicroFS v4. FAT32 is not a root.**

Ext2 is the right default for a 5-10 GiB installed root on NVMe. The kernel
already mounts it, writes it, checks it, and repairs it. Its on-disk format is
stable, sized for this volume class, and readable by common boot loaders. The
cost is explicit: ext2 has no journal, so a power cut can lose the last write
and `fsck` is the recovery path. The plan accepts that cost for the first disk
install and makes `fsync`/write barriers and the journal question visible in
the non-goals and follow-up work.

MicroFS v4 is attractive because ADR 0047 gives it a metadata journal and the
"whole or absent" file promise. It remains an option for small installs, but it
is not the default for 5-10 GiB NVMe roots in this release: its 1024-byte block
format, image tooling, and boot-loader story are tuned to small system volumes,
not to a general installed disk. A MicroFS install path may be kept behind an
explicit `--fs=mfs` once measured.

FAT32 is rejected as `/`. It cannot represent owners, groups, modes, or the Unix
metadata the user/session model expects. It remains the right filesystem for the
ESP and for exchange volumes.

The plan therefore treats "installed root filesystem" as a parameter with ext2
default, records the choice in `/etc/fstab` or an equivalent boot/config record,
and requires `k_root_mount()` to identify the installed volume by bytes, not by
a hard-coded device name.

## 3. Boot

The installer must make the machine boot Micro OS without damaging the boot path
of the existing OS. UEFI and BIOS are different contracts; UEFI is the primary
target for this release because the target machines named by the owner are NVMe
laptops/PCs.

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
* NVRAM Boot#### creation is separate from file copy. The default mode copies
  files and prints the firmware command to add a boot entry. An explicit
  `--boot-entry` mode may create a Micro OS entry with `efibootmgr`-equivalent
  logic, but it must save the old BootOrder, append the new entry, and never
  remove existing entries.
* Secure Boot is out of scope for this release unless the loader and kernel are
  signed by a key the firmware trusts. The plan reports this as a preflight
  failure, not as a silent fallback.

The ESP is mounted read-write only for the duration of the copy and sync. The
copy is atomic at file granularity: write to a temporary name in
`\EFI\MICROOS\`, flush, rename into place, flush the directory, then sync the
ESP volume.

### BIOS

BIOS install is possible only as a constrained path. A classic MBR disk cannot be
safely shared with an unknown foreign boot layout by writing boot code into
sector 0. The installer therefore does not write the MBR bootstrap on a disk
that already contains a foreign boot sector unless the owner explicitly selects
an advanced mode and the backup/restore path is proven.

The safe BIOS options are:

* install Micro OS as a second boot entry in an existing boot manager, if one is
  detected and supported;
* install the loader into the Micro OS partition's boot area/VBR only when the
  partition is booted by an existing chainloader and the MBR is untouched;
* for the first release, prefer UEFI and mark BIOS-on-shared-disk as "manual,
  advanced, not default".

If limine is used for BIOS, `limine bios-install` is allowed only on a disk
image or on a disk explicitly designated as Micro OS-owned in a destructive mode
that this plan does not grant by default. Dual-boot BIOS on a foreign MBR is not
a release gate.

## 4. The steps of the installer

The installer is a shell command, `install`, or a small user program with the
same contract. It runs from the live ISO/stick. It does not run from the target
disk.

### 4.1 Discover

* Enumerate block devices and partitions: `nvme0`, `nvme0p1`, ..., `ahci0`,
  `usb0`, `boot0`. Use the kernel blockdev/partition view, not a hand-rolled
  parser over a cached string.
* Read GPT/MBR tables and print a table: device, size, type, filesystem guess,
  label/UUID if known, mount point, and flags: ESP, foreign system, Micro OS,
  blank, unknown.
* Read UEFI boot entries when available; otherwise report "boot entries not
  readable".

### 4.2 Select

* Require a target partition name. The installer accepts no default target.
* Require a confirmation word that includes the target name, for example
  `INSTALL nvme0p3`. `--yes` is not enough without the named target; a
  non-interactive mode requires both `--target nvme0p3 --yes --confirm nvme0p3`.
* Refuse dangerous targets:
  - the whole disk;
  - the ESP;
  - a partition currently mounted as `/` or carrying the live medium;
  - a partition whose type/label/filesystem identifies it as a foreign system
    root unless the owner passes an explicit override flag that is off by
    default and prints a red warning;
  - a partition smaller than the minimum size or larger than the requested
    policy without acknowledgement.

### 4.3 Plan

Produce a dry-run plan before any write:

```
target: nvme0p3 (8 GiB, GPT type ..., currently ext4, label "data")
action: format as ext2, mount as /, install Micro OS 2.3
boot:   copy \EFI\MICROOS\BOOTX64.EFI to ESP nvme0p1, no NVRAM change
source: /boot/kernel, /boot/loader, /bin, /etc, /lib if present
risk:   nvme0p3 will be erased; nvme0p1 ESP will gain \EFI\MICROOS\
```

The plan is machine-readable in a log and human-readable on the console. A dry
run exits after printing it.

### 4.4 Prepare

* Re-read the partition table and verify the target still matches the plan
  (size, UUID, type, device path). If it changed, abort.
* Unmount the target if mounted.
* Check free space in ESP for the boot files.
* Check that the source image contains every required artifact and that their
  SHA-256 manifest is present and signed.

### 4.5 Format and copy

* Create the filesystem on the named partition. The default is
  `mkfs.ext2`-equivalent logic in the installer tool path, or a host-built image
  written as a filesystem only when safe and exact; prefer making the filesystem
  on the target and copying files so the partition UUID and size are correct.
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

### 4.6 Boot loader

* Mount the ESP read-write.
* Copy boot files into `\EFI\MICROOS\`.
* Sync the ESP.
* If NVRAM update was requested, create the entry and print old/new BootOrder.
* If BIOS mode was explicitly selected and safe, install the selected stage into
  the agreed location and record the previous bytes for rollback.

### 4.7 Verify

* Sync the target filesystem.
* Run `fsck` in read-only mode on the installed volume.
* Mount it read-only and verify required paths exist.
* Verify SHA-256 of copied boot artifacts and a manifest of critical files.
* Print the result: installed, verified, boot entry state, and how to boot the
  old OS.

### 4.8 Finish

* Unmount target and ESP.
* Offer reboot only after an explicit confirmation. The default is to return to
  the shell with the next command printed.

## 5. Safety and rollback

The installer has one job more important than installing: not damaging the other
system.

### Before any write

* Dry-run by default.
* Print the full plan and require the target name as confirmation.
* Read and save the GPT/MBR entries relevant to the target and ESP.
* Save the ESP Micro OS directory state if it exists.
* Save NVRAM boot order if it will be changed.
* Verify source manifest signatures/hashes. Unsigned or mismatched source
  artifacts abort the install before partition changes.

### During writes

* Format only the named target partition.
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
2. Write a GPT with an ESP and a blank target partition, or a GPT with an
   existing foreign partition and free space for Micro OS.
3. Boot the Micro OS ISO in QEMU with the disk attached.
4. Run `install --dry-run`; verify no writes.
5. Run `install --target ... --yes --confirm ...`; verify only the named
   partition and `\EFI\MICROOS\` changed.
6. Shut down, boot QEMU from the installed disk (OVMF, no ISO).
7. Run an `x86-smoke`-like installed check: root mounted read-write, `/bin`
   present, `fsck` clean, `uname`, file write/read survives reboot, network not
   required.

The gate compares SHA-256 of untouched partitions before and after install. It
also boots a disk that contains a fake foreign ESP entry and proves it still
exists.

### Owner NVMe checklist

Only after the QEMU gate:

1. **Backup**: confirm the owner has a current backup or accepts the risk.
2. **Identify disks**: photograph or save `lsblk`/firmware disk list; confirm the
   NVMe device, ESP, and target partition by size and UUID.
3. **Dry-run**: run installer dry-run and read the plan aloud; abort on any
   unexpected target.
4. **Install**: install only to the named 5-10 GiB partition; keep NVRAM change
   explicit.
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
* No unsafe BIOS MBR overwrite on a disk that already boots another OS.
* No resize of foreign partitions in the default path; partition creation is
  allowed only in explicitly free space and after the same dry-run/confirm gate.
* No support for installing from an already installed Micro OS root in this
  first cut; the live ISO/stick is the supported source.

## 8. Questions for the owner

1. **Release identity**: should this be 2.3 with a new name/plan, or should it
   take a different number because 2.3 is already "Pompeii" in
   `docs/release-names.md`?
2. **Default filesystem**: ext2 by default for 5-10 GiB NVMe, with MicroFS v4 as
   an explicit option? Or do you want MicroFS first because of the journal?
3. **Boot loader policy**: copy `\EFI\MICROOS\BOOTX64.EFI` only, or also create an
   NVRAM Boot#### entry by default?
4. **BIOS scope**: UEFI-only for the first release, or do you want a constrained
   BIOS path for old PCs?
5. **Partitioning**: may the installer create a new partition in free space, or
   must the owner create the 5-10 GiB partition beforehand?

## 9. Work lanes

The lanes are ordered by dependency. A lane is done only when its gate is green;
no lane may claim "works on my NVMe" as evidence.

* **L0 Spec** (docs, 1d): freeze the ADR - target rules, ESP rules, fs default,
  boot-entry policy, non-goals. Gate: ADR reviewed; owner answers the five questions.
* **L1 Inventory** (kernel, 2d): stable partition inventory - GPT/MBR, type, size,
  ESP flag, filesystem guess, mount state. Gate: host tests; QEMU mixed disk listed.
* **L2 Dry-run** (installer, 2d): `install --dry-run` validates target, refuses
  dangerous cases, prints the plan, writes nothing. Gate: no sector writes in tests.
* **L3 Format** (fs/tools, 3d): create the root fs, `/etc/volumes`, install record.
  Gate: volume mounts; `fsck` clean; required tree present.
* **L4 Copy** (installer, 2d): copy tree with manifest, SHA-256, sync, read-only
  verify. Gate: QEMU install passes; corrupted copy fails.
* **L5 UEFI boot** (boot, 3d): mount ESP, copy `\EFI\MICROOS\`, optional NVRAM entry;
  never touch foreign boot files. Gate: OVMF boots; foreign ESP unchanged.
* **L6 Root boot** (kernel, 3d): boot the installed disk as a normal writable root,
  without regressing live ISO/USB boot. Gate: rw root; file survives reboot.
* **L7 QEMU gate** (qa, 2d): `x86-install-smoke` - 8 GiB disk, install, boot,
  smoke subset, untouched-partition SHA-256. Gate: green in QEMU CI.
* **L8 NVMe checklist** (docs/owner, 1d): owner dry-run, backup, identify disk,
  return path. Gate: owner accepts plan; old OS boots after install.

### Estimate

The estimate is in focused engineering lanes, not calendar promises:

* L0-L2: 1 lane-week - contract, inventory, dry-run planner.
* L3-L4: 1-2 lane-weeks - filesystem creation, copy, manifests, fsck.
* L5-L6: 1-2 lane-weeks - UEFI boot install and installed-root boot selection.
* L7-L8: 1 lane-week - QEMU gate, docs, owner checklist.

The critical risk is boot selection: the current x86 root selection prefers the
boot module in several live-medium cases, and an installed disk must be booted
without regressing ISO/USB behaviour. That risk belongs to L6 and blocks the
owner gate.
