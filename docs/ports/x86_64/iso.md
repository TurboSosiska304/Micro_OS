# The medium: a bootable image (K13)

The machine of the port boots in QEMU with `-kernel` and `-initrd`: the
hypervisor reads the multiboot image and puts the kernel in the memory. This
page is the other machine, the one a person has: a medium. `make x86-iso`
writes one image, and it boots in four: a BIOS and a firmware of UEFI, each
from a CD drive and from a USB stick written with `dd`. The decision is
[ADR 0026](../../adr/0026-bios-iso.md), amended for the firmware and the
stick; the page of the person who carries the stick is [boot.md](boot.md).

Status: the image boots, mounts the root image and runs `/bin`. The BIOS
halves (a CD and a stick) reach the shell; the two halves of the firmware
reach the kernel and the root image and wait for one fix in the port, which
is the list at the end of this page.

## Build it

```bash
make x86-iso             # build-x86-iso/microos.iso
make x86-iso-run         # boot it in a terminal (-nographic), from a CD
make x86-iso-smoke       # a BIOS and a CD: the shell, /bin, /etc (14 checks)
make x86-iso-uefi-smoke  # OVMF, a CD and a stick of USB (26 checks)
make x86-iso-usb-smoke   # a BIOS and a stick of USB (13 checks)
```

`make x86-iso` depends on `make x86-disk` and on the host tools: it builds
nothing a second time. It stages the tree of the image:

```
boot/loader32.elf      the loader of the port, a multiboot 1 image
boot/microos64.bin     the kernel, signed (ADR 0020)
boot/root.img          the root image: MBR + MicroFS with /bin and /etc
boot/limine.conf       the menu
boot/limine-bios.sys   the second stage of the BIOS (read from the image)
boot/limine-bios-cd.bin   the El Torito record of the BIOS
boot/limine-uefi-cd.bin   the FAT image of the EFI: the second record of the
                          catalogue and the ESP of the medium
EFI/BOOT/BOOTX64.EFI   a copy of the application, for a tool that writes a
                       stick out of the tree instead of the image
```

and asks `xorriso` for the image: the two records of El Torito (`-b` for the
BIOS, `--efi-boot` for the EFI), a GPT whose partition is the FAT image of the
EFI (`-efi-boot-part --efi-boot-image --protective-msdos-label`), and the
mark of a basic data partition that some tools look for
(`-isohybrid-gpt-basdat`). The last step is `limine bios-install`: it writes
the first stage of the BIOS into the boot sector and the second into the 32
KiB of the system area (before the ISO9660 descriptor), and folds the GPT
into the MBR ("gpt-to-mbr-isohybrid conversion", in its words), because the
firmware of a stick reads the MBR of the medium first. The partition table of
the finished image is therefore a hybrid MBR of three entries: the head of the
El Torito EFI image, the **ESP** (type `0xEF`, the FAT image at LBA 132, 6144
sectors) and the ISO9660 volume. `xorriso -indev image -report_system_area
plain` prints it. That boot sector is what makes `dd` of the image to a stick
a bootable disk; it is not a step of a CD, which reads the catalogue instead.

What has to be on the machine:

| Tool | Why | Package (Arch / Debian) |
|---|---|---|
| `xorriso` | writes the ISO9660, the El Torito catalogue and the GPT | `libisoburn` / `xorriso` |
| `limine` | the three boot files of the image **and** the command `limine bios-install` | `limine` / `limine` |
| `grub-mkrescue` | the first choice of the boot loader (`--bootloader grub`) | `grub` / `grub-pc-bin grub-efi-amd64-bin` |
| OVMF | the firmware of the two UEFI boots of the tests | `edk2-ovmf` / `ovmf` |
| `qemu-system-x86_64` | the machines of the tests | `qemu-system-x86` / `qemu-system-x86` |

`tools/x86_iso.py --bootloader auto` (the default of the Makefile) takes
GRUB when `grub-mkrescue` is there and limine otherwise; `grub` and `limine`
force one and fail with the package to install when it is not. `LIMINE_DIR`
names a directory other than `/usr/share/limine`, `/usr/lib/limine` or
`/usr/local/share/limine`. The image of this machine was written with
limine 12.10, because GRUB is not installed there; the GRUB half of the tool
follows the same three files, writes a hybrid record of its own, and has not
been booted. `--no-hybrid` writes the CD of the first prototype: one record,
no GPT, no boot sector of a disk.

`make x86-iso-smoke` boots the image from a CD on a BIOS and is not part of
`make ci`: it needs `xorriso` and a boot loader of the system installed,
which the build farm is not promised; the gate of the kernel stays
`make x86-smoke`. The two targets of a firmware need OVMF as well, and a
machine without it stops with the package to install (the exit status is 2,
the way `tools/x86_iso.py` says which boot loader is missing).

## The boot

```
BIOS, CD     El Torito catalogue -> limine-bios-cd.bin -> limine-bios.sys
BIOS, stick  MBR -> the first stage of limine -> limine-bios.sys
UEFI, CD     El Torito catalogue -> limine-uefi-cd.bin -> BOOTX64.EFI
UEFI, stick  the MBR -> the ESP (type 0xEF) -> BOOTX64.EFI
                    |
                    v
             loader32.elf  (multiboot 1: header, 32-bit, at 1 MiB)
                  verifies the signature of module 0, copies the kernel to
                  the first free 2 MiB block at or above 16 MiB (the module
                  of the medium may be in the way, and the kernel is told
                  where the copy went), builds the identity map of the first
                  gigabyte and the kernel at the top, enters long mode
             -> microos64.bin (module 0)
             -> root.img      (module 1)
```

