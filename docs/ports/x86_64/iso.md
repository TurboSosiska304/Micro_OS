# The CD: a bootable ISO (K13)

The machine of the port boots in QEMU with `-kernel` and `-initrd`: the
hypervisor reads the multiboot image and puts the kernel in the memory.
This page is the other machine, the one a person has: a BIOS reads the El
Torito catalogue of an ISO, starts a boot loader, and the boot loader starts
`loader32.elf` with the kernel and a root image as modules. The decision is
[ADR 0026](../../adr/0026-bios-iso.md).

Status: a prototype. It boots, mounts the root image and runs `/bin`; the
gaps are at the end of this page.

## Build it

```bash
make x86-iso          # build-x86-iso/microos.iso
make x86-iso-run      # boot it in a terminal (-nographic)
make x86-iso-smoke    # boot it and check the transcript
```

`make x86-iso` depends on `make x86-disk` and on the host tools: it builds
nothing a second time. It stages three files - the loader, the signed kernel
and `programs.img` of `make x86-disk` - and asks a boot loader to write the
image:```
boot/loader32.elf    the loader of the port, a multiboot 1 image
boot/microos64.bin   the kernel, signed (ADR 0020)
boot/root.img        the root image: MBR + MicroFS with /bin and /etc
boot/grub/grub.cfg   the menu, when the boot loader is GRUB
boot/limine.conf     the menu, when the boot loader is limine
```

What has to be on the machine:

| Tool | Why | Package (Arch / Debian) |
|---|---|---|
| `xorriso` | writes the ISO9660 and the El Torito catalogue | `libisoburn` / `xorriso` |
| `grub-mkrescue` | the first choice of the boot loader | `grub` / `grub-pc-bin grub-common` |
| `limine` | the fallback: `limine-bios.sys` and `limine-bios-cd.bin` in `/usr/share/limine` | `limine` / `limine` |
| `qemu-system-x86_64` | the machine of the tests | `qemu-system-x86` / `qemu-system-x86` |

`tools/x86_iso.py --bootloader auto` (the default of the Makefile) takes
GRUB when `grub-mkrescue` is there and limine otherwise; `grub` and `limine`
force one and fail with the package to install when it is not. `LIMINE_DIR`
names a directory other than `/usr/share/limine`, `/usr/lib/limine` or
`/usr/local/share/limine`. The image of this machine was written with
limine 12.10, because GRUB is not installed there; the GRUB half of the tool
follows the same three files and has not been booted.

`make x86-iso-smoke` is not part of `make ci`: it needs a boot loader of the
system (`grub-mkrescue` or limine) and `xorriso` installed, which the build
farm is not promised; the gate of the kernel stays `make x86-smoke`.

## The boot

```
BIOS -> El Torito catalogue -> boot loader (GRUB or limine)
     -> loader32.elf  (multiboot 1: header, 32-bit, at 1 MiB)
          verifies the signature of module 0, copies the kernel to 16 MiB,
          refuses a module that lies over those bytes, builds the identity
          map of the first gigabyte and the kernel at the top, enters long mode
     -> microos64.bin (module 0)
     -> root.img      (module 1)
```

The loader passes the multiboot information through, and the kernel reads
the module table from it:

* `src/blockdev_module.c` registers the bytes of module 1 as `boot0`, a
  read-only block device of 4 KiB sectors;
* `partition_scan()` reads its MBR - the root image is the image of
  `make x86-disk` - and registers `boot0p1`;
* `port_x86_64_root_device()` names `boot0p1` before the ATA disk and
  before `ram0`, and `kmain` mounts it at `/`;
* the heap of the port starts above the last byte of every module, so a
  block of the allocator is never the bytes of the volume the boot reads.

The volume is read-only: the medium is the image of the CD. A write is
refused by the block device and reported by the filesystem:

```
microos> write /new.txt hello
write: /new.txt: input/output error
microos> mkdir /d
mkdir: /d: input/output error
```

The boot says the same about the three settings of `/etc` it would create on
a writable volume:

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

* a BIOS boots the ISO from a CD drive or from `-cdrom` in QEMU, with no
  `-kernel`, no `-initrd` and no disk;
* the loader verifies the signature of the kernel before a byte of it runs
  (ADR 0020) and stops with a sentence when a boot loader puts a module over
  the kernel image;
* the root image is mounted from the second module as `boot0p1`, and the
  boot runs `/bin/hello` out of it through `execve`;
* the shell, `/bin` (16 programs), `cat`, `ls`, `df`, `exec`, `ps` and the
  rest of the tree work from the CD, read-only;
* `make x86-iso-smoke` checks 14 lines of that boot, including `ls /bin`,
  `df`, `cat /etc/setting`, `exec /bin/hello` and `ls /`.

## What does not work yet

* **UEFI.** The ISO boots BIOS only. The EFI application needs the memory
  map of the firmware, GOP for the screen and the exit of the boot services
  before the kernel; the port draws into the text mode of the BIOS, and
  ADR 0023 (the framebuffer of 1.2) is the prerequisite.
* **A hybrid image.** The ISO does not boot from a USB stick written with
  `dd`: that needs a boot sector installed into the image with
  `limine bios-install` (or a protective MBR and GRUB), and the two were not
  part of this prototype. The CD path through El Torito is the one tested.
* **A writable root.** The root is the image of the CD and every write is
  `EIO`. `/etc` is the one of the tool; `config set` edits the file in the
  memory of the filesystem and the edit is lost at the next boot.
* **Persistence.** There is no place for a person's files. A machine that
  has a disk and boots the ISO still mounts `boot0p1` as `/`; the disk is
  not mounted automatically.
* **The GRUB path.** `--bootloader grub` writes the menu and calls
  `grub-mkrescue`, but it was never booted on this machine, which has no
  GRUB. The limine path is the one of `make x86-iso-smoke` here.
* **A reproducible ISO.** The volume descriptor is stamped from
  `SOURCE_DATE_EPOCH`, but the directory records still carry the clock of
  the run: two builds of one tree differ in a few bytes of the dates.
* **An installed system.** There is no installer, no `grub.cfg` on a disk
  and no way to boot without the CD; `pkg` and K12 are the next step.

The register of these is `docs/debts.md`, section K13.
