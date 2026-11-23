/*
 * Micro OS — what the UEFI loader hands the kernel of the x86-64 port.
 *
 * The loader of the BIOS path (loader32.S) gives the kernel one address: the
 * multiboot 1 information, which names the modules of the boot and the map of
 * the memory. A machine that starts under UEFI has one thing more to say -
 * the picture the firmware has already programmed, the root pointer of the
 * ACPI tables the firmware keeps out of the extended BIOS area, and where in
 * the memory the loader put the image - so the boot of that machine passes a
 * structure of this file before it.
 *
 * The two boots are told apart by the second argument of the entry: the
 * loader of the BIOS path is written to leave it zero, and the UEFI loader
 * puts PORT_X86_64_BOOT_MAGIC there. A structure of this file is therefore
 * never mistaken for the multiboot information, which is checked first for the
 * magic and would fail the test.
 *
 * The multiboot information travels inside this structure as well: the port
 * reads the modules of the boot (the kernel and the disk image) and the map
 * of the memory through the same code on both machines, so a machine that
 * boots under UEFI has a disk without a driver for the controller the disk is
 * on. The map the loader writes is the one the firmware described, with the
 * regions it can be handed as usable memory and the rest as reserved.
 *
 * This header is read by the loader, which has no libc and no header of this
 * tree, so it uses the plain widths of C and not `k_u64_t`.
 */
#ifndef MICRO_OS_PORT_X86_64_BOOT_H
#define MICRO_OS_PORT_X86_64_BOOT_H

/* "MOSUEFI", little endian. */
#define PORT_X86_64_BOOT_MAGIC 0x4D4F535855454649ull
#define PORT_X86_64_BOOT_VERSION 1u

/*
 * The format of a pixel of the frame the firmware programmed. The kernel's
 * console speaks RGB565 (kernel/render.h), so a frame of 32 bits per pixel is
 * written through a shadow and converted on the way to the glass
 * (src/gop.c); a frame that is already 565 is written by the console itself.
 * The two 32-bit formats are the two of the UEFI specification and they
 * differ in the order of the bytes of a pixel, not in the number of bits.
 */
#define PORT_X86_64_PIXEL_RGB565 0u /* 16 bits: 5 red, 6 green, 5 blue */
#define PORT_X86_64_PIXEL_RGBX32 1u /* 32 bits, the red byte first */
#define PORT_X86_64_PIXEL_BGRX32 2u /* 32 bits, the blue byte first */
#define PORT_X86_64_PIXEL_MASK16 3u /* 16 bits, the masks below */
#define PORT_X86_64_PIXEL_MASK32 4u /* 32 bits, the masks below */

struct port_x86_64_boot_info {
    unsigned long long magic;   /* PORT_X86_64_BOOT_MAGIC */
    unsigned long long version; /* PORT_X86_64_BOOT_VERSION */

    /* The multiboot 1 information: the modules (the kernel is 0, the disk
     * image 1) and the map of the memory, in the format the port reads. */
    unsigned long long mb_info;

    /* Where the loader copied the bytes of the kernel. The image is linked at
     * PORT_X86_64_KERNEL_BASE and the loader may place it anywhere below the
     * arena of the programs, so the port asks instead of assuming 16 MiB. */
    unsigned long long kernel_phys;

    /* The picture of the firmware: EFI_GRAPHICS_OUTPUT_PROTOCOL. */
    unsigned long long fb_phys; /* FrameBufferBase */
    unsigned long long fb_size; /* FrameBufferSize, the bytes of the frame */
    unsigned int fb_width;      /* pixels of a visible line */
    unsigned int fb_height;     /* visible lines */
    unsigned int fb_pitch;      /* bytes of a line of the memory */
    unsigned int fb_bpp;        /* bits of a pixel, 16 or 32 */
    unsigned int fb_format;     /* PORT_X86_64_PIXEL_* */
    unsigned int fb_mask_red;   /* the masks of PORT_X86_64_PIXEL_MASK* */
    unsigned int fb_mask_green;
    unsigned int fb_mask_blue;

    /* The root pointer of the ACPI tables (EFI_ACPI_20_TABLE_GUID), 0 when
     * the firmware did not offer one. */
    unsigned long long rsdp;
};

#endif /* MICRO_OS_PORT_X86_64_BOOT_H */
