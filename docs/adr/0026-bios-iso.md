# ADR 0026 - A bootable image of x86-64: the ISO of K13

* Status: accepted (prototype)
* Date: 2026-11-22
* Supersedes: nothing
* Related: ADR 0007 (the higher half), ADR 0008 (the disk and the
  partitions), ADR 0020 (the signed image), ADR 0016 (the programs),
  `docs/ports/x86_64/iso.md`

## Context

The x86-64 port boots in QEMU from `-kernel loader32.elf -initrd
microos64.bin`: the hypervisor reads the multiboot image, places the kernel
in the memory and describes it in the boot information. That is the machine
of the tests and of CI, and it is not a machine a person can boot. A PC, a
laptop or a virtual machine without the flags of QEMU needs a medium - and a
medium needs a boot loader that the firmware knows how to start.

The kernel is not the missing part: `loader32.elf` is a multiboot 1 image,
it copies the kernel to 16 MiB, builds the map of the first gigabyte and the
kernel at the top, and enters the 64-bit kernel. The question of K13 is only
how a medium gets those bytes to the loader, and where the filesystem of the
machine lives when the medium is a CD.

Three shapes were possible:

* a disk image with a boot sector of our own (an MBR loader that reads the
  rest of the image from the disk or from the ISO9660 of a CD);
* an ISO with a boot loader of the system (GRUB, limine, syslinux) through
  the El Torito catalogue;
* an EFI application only, which needs the graphics and the console of the
  firmware (GOP) before any of the kernel exists.

The first is a boot loader of several hundred lines of assembly with a
reader of ISO9660, a second implementation of what GRUB and limine already
are. The third is the UEFI debt of the port, not its first medium. The
second is the one a person can build on any distribution and inspect with
`xorriso -indev`.

## Decision

### 1. The medium is an El Torito ISO, BIOS first

`make x86-iso` writes `build-x86-iso/microos.iso`: the loader, the signed
kernel, the root image and the menu of the boot loader, in one ISO9660 image
with an El Torito catalogue. A BIOS reads the catalogue, starts the record
of the boot loader, and the boot loader starts `loader32.elf` as a multiboot
1 kernel and hands it the other two files as modules.

UEFI is not in this decision. The EFI application of the loader would have
to take the memory map of the firmware, set a graphics mode and leave the
screen to the kernel, and the kernel of this port draws into the text mode
of the BIOS (ADR 0023 is the framebuffer of 1.2). The ISO is BIOS-only until
the screen of the port is a linear framebuffer.

### 2. The root image is the second module

The kernel does not carry `/bin` and `/etc` in itself: they are files of a
MicroFS volume (`make x86-disk`, ADR 0008). On a CD there is no IDE disk to
put that volume on, so the volume is the second module of the boot:

* `/boot/root.img` is the image `tools/x86_image.py` writes: an MBR and a
  MicroFS volume in its first partition, with `/bin` and `/etc` already
  written by the host tool;
* the boot loader loads it after the kernel; `loader32.S` keeps the
  multiboot information and the kernel reads the module from it;
* `src/blockdev_module.c` registers the bytes of the module as a block
  device of `MFS_BLOCK_SIZE` (`boot0`), the portable parser reads its MBR
  and registers `boot0p1`, and `port_x86_64_root_device()` names that
  partition before the disk and before `ram0`;
* the heap of the port starts above the last byte of every module, so that
  a block of the allocator is never the bytes of the volume the boot is
  reading.

The device is read-only. The bytes are part of the image of the boot: a
write would last until the machine stops and could leave the volume half
written in the meantime, and a CD has no other copy. The block layer refuses
the write, MicroFS reports `EIO`, and the shell says so
(`write: /x: input/output error`). A machine that needs a writable root
mounts its disk, not this image.

### 3. The loader refuses a module over the kernel

The kernel is copied to `KERNEL_PHYS` (16 MiB) while the modules live where
the boot loader put them. A boot loader is free to place a module there; the
copy of the kernel would then destroy the root image exactly when the kernel
is about to read it. `loader32.S` walks the module table before it copies a
byte and stops with a sentence when any module intersects the destination
range. Module 0 may sit at `KERNEL_PHYS` itself: that copy is a range onto
itself.

### 4. The boot loader is not vendored

The build uses the boot loader of the system; nothing of GRUB or limine is
in this tree. `tools/x86_iso.py` prefers `grub-mkrescue` - the menu is
`multiboot /boot/loader32.elf` and one `module` for the kernel and one for
the root image - and falls back to `limine` when the machine has no GRUB
(the distribution of the machine that wrote this ADR has limine and no
GRUB). `--bootloader grub|limine` names one, and an installation without
either fails with the package to install, not with a build error of the
tree.

The GRUB path is the decided one and the limine path is the one that was
booted on this machine; the difference is the file of the menu and the
El Torito record, not the boot: the loader, the kernel and the modules are
the same three files, and the port reads the multiboot information of
either. The GRUB half is untested here (see `docs/ports/x86_64/iso.md`).

### 5. The image of the ISO is checked like the machine of QEMU

`make x86-iso-smoke` boots the ISO with `-cdrom` and nothing else: no
`-kernel`, no `-initrd`, no disk. It checks the signature of the kernel by
the loader, the mount of `boot0p1`, the program the boot runs out of `/bin`,
and the shell with `ls`, `df`, `exec /bin/hello` and `ls /`. A green smoke
test is the definition of "the ISO boots".

## Consequences

* The port has a second way in, and it is the one a person has: a medium.
* `make x86-smoke`, `make x86-disk-smoke`, `make x86-all` and `make bundle`
  are untouched: the ISO reads the same three files they build and adds a
  directory of its own (`build-x86-iso/`).
* The kernel's ABI, MicroFS, MOX1 and the board are not changed. The change
  in the kernel is one device over the memory of a module and one line in
  the choice of the root medium.
* The medium is a read-only root: a program that writes a file gets `EIO`,
  and the settings of `/etc` that the boot would create are not created
  (`settings: cannot write /etc/display` is the honest line of the boot).
* The image is not yet reproducible byte for byte, the EFI half is not
  written, and an ISO written to a USB stick does not boot as a disk. All
  three are debts of the register (`docs/debts.md`, K13).
