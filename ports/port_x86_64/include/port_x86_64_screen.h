/*
 * Micro OS — the screen of the x86-64 port: a framebuffer, not a GPU.
 *
 * The machine has had one screen since 1.0: the text mode of the VGA, 80 by
 * 25 cells at 0xB8000. ADR 0023 chooses the next one - the linear framebuffer
 * of the VGA of QEMU, programmed through the Bochs VBE interface to 1280x800
 * and 16 bits per pixel in RGB565 - and keeps the text mode as the fallback
 * of a machine whose interface does not answer.
 *
 * This header is the half of the screen that is the device: the interface of
 * the mode (src/vbe.c) and the picture the console draws on (src/fb.c). The
 * console itself is not here: it counts cells and knows nothing about a mode
 * (src/console.c).
 */
#ifndef MICRO_OS_PORT_X86_64_SCREEN_H
#define MICRO_OS_PORT_X86_64_SCREEN_H

#include "kernel/fb.h"
#include "kernel/fb_ops.h"
#include "kernel/types.h"
#include "port_x86_64.h"

/* The mode the port asks of the Bochs VBE interface (ADR 0023). */
#define PORT_X86_64_SCREEN_WIDTH 1280u
#define PORT_X86_64_SCREEN_HEIGHT 800u
#define PORT_X86_64_SCREEN_BPP 16u

/*
 * Where the window of the framebuffer is mapped: the top 2 GiB, the region
 * the C of the kernel can name with one immediate (ADR 0007), half a
 * gigabyte above the image and well below the end of the PD that carries it.
 * A picture of about 2 MB is one or two 2 MiB pages there.
 */
#define PORT_X86_64_SCREEN_VIRT (PORT_X86_64_KERNEL_BASE + 0x20000000ull)

/* The 2 MiB of one page of the map: a window of a device is mapped in whole
 * pages of that size and the offset of the frame inside its first one is
 * added back to the virtual address (ADR 0007). */
#define PORT_X86_64_SCREEN_PAGE 0x200000ull

/* The mode as the machine answered it: where the picture is, and its shape. */
struct port_x86_64_screen_mode {
    k_u64_t phys;     /* the window of the framebuffer: a BAR of the VGA, or the
                       * FrameBufferBase of the graphics output of a firmware */
    k_u32_t bytes;    /* the picture as the mode left it */
    k_u16_t width;    /* pixels of a line */
    k_u16_t height;   /* lines */
    k_u32_t pitch;    /* bytes of a line */
    k_u8_t bpp;       /* bits of a pixel, 16 or 32 */
    k_u8_t format;    /* PORT_X86_64_PIXEL_*, the format of a pixel */
    k_u32_t mask_red; /* the masks of PORT_X86_64_PIXEL_MASK16/32 */
    k_u32_t mask_green;
    k_u32_t mask_blue;
};

/*
 * Ask the Bochs VBE interface of the VGA for the mode of ADR 0023: the
 * device 1234:1111 on the PCI bus, the registers 0x1CE and 0x1CF, 1280x800
 * and 16 bits per pixel. False when the machine has no such interface
 * (another card, no card), when the interface answers with an identifier it
 * does not know, or when the mode did not read back - the caller then keeps
 * the text mode. The mode is set even when the caller cannot map it: the
 * screen of the firmware is gone at that point, and a caller that fails after
 * this call has to say so rather than expect the old mode back.
 */
bool port_x86_64_vbe_mode(struct port_x86_64_screen_mode *mode);

/*
 * The picture a firmware of UEFI programmed, when the boot was that one: the
 * mode the loader chose (EFI_GRAPHICS_OUTPUT_PROTOCOL, loader/uefi), its
 * frame mapped uncached in the top 2 GiB, and - for a frame that is not
 * already RGB565, which is the format the console of the kernel draws in -
 * a shadow of the picture in that format and the conversion of the cells that
 * changed into the frame of the firmware (src/gop.c). NULL when the boot was
 * the BIOS path, when the firmware gave no picture, or when the frame could
 * not be mapped; the caller then asks the VBE interface and falls back to the
 * text mode, exactly as it did before a UEFI machine existed.
 *
 * The screen answers with the grid of cells the console counts in and its
 * own name, which says the size, the format and the cells of the picture.
 */
const struct port_x86_64_screen *port_x86_64_gop_screen(void);

/*
 * The rectangle of a frame that a caller outside the console has changed:
 * `/dev/fb0` is a picture of a program, and the port draws it through the
 * same shadow (src/fb.c) as the console, so the bytes arrive at the glass
 * through the conversion of a rectangle of pixels. The port's own device
 * (src/fbdev.c) calls it after a blit; a screen that is not a firmware's
 * frame does nothing.
 */
void port_x86_64_gop_present(const struct fb_rect *rect);

/*
 * The screen of the framebuffer: the renderer of ADR 0023 (kernel/render.h,
 * RGB565, the 8x16 font of the PC) and the grid of cells the console draws
 * into. `base` is the virtual address of the first byte of the picture, which
 * the paging code of the port mapped. NULL when the mode is not one a renderer
 * of this port speaks.
 */
const struct port_x86_64_screen *port_x86_64_fb_screen(const struct port_x86_64_screen_mode *mode,
                                                       void *base);

/*
 * The gap between two lines of the picture, in pixels: the `line_gap` of
 * /etc/font, 0, 2 or 4 (ADR 0021). The step of a line becomes 16 plus the gap
 * and the rows of the grid are counted again - 50, 44 or 40 rows of a picture
 * of 800 lines - and the picture is drawn again from the cells the console
 * holds. Does nothing when the screen is the text mode of the VGA.
 */
void port_x86_64_fb_set_line_gap(int gap);

/* Register the `font` system of /etc (the line gap) with the machine. */
int port_x86_64_font_settings_register(void);

/*
 * The framebuffer as the device `/dev/fb0` (ADR 0028): the description of the
 * picture, one rectangle of pixels, and the two commands that take the
 * console's screen and give it back. False from the first when the machine
 * has no framebuffer, which is the text mode of the fallback.
 */
bool port_x86_64_fb_info(struct fb_info *out);
/*
 * The memory of the picture as the contract of an operation of a device
 * (kernel/fb_ops.h): the software path of `/dev/fb0` and of the console is
 * one `k_fb_blit` or `k_fb_fill` over this surface. False when the machine
 * has no framebuffer.
 */
bool port_x86_64_fb_surface(struct k_fb_surface *out);
/* Override the reported framebuffer format when the display plane changes. */
void port_x86_64_fb_set_format(k_u8_t format, k_u8_t bpp);
void port_x86_64_fb_console_take(void);
void port_x86_64_fb_console_give(void);
void port_x86_64_fb_console_force(void);

/*
 * Put the device `/dev/fb0` in the devfs. Called once at the boot, after the
 * devfs is mounted; a machine without a framebuffer registers nothing.
 */
int port_x86_64_fbdev_register(void);

#endif /* MICRO_OS_PORT_X86_64_SCREEN_H */
