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

/* The mode as the machine answered it: where the picture is, and its shape. */
struct port_x86_64_screen_mode {
    k_u32_t phys;   /* the window of the framebuffer, from the BAR of the VGA */
    k_u32_t bytes;  /* the picture as the mode left it */
    k_u16_t width;  /* pixels of a line */
    k_u16_t height; /* lines */
    k_u16_t pitch;  /* bytes of a line */
    k_u8_t bpp;     /* bits of a pixel, 16 in the mode of ADR 0023 */
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
void port_x86_64_fb_console_take(void);
void port_x86_64_fb_console_give(void);
void port_x86_64_fb_console_force(void);

/*
 * Put the device `/dev/fb0` in the devfs. Called once at the boot, after the
 * devfs is mounted; a machine without a framebuffer registers nothing.
 */
int port_x86_64_fbdev_register(void);

#endif /* MICRO_OS_PORT_X86_64_SCREEN_H */
