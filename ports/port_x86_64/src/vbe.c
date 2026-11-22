/*
 * Micro OS — the Bochs VBE interface of the VGA of QEMU.
 *
 * A linear framebuffer is the picture the machine can show, and the one the
 * display of QEMU offers to a guest without a driver for a card is the
 * "Bochs VBE" of the device 1234:1111: a register file on the I/O ports 0x1CE
 * and 0x1CF, and a window of the memory in the first BAR of the device
 * (ADR 0023).
 *
 * The interface is also what a guest of Bochs and of QEMU has always had, so
 * the code below is the small subset of it a console needs: the identifier,
 * the four registers of a mode and the enable. There is no VBE BIOS call, no
 * EDID and no list of modes - the port asks for the mode of ADR 0023 and the
 * answer is yes or no.
 *
 * The mode is RGB565, which is not a VBE format: VBE calls 16 bits per pixel
 * "5:6:5" and that is the format `k_renderer` of the kernel speaks, so the
 * picture needs no conversion on the way from the console to the memory.
 */
#include "port_x86_64_screen.h"

#include "kernel/log.h"
#include "port_x86_64_pci.h"

/* --- the registers of the interface --------------------------------------- */

#define VBE_INDEX_PORT 0x1CEu
#define VBE_DATA_PORT 0x1CFu

/* The identifier is in the register 0: 0xB0C0 is the first version of the
 * interface, 0xB0C5 the last, and anything else is not this interface. */
#define VBE_REG_ID 0x0u
#define VBE_REG_XRES 0x1u
#define VBE_REG_YRES 0x2u
#define VBE_REG_BPP 0x3u
#define VBE_REG_ENABLE 0x4u
#define VBE_REG_VIRT_WIDTH 0x6u
#define VBE_REG_VIRT_HEIGHT 0x7u
#define VBE_REG_MEMORY_64K 0xAu

#define VBE_ID_FIRST 0xB0C0u
#define VBE_ID_LAST 0xB0C5u

/* The enable register: the mode on, the window of the memory in use, and the
 * memory left as it is - the console clears the picture itself, which is one
 * pass over 2 MB either way. */
#define VBE_ENABLED 0x01u
#define VBE_LFB_ENABLED 0x40u
#define VBE_NOCLEARMEM 0x80u

/* The VGA of QEMU: the vendor 0x1234 and the device 0x1111 of the "VGA"
 * device of the machine, whose first BAR is the 16 MB of its memory. */
#define VGA_VENDOR_QEMU 0x1234u
#define VGA_DEVICE_STDVGA 0x1111u

#define VBE_WINDOW_MAX (16u * 1024u * 1024u)

static void vbe_write(k_u16_t index, k_u16_t value)
{
    port_outw(VBE_INDEX_PORT, index);
    port_outw(VBE_DATA_PORT, value);
}

static k_u16_t vbe_read(k_u16_t index)
{
    port_outw(VBE_INDEX_PORT, index);
    return port_inw(VBE_DATA_PORT);
}

/* The window of the picture: a memory BAR of the card that is at least as big
 * as the mode needs.
 *
 * The VGA of QEMU keeps the 16 MB of its memory in the first BAR, and the
 * screen is there. A card of the same interface may keep the picture in
 * another BAR (the "bochs-display" device has a small register window first
 * and the memory second), so the search walks the six registers and takes the
 * one whose size the device reports to be enough - a window too small would
 * put the pixels of the last lines into whatever the card keeps next. */
