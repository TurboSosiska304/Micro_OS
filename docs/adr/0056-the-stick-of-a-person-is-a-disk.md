# ADR 0056 - the stick of a person is a disk, and the memory is not

* Status: accepted, implemented
* Date: 2026-11-24
* Related: ADR 0008 (the block devices), ADR 0022 (the layout of the flash),
  ADR 0037 (the second disk), ADR 0040 (the USB of the keyboard), ADR 0041
  (a disk nobody formatted), ADR 0047 (the journal of MicroFS), ADR 0050 (the
  clean end), `docs/debts.md` (K12, K13, K14)

## Context

A laptop of these years has no optical drive and a firmware of UEFI, and the
image of the release is a stick: the firmware reads
`\EFI\BOOT\BOOTX64.EFI` from a FAT partition, the loader starts the kernel,
and the machine answers on the screen of the firmware. That is enough for a
machine that only reads, and it was the shape of the first UEFI boot: the
loader also read the volume of the machine from the ESP (`disk.img`), copied
it to the memory before the kernel ran, and handed it over as a module of the
boot. The port mounted it, the table of the partitions inside it said
`boot0p1`, and the volume was mounted *read-write* - the images of the block
layer of a module have no door of the writes, so nothing refused the mount.

A write to that volume is a write to the memory. The driver answers `EIO` at
the first sector (the module's `write` is the call that says "not a medium"),
and the shell says what a person saw on the laptop:

```
microos> write /x.txt hi
write: /x.txt: input/output error
settings: cannot write /etc/display
```

The bytes of a file and a boot setting are the two places a person meets the
limits of the boot first. A machine whose root is a file in the memory is a
machine that forgets everything at the power, and its mount lied about it.

## Decision

### 1. The stick is a GPT with two partitions

```
+---------+---------------------------+--------------------------+
| 1 MiB   |  partition 1: ESP, FAT32  |  partition 2: MicroFS    |
| GPT     |  BOOTX64.EFI, kernel,     |  the volume of the       |
|         |  disk.img (the fallback)  |  machine, root at usb0p2 |
+---------+---------------------------+--------------------------+
          LBA 2048                    LBA 133120
```

The ESP is the volume the firmware reads, 64 MiB: the loader, the signed
kernel (ADR 0020) and, on purpose, the old `disk.img` of the module. The
second partition is the volume of the machine, 24 MiB, written by
`tools/x86_image.py --volume-only`: a MicroFS volume at the first byte of the
partition and no MBR of its own, because the table of the stick is the GPT and
a table inside a partition is a second one nobody reads.

The module stays for one reason: a machine whose USB controller is not brought
up still boots. It is read-only and it says so now (§4), so a person is never
told a write of the memory happened when it did not.

`make uefi-stick` writes the image (`tools/uefi_stick.py`): the ESP by the
builder of the old volume, the volume by the builder of the tree, the table by
`sfdisk`, and the two parts placed with `dd`. The image is 90 MB, boots under
OVMF as a USB storage device, and the same bytes a person writes to a stick
are the medium of this system.

### 2. The disk of the stick is the USB stack of the kernel, not the firmware

There are two ways to write a USB stick from a machine of UEFI, and only one
of them is a driver:

* **EFI_BLOCK_IO_PROTOCOL** is the firmware's own view of the stick. The
  loader can read and write it - and it is gone at `ExitBootServices`, which
  is the step before the kernel runs. A kernel that used it would have to keep
  the boot services alive for the whole session, or call back into a firmware
  whose memory map, stack and interrupts are the firmware's again; that is not
  an operating system, and it is the reason the loader's own boot services
  end where they end (step 4 of its header).
* **the USB stack of the kernel** is `src/xhci.c` and
  `src/usb_storage.c`: the controller, the Bulk-Only Transport, INQUIRY,
  READ CAPACITY, READ(10), WRITE(10) and SYNCHRONIZE CACHE. It is a driver
  that belongs to this system, it survives `ExitBootServices` because it does
  not use the firmware, and it is the one path that writes a sector of the
  medium.

The decision is the second. It also means the medium is a disk of the block
layer like the IDE channel, the SATA controller and the NVMe namespace: one
name (`usb0`), one table of the partitions (`usb0p1`, `usb0p2`), the cache of
ADR 0032, and the rule of ADR 0041 without a second copy of it.

### 3. The volume of the stick is the root, and the module is the fallback

The kernel enumerates the stick at the boot (the xHCI driver runs in
`port_x86_64_init`, before the root is chosen). The storage class scans the
table and, of its partitions, takes the one whose bytes are a volume of this
system: the ESP is FAT32 and of another system, the volume of the machine is
MicroFS and of ours. That partition is the candidate of the root; a stick of
another system has no candidate at all.

`port_x86_64_root_device()` prefers it over the module when the boot brought
both: a medium that keeps what is written wins over an image of the memory
(ADR 0041 §1 unchanged - the volume is named in `/etc/volumes`, and the stick
names its partition as `usb0p2`). The other order - module, virtio, IDE, AHCI,
NVMe, USB - is the one it was, so a machine with an ATA disk and a foreign
stick still boots the disk.

The name of the volume in the list is the partition, not the disk:
`usb0p2`. It is the name the mount and the door of the writes use, and it is
the line `mkfs` writes into `/etc/volumes` of the stick (`STICK_VOLUMES` of
`tools/uefi_stick.py`, the four disks of the port and `usb0p2`).

### 4. The door of the writes, and the module that says no

The USB driver had the write path of the Bulk-Only Transport and kept it
closed: the class registered read-only, because the rule of ADR 0041 was
written for a stick of a person that this system did not make. The stick of
this system is the case the rule has: a volume of MicroFS, named in
`/etc/volumes`. The driver grows the door the SATA controller has
(`ops->writes`): a mount that asks for the writes re-identifies the partition
and asks the rule again, and `/etc/volumes` is read at that moment, from the
volume that has just been mounted. A yes opens the flag; a no is `KERR_ROFS`
and the root stays read-only with the sentence of the refusal.

`usb_sync()` is the SCSI SYNCHRONIZE CACHE (0x35) of the stick: the command
that turns the sectors the driver wrote into sectors of the medium. The
journal of MicroFS already orders the data before the commit (ADR 0047); this
is the last step of that order, and it is what makes a file written and a
power that follows a file in the second boot.

The image of the memory grew the other half of the same truth: the module's
block device now has a door of the writes that refuses to open, so the mount
of `boot0p1` says `read-only` and a write is `EIO`/`ROFS` with a sentence
instead of a lie. The ISO, the CD and a machine whose USB did not come up
keep the module as they had it - read-only, now honestly.

### 5. The gate: the file survives the power

`make uefi-stick-smoke` (`tools/uefi_stick_smoke.py`) boots the image as a
USB stick under OVMF, writes `/persist.txt`, takes the power away by killing
the QEMU process (not a `shutdown`: a person closes a lid or the battery
goes), and boots the same bytes again. The checks are the promise:

* the firmware finds the stick, the kernel finds `usb0p2`, the mount opens
  the writes and `/proc/mounts` says `usb0p2 / mfs rw`;
* `write /persist.txt the-stick-remembers` answers with the bytes, the
  SHA-256 of the medium changes, and the second boot reads the text back;
* the volume of the stick carries `/bin/ports` - the programs of the SDK
  (`kilo` and the console set of sbase) are on the medium a person installs,
  and not only in the disk image of `make x86-disk`;
* `fsck` of the second boot says the volume is consistent;
* the negative half: a stick whose second partition is FAT32 (a volume of
  another system) is not taken for a root. The machine boots the image of the
  ESP, the root is `boot0p1` read-only, the write is refused, and the SHA-256
  of the stick is the same before and after.

Twenty-one checks, about a minute, on a machine with OVMF. It is not part
of `make ci`: it needs the firmware and QEMU's USB controller.

### 6. What it costs

| | value |
|---|---|
| ESP of the stick | 64 MiB (FAT32 minimum is 48) |
| volume of the machine | 24 MiB (the tree is about 10) |
| the stick image | 90 MB |
| the gate | 21 checks, two boots and one foreign boot, about a minute |

The volume is smaller than the 5 GiB of the disk of the release on purpose:
a stick of a person is a rescue and a demonstration of the machine, and the
tree of `/bin` fits in a tenth of it. The image of the ESP carries the module
of the fallback (16 MiB), which is the largest object of the stick.

## Consequences

**Good**

* A file written on a stick of this system is a sector of the stick: it
  survives the power, and the second boot reads it with `cat`.
* The write path is a driver of the kernel and not a protocol of the
  firmware, so the same code writes a stick on a machine whose firmware
  offers nothing after `ExitBootServices`.
* The rule of ADR 0041 is unchanged and is asked at the door of the
  controller: the volume is ours and named, or it is not written.
* A machine whose USB controller is not up boots the image of the ESP and is
  told that the root is read-only, instead of a read-write mount that fails
  at the first write.
* The image of the boot (the ISO, the CD and the ESP of a machine without a
  stick) keeps working: the module is still the root when there is nothing
  better.

**Bad**

* The stick is written by a tool of the host and the whole table is the
  tool's: a person's stick with files is erased, and there is no installer
  that adds a partition beside them (K12).
* The fallback image in the ESP is a copy of the volume at the moment of the
  build. A machine that boots it after the stick's partition was written is a
  volume of an earlier day, read-only, and the log of the boot says which
  root it has. It is a rescue and not a root a person should work in.
* The volume of the stick is 24 MiB by default: enough for the tree and a
  file, not for a download of a hundred megabytes. `--volume-mb` is the
  tool's answer, and a bigger stick is a format away.
* The electromagnetic honesty of the gate is QEMU's: the firmware's USB
  controller of the emulator brings the stick up, and the xHCI of the laptop
  of the report did not ("port N is not enabled after its reset"). Until the
  lane of the USB finds that defect the gate proves the stack and not the
  laptop, and the laptop still boots the read-only image.