The loader passes the multiboot information through, and the kernel reads the
module table from it:

* `src/blockdev_module.c` registers the bytes of module 1 as `boot0`, a
  read-only block device of 4 KiB sectors;
* `partition_scan()` reads its MBR - the root image is the image of
  `make x86-disk` - and registers `boot0p1`;
* `port_x86_64_root_device()` names `boot0p1` before the ATA disk and
  before `ram0`, and `kmain` mounts it at `/`;
* a module that a loader put inside the window of the programs (32-48 MiB) is
  copied out of it before the page tables of the port exist, and the heap is
  placed in the first free piece of the memory outside the window: a block of
  the allocator is never the bytes of the volume the boot reads, and never
  the arena a program sees through its page tables.

The volume is read-only: the medium is the image of a CD or of a stick written
from it. A write is refused by the block device and reported by the
filesystem:

```
microos> write /new.txt hello
write: /new.txt: input/output error
microos> mkdir /d
mkdir: /d: input/output error
```

The boot says the same about the three settings of `/etc` it would create on a
writable volume:

```
settings: cannot write /etc/display
settings: cannot write /etc/font
settings: cannot write /etc/console
```

`/bin` and `/etc/setting` come from the image and are read normally:

```
microos> df
blocks: 6784 of 6877 free (1 KB each)
inodes: 9130 of 9151 free
microos> exec /bin/hello
PROGRAM: hello from /bin
PROGRAM: cpl 3
...
the program exited with 0
```

## What it does

* a BIOS boots the image from a CD drive or from a USB stick, with no
  `-kernel`, no `-initrd` and no disk, and reaches the shell;
* a firmware of UEFI finds the ESP of the medium — the second record of the
  catalogue on a CD, the `0xEF` partition of the MBR on a stick — starts the
  EFI application and the same loader, and the kernel mounts the same root
  image;
* the loader verifies the signature of the kernel before a byte of it runs
  (ADR 0020) and stops with a sentence when a boot loader puts a module over
  the kernel image;
* the root image is mounted from the second module as `boot0p1`, and the boot
  runs `/bin/hello` out of it through `execve`;
* the shell, `/bin` (16 programs), `cat`, `ls`, `df`, `exec`, `ps` and the
  rest of the tree work from the medium, read-only;
* `make x86-iso-smoke` checks 14 lines of the boot of a BIOS and a CD: the
  signature, the mount, `ls /bin`, `df`, `cat /etc/setting`, `exec /bin/hello`
  and `ls /`;
* `make x86-iso-uefi-smoke` starts the image twice on OVMF, once on a DVD
  drive and once behind a USB controller, and checks 13 lines of each: the
  device the firmware chose, the signature, the mount, the program of `/bin`,
  the shell, `uname`, `ls /bin` and `cat /etc/setting`;
* `make x86-iso-usb-smoke` is the same 13 lines for a BIOS and a stick.

## What does not work yet

* **The shell of the UEFI boot.** The firmware finds the ESP, the loader and
  the kernel come up, the root image is mounted and `/bin/hello` is started —
  and the machine stops there. The cause is the map of the memory of the port,
  not the medium: limine places the modules of a firmware boot at 23.5 MiB, the
  root image is 12 MB, so `heap_init()` starts the heap at 35.5 MiB — inside
  the virtual window of the programs (32-48 MiB). Under the page tables of a
  program that range is the arena at 64 MiB, so the kernel's own heap is
  remapped the moment a program runs, and the first call faults (`CR2` is the
  stack of the kernel, 37 MB). A second half of the same cause: the last 3.5 MB
  of the root image lie in the window as well, so a volume read above 32 MiB of
  the module reads the arena. A BIOS never sees either: there the modules land
  below 16 MiB and the heap is at 16.6-32 MiB. Two lines of a guard in
  `heap_init()` (the heap does not follow a module into the window) move the
  boot to the shell and through `uname`; the whole of the fix belongs to the
  map of the heap and to the loader that places the modules
  (`ports/port_x86_64/src`, ADR 0037), and `make x86-iso-uefi-smoke` reports
  the stall and names it. The two boots themselves — the ESP on a stick,
  found by the firmware — are proved by it.
* **A writable root.** The root is the image of the medium and every write is
  `EIO`. `/etc` is the one of the tool; `config set` edits the file in the
  memory of the filesystem and the edit is lost at the next boot.
* **Persistence.** There is no place for a person's files. A machine that has
  a disk and boots the image still mounts `boot0p1` as `/`; the disk is not
  mounted automatically.
* **A live machine.** Every boot of this page is a QEMU: SeaBIOS for the BIOS,
  OVMF for the firmware. No laptop, no desktop and no real CD drive has booted
  the image, and the firmware of a machine of this age has its own rules about
  the ports of USB, the order of the devices and Secure Boot (which has to be
  off: see [boot.md](boot.md)).
* **The GRUB path.** `--bootloader grub` writes the menu and calls
  `grub-mkrescue`, which writes a hybrid record of its own, but it was never
  booted on this machine, which has no GRUB.
* **A reproducible image.** The volume descriptor and the partition table are stamped from
  `SOURCE_DATE_EPOCH` and the disk identity is the label of the volume, but the
  directory records still carry the clock of the run: two builds of one tree
  differ in a few bytes of the dates.
* **An installed system.** There is no installer, no `grub.cfg` on a disk and
  no way to boot without a medium; `pkg` and K12 are the next step.

The register of these is `docs/debts.md`, section K13.
