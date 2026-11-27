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
 * The write is the contract of `kernel/fb_ops.h`: the device of the picture
 * may fill and copy the pixels itself, and the software path is the one that
 * answers when it has no such operation. `FBIO_PRESENT` carries the same
 * contract one step further: a complete RAM surface may become the displayed
 * surface without a copy, with the same software fallback when it cannot.
 *
 * There is no clipping: a rectangle that leaves the picture is refused, so a
 * drawing bug of a program is a visible error and not a picture that almost
 * fits. The size of one write is the size of one transfer of the system
 * (KSYS_MAX_IO), so an interface that repaints a window writes it in strips;
 * the border of the contract says so and the first UI does it.
 */
#include <string.h>

#include "devfs/devfs.h"
#include "kernel/blit.h"
#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/fb_ops.h"
#include "kernel/log.h"
#include "kernel/port/exec.h"
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
    struct k_fb_rect destination;
    struct k_fb_surface surface;
    const k_u16_t *pixels;
    int rc;

    if (buf == NULL) {
        return KERR_INVAL;
    }

    if (!port_x86_64_fb_info(&info) || !port_x86_64_fb_surface(&surface)) {
        return KERR_INVAL;
    }

    rc = fb_rect_parse(&info, buf, size, &rect, &pixels);
    if (rc != 0) {
        return rc;
    }

    /*
     * The rectangle of the program goes through the contract of the picture
     * (kernel/fb_ops.h): the device copies it itself when it can, and the
     * software path of 1.6 - the `memcpy` into the memory the display reads -
     * is the whole of it when it cannot. The bytes on the screen are the same
     * either way.
     */
    destination.x = rect.x;
    destination.y = rect.y;
    destination.w = rect.w;
    destination.h = rect.h;

    rc = k_fb_blit(&surface, &destination, pixels, rect.w, 0, 0);
    if (rc != 0) {
        return rc;
    }

    port_x86_64_gop_present_rect(rect.x, rect.y, rect.w, rect.h);
    return (int)size;
}

static int fbdev_present(k_u64_t argument)
{
    struct fb_present_surface request;
    struct fb_info info;
    struct k_fb_surface destination;
    struct k_fb_surface source;
    k_u64_t bytes;

    if (!k_port_exec_contains((const void *)(uintptr_t)argument, sizeof(request))) {
        return KERR_FAULT;
    }

    k_port_exec_user_access_begin();
    memcpy(&request, (const void *)(uintptr_t)argument, sizeof(request));
    k_port_exec_user_access_end();

    if (request.base == 0u || request.width == 0u || request.height == 0u ||
        request.pitch < request.width || !port_x86_64_fb_info(&info) ||
        !port_x86_64_fb_surface(&destination) || request.width != info.width ||
        request.height != info.height || request.pitch != destination.pitch) {
        return KERR_INVAL;
    }

    bytes = (k_u64_t)request.pitch * (k_u64_t)request.height * (k_u64_t)fb_pixel_bytes(info.format);
    if (bytes > (k_u64_t)((k_size_t)-1)) {
        return KERR_TOOBIG;
    }

    source.base = (k_u16_t *)(uintptr_t)request.base;
    source.width = request.width;
    source.height = request.height;
    source.pitch = request.pitch;
    source.format = info.format;
    destination.format = info.format;

    {
        int present_rc = k_blit_present(&destination, &source);

        if (present_rc == 1) {
            port_x86_64_gop_present_rect(0, 0, (int)info.width, (int)info.height);
        }
        return present_rc;
    }
}

static int fbdev_ioctl(k_u32_t request, k_u64_t arg)
{
    switch (request) {
    case FBIO_TAKE:
        port_x86_64_fb_console_take();
        kinfo("fb: the console is taken by a program, the picture is its own");
        return 0;
    case FBIO_GIVE:
        /*
         * The screen is one resource and a program may have started another
         * one: the give of a program that is not the last holder of the
         * device changes nothing, so the picture of a file manager survives
         * the editor it started.
         */
        if (g_open_count <= 1) {
            (void)k_blit_present_release();
            port_x86_64_fb_console_give();
            kinfo("fb: the console is given back and the grid repainted");
        }
        return 0;
    case FBIO_PRESENT:
        return fbdev_present(arg);
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
        (void)k_blit_present_release();
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
