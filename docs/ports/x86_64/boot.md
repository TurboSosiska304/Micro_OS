# The medium: one image, four machines

`make x86-iso` writes one file, `build-x86-iso/microos.iso`, and that file
boots four ways: a BIOS and a firmware of UEFI, each from a CD drive and from
a USB stick written with `dd`. The CD is the machine of
[iso.md](iso.md); this page is the stick, the menu of a firmware and the
things that are not proved.

| the machine | the entry of the image it starts |
|---|---|
| BIOS, CD drive | the El Torito record of `boot/limine-bios-cd.bin`, which reads `boot/limine-bios.sys` from the image |
| BIOS, USB stick | the MBR written by `limine bios-install` (its first stage), and the same `limine-bios.sys` in the system area of the image and in the ISO9660 above it |
| UEFI, CD drive | the El Torito record of the EFI, which is the FAT image `boot/limine-uefi-cd.bin` holding `EFI/BOOT/BOOTX64.EFI` |
| UEFI, USB stick | the same FAT image as the EFI system partition of the hybrid MBR (type `0xEF`), found by the firmware itself |

The four paths meet in the same place: limine starts `boot/loader32.elf` as a
multiboot 1 image and hands it `boot/microos64.bin` and `boot/root.img` as
modules. The loader verifies the signature of the kernel, copies it to 16 MiB
and enters it; the port mounts the first partition of the second module at `/`
(ADR 0026). Nothing of the boot depends on the medium after that.

The image is 17 MB: 12 MB of the root image, 3 MB of the limine EFI image
(six architectures of EFI in one FAT, the price of the package of the system)
and the kernel and the loader.

## Write a USB stick

The image is a disk image, not a file to copy: write it to the **whole device**.