static bool vbe_window(k_u32_t needed, k_u32_t *phys)
{
    struct port_x86_64_pci_device vga;
    k_u8_t offset;

    if (!port_x86_64_pci_find(VGA_VENDOR_QEMU, VGA_DEVICE_STDVGA, &vga)) {
        return false;
    }

    /* The card decodes its memory from now on; a window the firmware left
     * disabled answers zero, and that is not a picture. */
    port_x86_64_pci_enable(&vga, PCI_COMMAND_MEMORY);

    for (offset = PCI_BAR0; offset < (k_u8_t)(PCI_BAR0 + 6u * 4u); offset = (k_u8_t)(offset + 4u)) {
        k_u32_t bar = port_x86_64_pci_read32(&vga, offset);
        k_u32_t size;

        if ((bar & 0x00000001u) != 0u || (bar & ~0x0Fu) == 0u) {
            continue;
        }

        size = port_x86_64_pci_bar_size(&vga, offset);

        if (size >= needed) {
            /* The C of the kernel reads 32 bits of the address, which is what
             * the port has of a BAR: a window above 4 GiB is not one it can
             * name. */
            *phys = bar & ~0x0Fu;

            return true;
        }
    }

    return false;
}

bool port_x86_64_vbe_mode(struct port_x86_64_screen_mode *mode)
{
    k_u16_t id;
    k_u16_t memory_64k;
    k_u32_t bytes;

    if (mode == NULL) {
        return false;
    }

    id = vbe_read(VBE_REG_ID);

    if (id < VBE_ID_FIRST || id > VBE_ID_LAST) {
        return false;
    }

    /*
     * The window is the memory of a mode of 16 bits per pixel: the picture
     * has one line of 1280 pixels of two bytes and 800 of them.
     */
    if (!vbe_window(PORT_X86_64_SCREEN_WIDTH * (PORT_X86_64_SCREEN_BPP / 8u) *
                        PORT_X86_64_SCREEN_HEIGHT,
                    &mode->phys)) {
        return false;
    }

    /*
     * The mode is written with the interface off and then turned on: the
     * registers of the size are not taken while the picture is being shown,
     * and the VGA of QEMU answers a read of them with what it would use.
     */
    vbe_write(VBE_REG_ENABLE, 0);
    vbe_write(VBE_REG_XRES, PORT_X86_64_SCREEN_WIDTH);
    vbe_write(VBE_REG_YRES, PORT_X86_64_SCREEN_HEIGHT);
    vbe_write(VBE_REG_BPP, PORT_X86_64_SCREEN_BPP);
    /* The virtual picture is the visible one: a width of its own makes the
     * line of the memory longer than the line of the glass, and a console
     * writes the picture it draws. */
    vbe_write(VBE_REG_VIRT_WIDTH, PORT_X86_64_SCREEN_WIDTH);
    vbe_write(VBE_REG_VIRT_HEIGHT, PORT_X86_64_SCREEN_HEIGHT);
    vbe_write(VBE_REG_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED | VBE_NOCLEARMEM);

    mode->width = vbe_read(VBE_REG_XRES);
    mode->height = vbe_read(VBE_REG_YRES);
    mode->bpp = (k_u8_t)vbe_read(VBE_REG_BPP);

    /* A mode the interface took is the mode it reads back: a card that says
     * 640x480 after 1280x800 is not the picture this port promised. */
    if (mode->width != PORT_X86_64_SCREEN_WIDTH || mode->height != PORT_X86_64_SCREEN_HEIGHT ||
        mode->bpp != PORT_X86_64_SCREEN_BPP) {
        return false;
    }

    mode->pitch = (k_u16_t)(mode->width * (mode->bpp / 8u));
    bytes = (k_u32_t)mode->pitch * mode->height;

    /* The memory the interface says it has: a mode that does not fit would
     * draw past the window and into whatever the card keeps next. */
    memory_64k = vbe_read(VBE_REG_MEMORY_64K);
    if (memory_64k != 0u) {
        k_u32_t limit = (k_u32_t)memory_64k * 64u * 1024u;

        if (bytes > limit) {
            kerror("screen: the mode %ux%u needs %u KB of a window of %u KB", mode->width,
                   mode->height, bytes / 1024u, limit / 1024u);
            return false;
        }
    }

    mode->bytes = bytes;

    return bytes <= VBE_WINDOW_MAX;
}
