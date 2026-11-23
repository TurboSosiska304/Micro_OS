/*
 * Micro OS — `/dev/fb0`, the framebuffer as a device (ADR 0028).
 *
 * A program that wants a screen reads the description of the picture, writes
 * rectangles of pixels into it and asks the kernel for the screen with
 * FBIO_TAKE; an interface is a program and not a second kernel. The device is
 * thin on purpose: the picture is `src/fb.c` (the renderer and the grid of the
 * console), and this file is the shape of the file - a `struct fb_info` on a
 * read, a `struct fb_rect` and its pixels on a write, two control commands -
 * and the rule that a program that closes the device gives the screen back.
 *
 * There is no clipping: a rectangle that leaves the picture is refused, so a
 * drawing bug of a program is a visible error and not a picture that almost
 * fits. The size of one write is the size of one transfer of the system
 * (KSYS_MAX_IO), so an interface that repaints a window writes it in strips;
 * the border of the contract says so and the first UI does it.
 */
#include <string.h>

#include "devfs/devfs.h"
#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/log.h"
#include "kernel/types.h"
#include "port_x86_64_screen.h"

/* How many descriptors name the device. The screen is one resource: the last
 * close gives it back even when a program forgot FBIO_GIVE or died. */
static int g_open_count;

static int fbdev_read(char *buf, k_size_t size)
{
    struct fb_info info;
    k_size_t take;

    if (buf == NULL) {
        return KERR_INVAL;
    }

    if (!port_x86_64_fb_info(&info)) {
        return KERR_INVAL;
    }

    take = size < sizeof(info) ? size : sizeof(info);
    memcpy(buf, &info, take);

    return (int)take;
}

static int fbdev_write(const char *buf, k_size_t size)
{
    struct fb_info info;
    struct fb_rect rect;
    const k_u16_t *pixels;
    int rc;

    if (buf == NULL) {
        return KERR_INVAL;
    }

    if (!port_x86_64_fb_info(&info)) {
        return KERR_INVAL;
    }

    rc = fb_rect_parse(&info, buf, size, &rect, &pixels);
    if (rc != 0) {
        return rc;
    }

    rc = port_x86_64_fb_blit(&rect, pixels);
    if (rc != 0) {
        return rc;
    }

    return (int)size;
}

static int fbdev_ioctl(k_u32_t request, k_u64_t arg)
{
    K_UNUSED(arg);

    switch (request) {
    case FBIO_TAKE:
        port_x86_64_fb_console_take();
        kinfo("fb: the console is taken by a program, the picture is its own");
        return 0;
    case FBIO_GIVE:
        port_x86_64_fb_console_give();
        kinfo("fb: the console is given back and the grid repainted");
        return 0;
    default:
        return KERR_INVAL;
    }
}

static void fbdev_open(void)
{
    g_open_count++;
}

static void fbdev_close(void)
{
    if (g_open_count > 0) {
        g_open_count--;
    }

    /* A program that closed the framebuffer is a program that is done with
     * the picture, whether it asked or not. */
    if (g_open_count == 0) {
        port_x86_64_fb_console_give();
    }
}

static const struct devfs_operations s_operations = {
    .read = fbdev_read,
    .write = fbdev_write,
    .ioctl = fbdev_ioctl,
    .open = fbdev_open,
    .close = fbdev_close,
};

int port_x86_64_fbdev_register(void)
{
    struct fb_info info;

    /* The text mode of the VGA is the console of the port and not a picture
     * with a shape a program may draw in: a machine without the framebuffer
     * has no /dev/fb0 and the interface says so when it opens it. */
    if (!port_x86_64_fb_info(&info)) {
        return 0;
    }

    if (devfs_add_device("fb0", &s_operations) != 0) {
        kwarn("fb: cannot add /dev/fb0 to the devfs");
        return -1;
    }

    return 0;
}
