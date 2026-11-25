# The boot of a firmware: Micro OS under UEFI

A machine of this age has no BIOS to hand a kernel a sector of a disk: the
firmware is UEFI, it reads a FAT partition, and it starts a PE/COFF
application from `\EFI\BOOT\BOOTX64.EFI`. This page is the loader of that
application, the picture it hands the kernel, and what is proved in QEMU (with
OVMF, the EDK2 build that QEMU carries) and what is not proved yet - the live
laptop.

| | |
|---|---|
| application | `ports/port_x86_64/loader/uefi/`, a PE/COFF image of the 64-bit machine, subsystem `EFI_APPLICATION`, about 16 KiB, no libc and no library |
| command | `make uefi` (clang and lld-link), `make uefi-run` (a window), `make uefi-smoke` (29 checks) |
| volume | one FAT32 volume: `EFI/BOOT/BOOTX64.EFI`, `microos64.bin`, `disk.img` (`tools/uefi_esp.py`) |
| kernel | the same `microos64.bin` as `make x86-build`: one kernel for the BIOS path and the firmware path |
| screen | `EFI_GRAPHICS_OUTPUT_PROTOCOL`, the biggest mode within 8 MiB of frame; a shadow in RGB565 and a conversion of the cells that change (`src/gop.c`) |
| disk | the ESP carries `disk.img` as the read-only root of a machine whose USB did not come up (`boot0p1`); the medium of a person is the stick of [ADR 0056](../../adr/0056-the-stick-of-a-person-is-a-disk.md), whose second partition is mounted `rw` as `usb0p2`; the disk of the machine is a volume of `ahci0p1` or `nvme0p1`, listed in `/etc/volumes` (ADR 0041, ADR 0042) |
| keyboard | PS/2 (the port's own `src/ps2.c`, including the embedded controller of a laptop) and USB HID of the xHCI (`src/xhci.c`); the page of a person is [`docs/install.md`](../../install.md) |
| serial | COM1, 115200 8N1, from the first line of the loader |

## What the loader does

`efi_main()` in `loader/uefi/uefi_loader.c`, in the order the firmware allows:

1. **The watchdog.** The firmware resets a program that takes too long;
   `SetWatchdogTimer(0, 0, 0, NULL)` turns it off. A loader that reads 16 MB
   of disk image slowly is not a hang.
2. **The picture.** `LocateProtocol(EFI_GRAPHICS_OUTPUT_PROTOCOL)`, then every
   mode of the card with `QueryMode`. The mode the firmware is already in is
   the first candidate; a mode bigger than it and within 8 MiB of frame wins
   (`choose_mode`). `SetMode` programs it and the loader reads back
   `FrameBufferBase`, `FrameBufferSize` and `PixelsPerScanLine`.
3. **The volume.** `EFI_LOADED_IMAGE_PROTOCOL` gives the device the
   application was started from, `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` opens it,
   and `EFI_FILE_PROTOCOL` reads `\microos64.bin` and `\disk.img` whole into
   pages the firmware gave out below 4 GiB (`GetInfo` for the size, `Read` for
   the bytes).
4. **The kernel's place.** `AllocateAddress` at 16 MiB, the address of the
   BIOS loader; when that block is taken (it is, in OVMF, because the loader
   itself is often there), the search walks down from 14 MiB in steps of 2 MiB
   and stops when the image fits below 32 MiB, the base of the arena of the
   programs. The image has to be 2 MiB aligned because the page table of the
   boot maps it with 2 MiB pages. The address is handed to the kernel in the
   third register of the entry, so the port no longer assumes 16 MiB - the
   loader of the BIOS path says it in the same register for the same reason.
5. **The map of the memory.** `GetMemoryMap` once for the size and once for
   the map; the regions of the firmware become the 24-byte entries of the
   multiboot 1 map the port already reads (`EfiConventionalMemory`,
   `EfiBootServicesCode/Data` usable, everything else reserved), clipped at
   4 GiB. The two modules - the kernel and `disk.img` - and the
   multiboot structure itself are written in a page of the loader's own, which
   is `EfiLoaderData` and stays out of the usable memory.
6. **The ACPI tables.** `EFI_CONFIGURATION_TABLE` holds the root pointer of
   ACPI 2.0 (GUID `8868E871-E4F1-11D3-BC22-0080C73C8881`). A machine of UEFI
   does not keep it in the extended BIOS area, so the loader passes it and
   `src/cpu.c` reads the MADT through it; the BIOS path keeps its own search.
7. **`ExitBootServices`.** With the key of the map that is current, in a loop:
   the specification allows the call to fail because the firmware allocated
   something between the two calls. After this there is no console of the
   firmware, no file and no memory service; the loader's line from here on is
   the one of COM1.
8. **The tables of the boot** (`build_tables`): `PML4[0]` is the identity of
   the first 4 GiB in 2 MiB pages - the memory of the kernel, the modules, the
   hand-off and the code of the loader - and `PML4[511]` names the image at
   `0xFFFFFFFF80000000`, 16 MiB of it in 8 chunks of 2 MiB. The kernel builds
   its own map as the first thing its C does (`src/paging.c`); these carry one
   jump.
9. **The jump** (`loader/uefi/uefi_enter.S`): `CR4.PAE|OSFXSR|OSXMMEXCPT`,
   `CR3`, `EFER.LME|NXE`, `CR0.PG|PE|NE`, a GDT of three entries, and `jmp`
   to the base of the image with `RDI` = the hand-off, `RSI` = the magic of
   the boot (`PORT_X86_64_BOOT_MAGIC`) and `RDX` = where the image is.

The hand-off itself is `ports/port_x86_64/include/port_x86_64_boot.h`: the
magic, the multiboot information, the picture (frame, size, width, height,
pitch, bits and format of a pixel, masks), and the root of the ACPI tables.
Where the image is, is not in it: both loaders pass that in the third register
of the entry (`RDX`), the loader of the BIOS path with the multiboot
information and `RSI` zero (`loader32.S`), and `port_x86_64_init()` reads one
protocol or the other before it builds a page table.

## The picture, and why there is a shadow

The console of the kernel draws RGB565 (`kernel/render.h`); the graphics
output of a firmware is `PixelBlueGreenRedReserved8BitPerColor`
(`BGRX`, the frame of almost every real card and of OVMF) or
`PixelRedGreenBlueReserved8BitPerColor`, and a 16-bit mode exists in the
specification but hardly any firmware offers one. So `src/gop.c` keeps the
picture of the kernel in a shadow of 16 bits per pixel from the port's heap
(4 MiB for 1920x1080) and converts the cells that changed into the frame of
the firmware:

* the frame is mapped uncached (PCD and PWT, `paging.c`'s
  `port_x86_64_paging_map_mmio`) in the top 2 GiB, at the virtual window the
  VBE screen uses as well;
* the console of `src/fb.c` writes the shadow unchanged - the same grid, the
  same font, the same colours, the same `/dev/fb0`;
* `src/gop.c` wraps the table of operations of the screen: after `put` it
  converts the band of that row of cells, after `clear`, `scroll`, `colors`
  and `redraw` the whole picture. The band is `[row * 16, (row + 1) * 20)`
  lines, the widest a row of cells can be (`font.line_gap` of `/etc/font` adds
  0 to 4 pixels to the 16 of the cell), so a present never misses a line and
  never converts more than four lines a row;
* the formats the conversion understands are `RGBX32`, `BGRX32` and the two
  bit-mask formats of 16 or 32 bits; the masks of a channel are a shift and a
  width, and a format none of them matches is refused by the loader (the mode
  is not chosen) and by the port (the screen is not taken).

The caps of `src/fb.c` are the ones of ADR 0023: the grid is at most 170
columns and 60 rows, so a frame of 1920x1080 is a console of 170x60 cells in
its top-left corner (1360x960 pixels) and the rest of the frame is the paper
of the console. A firmware mode of 1280x800 is a console of 160x50, the shape
of the VBE screen. Raising the caps is a change of `fb.c` and its grid.

## The disk is a file of the volume

`disk.img` is `build-x86/programs.img`: an MBR, one partition and a MicroFS
volume with `/bin` and `/etc` (`tools/x86_image.py`). The loader copies it
into the memory and describes it as module 1 of the multiboot information; the
port registers the bytes as the read-only block device `boot0`, the partition
parser finds `boot0p1` and the boot mounts it (`src/blockdev_module.c`, ADR
0026 §2 - the mechanism the ISO uses as well).

Two consequences worth naming:

* the volume of the boot is **read-only** while it is the module of the ESP:
  `touch`, `write` and `config set` that keep a file fail with the error of the
  medium, and the boot says so (`settings: cannot write /etc/display`). A
  machine whose USB controller answered boots the second partition of the stick
  instead (`usb0p2`, ADR 0056), and that root is written;
* the module is the fallback, not the only disk: the port drives the SATA
  controller (ADR 0041, `make ahci-smoke`), the NVMe controller (ADR 0042,
  `make nvme-smoke`) and the stick of the Bulk-Only Transport (ADR 0040), and
  `/etc/volumes` is what opens the writes of a volume the system made
  (ADR 0041 §1).

## Writing the volume to a USB stick

> The medium a person installs now is the **stick of two partitions**:
> `make uefi-stick` writes `build-uefi/stick.img` and the second partition is
> the writable root ([ADR 0056](../../adr/0056-the-stick-of-a-person-is-a-disk.md),
> [`docs/install.md`](../../install.md)). The section below is the first
> medium, the ESP alone, and it stays for a machine that only reads.

The image `make uefi` writes is a FAT32 filesystem in a file
(`build-uefi/esp.img`, 48 MB). Two ways to a stick:

```bash
# 1. the whole stick is the volume (the firmware reads a superfloppy)
sudo dd if=build-uefi/esp.img of=/dev/sdX bs=4M conv=fsync status=progress

# 2. the stick is a disk with one partition
sudo parted /dev/sdX mklabel gpt
sudo parted /dev/sdX mkpart ESP fat32 1MiB 100%
sudo mkfs.vfat -F32 -n MICROOS /dev/sdX1
sudo mount /dev/sdX1 /mnt
sudo mkdir -p /mnt/EFI/BOOT
sudo cp build-x86/BOOTX64.EFI  /mnt/EFI/BOOT/BOOTX64.EFI
sudo cp build-x86/microos64.bin /mnt/microos64.bin
sudo cp build-x86/programs.img  /mnt/disk.img
sync && sudo umount /mnt
```

`/dev/sdX` is the **stick** and not the disk of the machine: `lsblk` before
the first byte. On a machine whose firmware is set to the legacy CSM, the
boot entry of the stick ("UEFI: <name of the stick>") is the one to choose.

## What is proved in QEMU with OVMF

`make uefi-smoke` boots `build-uefi/esp.img` with

```
qemu-system-x86_64 -machine q35 -smp 2 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=<a copy of OVMF_VARS.4m.fd> \
    -drive file=build-uefi/esp.img,format=raw,if=ide,index=0 \
    -nographic -no-reboot -monitor unix:<socket>
```

and checks 26 things about that boot:

* the firmware starts `BOOTX64.EFI` and the loader's lines are on the console;
* it programs a picture and says which one - in OVMF, mode 22 of 30,
  1920x1080, 32 bpp `bgrx`, a frame of 8100 KB at `0x80000000`;
* the volume has the kernel and the disk image, and both are read whole;
* the kernel is placed below the arena, the memory map is read, the root of
  the ACPI tables comes from the configuration table of the firmware;
* the boot services are left and the kernel comes up: the banner, the boot
  line (`x86-64: the boot is uefi`), the W^X audit of the map, the console on
  the frame of the firmware;
* the firmware's MADT is read through the ACPI root: `2 cpu(s) reported by the
  firmware` on a machine of `-smp 2` (the EBDA search of the BIOS path finds
  nothing there, so this line is the proof that the hand-off works);
* the disk is `boot0p1`, the read-only module of the boot, and
  `/bin/hello` runs from it, in ring 3, out of a file of that volume;
* the shell answers `uname`, `ls /bin`, `cat /etc/setting` and `pkg -Q` (the
  last one with the refusal of a build without the installer, which is the
  honest answer of that build);
* the **screenshot** of the card (`screendump` through the QEMU monitor) is a
  PPM of 1920x1080, the pixel of the console's ink is the light grey of the
  theme widened from RGB565, a command typed at the shell changes the picture,
  and the part of the frame outside the grid of cells is the paper. The
  conversion of `src/gop.c` is read pixel by pixel, not assumed.

The BIOS path stays green on the same kernel: `make x86-smoke` (429 checks and
`x86-disk-smoke`, `x86-root-smoke`, `x86-smp-smoke`, `make smoke`) boot
`loader32.elf` with `-kernel` exactly as before, and the port chooses the VBE
frame because the boot handed it no picture.

## What is not done: the live laptop

**The numbers of this page are QEMU with OVMF.** The stick has booted on the
laptop of the owner since 1.9.0, and the defects it found there are the list of
`docs/releases/2.0.md`; a firmware of another make (Insyde, AMI, Phoenix,
Apple) is another implementation of the same specification, and the parts that
it does differently are exactly the ones that cost a day - the mode list it
offers, the memory map it describes, the console it has, and whether it keeps
its own memory below 4 GiB.

This lane ends at a machine that boots from a stick and keeps a file on the
second partition of it (`make uefi-stick-smoke`, 21 checks, ADR 0056). What is
left of a laptop of a person, in the order it is worth doing:

1. **A pointer.** The keyboard is read on both of its paths: the built-in one
   arrives through the embedded controller as PS/2 on IRQ1 (the ThinkPad X1 of
   the owner speaks the scancode set 2, and the port asks which set it speaks
   at the boot and translates all three, `kernel/input_scancode.c`), and the
   external one is a HID boot-protocol keyboard on the xHCI (`make usb-smoke`,
   66 checks, ADR 0040). The touchpad and the mouse of a laptop are not read
   (`docs/install.md` §9).
2. **A root that is the disk of the machine.** The port drives the SATA
   controller (ADR 0041, `make ahci-smoke`, 33 checks) and the NVMe controller
   (ADR 0042, `make nvme-smoke`, 35), and a volume is written when
   `/etc/volumes` names it; what is not there is an installer: the system lives
   on the stick it was written to, and no partition is added beside it
   (`docs/install.md` §9, K12).
3. **The clock and the shutdown.** The port reads the CMOS chip at the boot and
   writes a moment back into it (`ntp`), and `shutdown`/`reboot` return the
   machine to the firmware through ACPI S5 (ADR 0050, `make power-smoke`,
   22 checks). There is no suspend and no lid switch.
4. **The interrupts of the devices.** The I/O APIC carries the timer and the
   keyboard, with the overrides of the MADT and the 8259 masked (`src/ioapic.c`),
   but a device of the port cannot own a line of its own: there is no call that
   installs a handler and no MSI/MSI-X, so the card and the disk poll
   (`docs/debts.md`).
5. **Wi-Fi.** The wire is the network of this release: the radio of a laptop is
   a port of `iwm(4)` and `net80211`, fifteen to twenty thousand lines and its
   own plan (`docs/plan-wifi.md`, ADR 0039).

Other limits of this lane, honestly:

* the loader runs below 4 GiB and says so if the firmware puts it elsewhere
  (the identity map of its own jump covers the first 4 GiB);
* `ExitBootServices` and the page tables are proved on OVMF; a firmware that
  keeps its memory above 4 GiB, or a machine whose ACPI tables are out of the
  map, is a case of a later lane (`docs/debts.md` is the place to write it
  down);
* the module of the ESP is read-only, so a machine whose USB did not come up
  forgets a file at the power (the second partition of the stick does not);
* the screen is a conversion of a shadow, not a 2D engine: a program of the
  UI draws at the speed of the conversion of the cells it touches, and a
  full repaint of 1920x1080 is a copy of 8 MB;
* a program that draws on `/dev/fb0` reaches the glass of a GOP machine
  through `port_x86_64_gop_present()`, which `src/fbdev.c` does not call yet:
  the blits of a program are in the shadow and arrive at the next full present
  (a scroll, a clear, `FBIO_GIVE`). The console's own lines arrive at once;
  the one-line change belongs to the file of the device and is a job of the
  lane of the desktop;
* `config set font.line_gap` repaints the grid of the shadow and the glass
  follows it at the next full present; the boot applies the setting before the
  first picture, so the only stale case is a person changing the gap in a
  running machine.

## The files

| file | what it is |
|---|---|
| `loader/uefi/efi.h` | the part of the UEFI specification the loader reads, by hand: the tables, the console, the graphics output, the filesystem, the memory map, the GUIDs |
| `loader/uefi/uefi_loader.c` | the boot: the picture, the files, the map, the hand-off |
| `loader/uefi/uefi_enter.S` | the four registers of the processor, the GDT and the jump |
| `include/port_x86_64_boot.h` | the hand-off of a UEFI boot, read by the loader and by the port |
| `src/gop.c` | the screen of the firmware: the frame, the shadow, the conversion |
| `tools/uefi_esp.py` | the volume of the boot |
| `tools/uefi_smoke.py` | the gate: OVMF, the shell and the picture |
