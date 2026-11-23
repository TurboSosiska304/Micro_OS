# ADR 0037 - The boot of a firmware: an application of UEFI

* Status: accepted (prototype)
* Date: 2026-11-23
* Supersedes: nothing
* Related: ADR 0007 (the higher half and the page tables), ADR 0020 (the
  signed image), ADR 0023 (the screen of x86-64), ADR 0026 (the ISO and the
  root image as the second module), `docs/ports/x86_64/uefi.md`

## Context

The machine of the port boots because QEMU loads a multiboot image
(`-kernel loader32.elf -initrd microos64.bin`): a hypervisor is the firmware,
it puts the kernel in the memory and describes it in the boot information. The
ISO of ADR 0026 added a medium and a boot loader of the system, and it is
BIOS-only for a reason its decision names: the EFI application needs the
graphics output of the firmware (GOP), and the screen of the port was the text
mode of the VGA.

Two things changed since: the screen of the port is a linear framebuffer in
RGB565 (ADR 0023, 1.2.0), and a machine a person owns has no BIOS at all -
the firmware is UEFI, it reads a FAT partition and starts a PE/COFF
application. "Micro OS boots on the laptop" is not a tone of voice; it is a
loader of its own, the picture of the firmware, the map of the memory of the
firmware, the exit of the boot services and a disk the kernel can mount without
a driver for the controller it is on.

The alternatives were:

* **a boot loader of the distribution** (GRUB, systemd-boot, limine with its
  EFI build): a medium a person can build today, and the boot of the project
  becomes a configuration file of somebody else's program. It also decides the
  shape of what the kernel is handed, and this port's shape - the higher half,
  the modules, the identity map - is the project's own;
* **an EFI stub inside the kernel** (the Linux way: the kernel is itself a PE
  image with an EFI entry point): fewer moving parts, and the kernel would
  carry the firmware's structures and the PE headers of a machine that has no
  firmware, which is the opposite of the rule that the kernel knows nothing
  about a machine;
* **an application of our own** beside `loader32.S`, in the same directory,
  built from the same tree: one more loader, no new dependency on a boot
  loader of the system, and the kernel unchanged on both paths.

## Decision

### 1. The loader is an EFI application of the tree

`ports/port_x86_64/loader/uefi/` is a PE/COFF image with the subsystem
`EFI_APPLICATION`, entered with the x86-64 Windows ABI, no libc and no header
of a system: `efi.h` is the part of the specification the loader reads, written
by hand. It is built by the compiler of the host (clang targets the COFF of
the 64-bit Windows machine; `lld-link` writes the headers) and `make uefi`
says which of the two tools is missing instead of building an image that does
not boot.

The kernel is the **same** `microos64.bin` as the BIOS path. The two boots are
told apart by the second argument of the entry: `loader32.S` passes zero and
the multiboot information, the EFI application passes the magic of
`port_x86_64_boot.h` and a structure that carries the multiboot information
(the modules and the map) beside the three things only a firmware can say -
the picture, the root of the ACPI tables and where the image was placed.

### 2. The disk of the machine is a file of the volume

The first step to a laptop must not wait for a driver of AHCI or NVMe. The
loader reads `disk.img` out of the FAT volume with the file services of the
firmware and describes it as module 1 of the boot; the port registers it as
the read-only block device `boot0`, exactly as it registers the root image of
the ISO (ADR 0026 §2). The machine boots with `/bin` and `/etc` and no driver
for the controller the volume was on, and the volume is read-only until the
lane of the disk.

### 3. The screen of the firmware is a shadow of the console's

The console of the kernel draws RGB565 and the graphics output of a firmware
is 32 bits per pixel in an order that is the firmware's. `src/gop.c` keeps the
picture in a shadow of 16 bits per pixel from the heap and converts the cells
that changed into the frame, which is mapped uncached; a firmware that offers
a 565 mode (rare) gets the console's writes directly. The format of the pixel
is a field of the port's description of the screen and not a business of the
console, and `src/fb.c` - the grid, the font, the colours and `/dev/fb0` - is
the same file on both machines.

The caps of the grid (170 columns by 60 rows) are ADR 0023's and stay: a mode
bigger than 1360x960 is a console in the top-left corner of the frame.

### 4. The gate is OVMF

`make uefi-smoke` boots the volume under OVMF (the EDK2 build QEMU carries)
with the monitor of QEMU beside it, and checks the loader, the kernel, the
shell and - with a screenshot - the picture of the card. The BIOS gate stays
what it was: `make x86-smoke` and the other four boots of the port run on the
same kernel and the same sources.

## Consequences

* One kernel, two loaders, one hand-off structure. A change to the page tables
  of the boot or to the place of the image is a change in both loaders and in
  `port_x86_64_boot.h`, and the BIOS gate catches half of it while
  `uefi-smoke` catches the other half.
* The port no longer assumes that the image is at 16 MiB: the address is a
  variable the boot sets (`g_port_x86_64_kernel_phys`), which is what lets the
  loader choose a block the firmware actually gave it.
* The build of the loader needs `clang` and `lld-link`. They are not needed
  by any other target: `make x86-build` and the tests do not call them.
* The volume of the boot is read-only and `disk.img` is copied to the memory
  at every boot: a file a person writes is gone at the reset. The next lane is
  a driver of the disk (AHCI, then NVMe) and a writable root.
* A laptop has no PS/2 keyboard. The console of the port reads IRQ1, so the
  machine of this lane boots to a shell that cannot be typed at. xHCI and a
  HID keyboard are the other half of "a person can use it".
* The loader runs below 4 GiB because the identity map of its own jump covers
  the first 4 GiB; it says so instead of triple faulting when the firmware
  puts it elsewhere.