```bash
lsblk -o NAME,SIZE,MODEL,TRAN   # /dev/sdX: TRAN=usb, the size of the stick
sudo dd if=build-x86-iso/microos.iso of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

* `of=/dev/sdX` is the disk, **not** `/dev/sdX1`: a partition has no boot
  sector of its own and the write would not boot.
* Every byte of the stick is replaced by the image; pick the device by its
  size and `TRAN=usb` in `lsblk`, and check it twice.
* `conv=fsync` (and the `sync`) make the last block reach the device before
  the machine is switched off or the stick is pulled out.
* The image is 17 MB, so the rest of the stick keeps what it had. It is not a
  partition of the image and no tool extends it; the medium of the boot is
  read-only in any case, and a writable root is an installer's work (K12).
* `cp build-x86-iso/microos.iso /dev/sdX` is the same write. A tool that
  formats a FAT stick and copies the tree of the ISO into it (the "ISO mode"
  of some writers) can start the UEFI boot — the tree carries
  `EFI/BOOT/BOOTX64.EFI` for it — but not the BIOS boot, which needs the boot
  sector of the image: write the image itself.

The same stick still boots from a CD if it is burned to one: the records of
El Torito are in the same bytes.

## The boot menu of the machine

A machine of UEFI offers the stick twice, and the two entries are not the
same boot:

* **`UEFI: <the stick>`** (or `UEFI OS`, or the model of the stick) is the
  firmware reading the ESP of the medium and starting `EFI/BOOT/BOOTX64.EFI`.
  This is the one the image is built for.
* **the bare name of the stick** is the compatibility path (CSM, "legacy
  boot"), which runs the MBR: it boots too, and it is the same system, but it
  is the BIOS half of the image and it draws into the text mode of the VGA.

The key that opens the menu is `F12`, `F11`, `F8` or `Esc` at the logo of the
firmware, depending on the machine; `Esc` then a menu, or the "boot override"
of the BIOS setup, is the same list. A machine that boots the CD drive has the
same two entries for the drive.

### Secure Boot has to be off

The firmware verifies the signature of the EFI application against the keys
it holds, and `BOOTX64.EFI` of limine is not signed by Microsoft or by the
keys of the machine. With Secure Boot on, the firmware refuses the image
("Security Violation", "not authorized", or a return to the menu). Disable it
in the setup of the firmware:

* `Security` → `Secure Boot` → `Disabled`, then save and exit (some machines
  also want `Setup Mode` or a "clear the keys" step before the setting is
  editable);
* or delete the keys in `Key Management` (`Clear All Secure Boot Keys`).

The signature the image *does* carry is the port's own: the loader verifies
the SHA-256 and the Ed25519 trailer of the kernel before it copies a byte
(ADR 0020). It is not a signature of the firmware chain, and it does not
satisfy Secure Boot.

## Build and check

```bash
make x86-iso             # build-x86-iso/microos.iso, 17 MB
make x86-iso-smoke       # a BIOS and a CD drive: the shell, /bin, /etc (14)
make x86-iso-uefi-smoke  # OVMF, a CD drive and a USB stick (26)
make x86-iso-usb-smoke   # a BIOS and a USB stick (13)
make x86-iso-run         # the image in a terminal, from a CD, on a BIOS
```

The two smokes of the firmware and of the stick are one tool,
`tools/x86_iso_uefi_smoke.py`: `--firmware uefi|bios` picks the firmware
(OVMF or SeaBIOS) and `--media cdrom|usb|both` the medium. The firmware is
**not told** which device to boot: the machine is started with the image on a
DVD drive (`-drive media=cdrom`) or behind a USB controller
(`-device usb-storage`), and the test reads the transcript for the boot the
firmware chose — `BdsDxe: starting Boot0002 "UEFI QEMU QEMU USB HARDDRIVE"`
is the firmware finding the ESP of the stick by itself. A boot from the USB
controller is the proof that the image is a stick, and not a CD that only
works because QEMU was told to use the catalogue.

| tool | why | package (Arch / Debian) |
|---|---|---|
| `xorriso` | ISO9660, El Torito and the GPT of the system area | `libisoburn` / `xorriso` |
| `limine` | the data files (`limine-bios.sys`, `limine-bios-cd.bin`, `limine-uefi-cd.bin`) **and** the command `limine bios-install`, which writes the boot sector of the stick | `limine` / `limine` |
| `grub-mkrescue` | the first choice of the boot loader, with a hybrid record of its own | `grub` / `grub-pc-bin grub-efi-amd64-bin` |
| OVMF | the firmware of the UEFI boots | `edk2-ovmf` / `ovmf` |
| `qemu-system-x86_64` | the machines of the tests | `qemu-system-x86` / `qemu-system-x86` |

`tools/x86_iso.py --no-hybrid` writes the CD of the first prototype (one El
Torito record, no GPT, no boot sector): it boots from a drive and from
nothing else. `--bootloader grub` writes a hybrid image with `grub-mkrescue`
and was not booted on the machine that wrote this page (no GRUB there).

## What is proved, and what is not

Proved, in QEMU, by the smokes above: the four ways in reach the kernel,
verify its signature, mount the root image of the medium and run the programs
of `/bin`; the BIOS boots, from a CD and from a stick, run the shell and
answer `ls /bin`, `df`, `cat /etc/setting` and `exec /bin/hello`.

Not proved, and honest about it:

* **A live machine.** Nothing on this page was booted on real hardware: the
  tests are QEMU with SeaBIOS and with OVMF, and the firmware of a laptop has
  its own ideas about USB 3 ports, the order of the devices and the keys of
  Secure Boot. The first live boot is a person's task, and the failure modes
  are at the end of this page.
* **The shell of the UEFI boot.** The firmware finds the ESP, limine starts
  the loader, the kernel comes up, mounts the root image and starts the
  program of `/bin` — and stops there. The cause is the map of the memory of
  the port, not the medium: limine places the modules of a firmware boot at
  23.5 MiB, the root image is 12 MB, so the heap of the port starts at
  35.5 MiB, which is inside the virtual window of the programs (32-48 MiB). A
  program's page tables map that range to the arena at 64 MiB, so the kernel's
  heap is remapped as soon as a program runs and the first call faults (`CR2`
  is the kernel's stack, 37 MB). The last 3.5 MB of the root image reach into
  the same window, which is the second half of the cause. A BIOS never sees
  it: there the modules land below 16 MiB and the heap is at 16.6-32 MiB.
  `make x86-iso-uefi-smoke` reports it and names the stall; the fix is the map
  of the heap and the placement of the modules in `ports/port_x86_64/src`,
  which is the work of the firmware boot of ADR 0037, not of this tool.
* **Secure Boot on.** By design: see above.
* **A reproducible image.** Two builds of one tree differ in the recording
  dates of the ISO9660 directory records, which xorriso takes from the clock
  of the run (the volume descriptor and the partition table are stamped from
  `SOURCE_DATE_EPOCH`). The debt is K13 of `docs/debts.md`.

## When a machine will not boot the stick

* The firmware lists the stick but the boot returns to the menu: the ESP is
  found and the application refused — Secure Boot, or a partition table the
  firmware reads as "not a bootable disk".
* Nothing at all, and the stick is not even listed: USB boot is off in the
  setup, or the stick is on a port the firmware does not boot from (an old
  machine and a USB 3 port). Try another port.
* The boot starts and stops on a black screen with a blinking cursor: the
  compatibility path took the MBR, and the second stage of the BIOS did not
  find `limine-bios.sys` — the write was cut (no `conv=fsync`), or the stick
  was written with a tool that copied the tree instead of the image.
* A kernel line and then nothing: the kernel is up and the medium or the
  program of `/bin` failed. The serial port (115200 8N1 on the first port of
  the machine) carries the same lines as the screen, and it is how a laptop
  without a serial port is read with a USB-UART adapter.
