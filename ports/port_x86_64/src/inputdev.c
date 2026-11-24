/*
 * Micro OS — `/dev/input0`, the keyboard as a device (ADR 0028).
 *
 * A program reads `struct input_event` (a moment, a key, pressed or released)
 * and gets EAGAIN when the user is silent, the rule of a pipe with a producer
 * (ADR 0014). The events are made by the handler of IRQ1 (src/ps2.c); this
 * file is the shape of the file and the lifetime of the keyboard: an open
 * takes the keys away from the console, the last close gives them back.
 *
 * The device is the reason the interface can exist without a terminal layer:
 * the console of the kernel is characters for a shell, and an interface that
 * moves a focus on Tab needs the keys themselves.
 */
#include <string.h>

#include "devfs/devfs.h"
#include "kernel/errno.h"
#include "kernel/input.h"
#include "kernel/log.h"
#include "kernel/types.h"
#include "port_x86_64_input.h"
#include "port_x86_64_xhci.h"

static int g_open_count;

static int inputdev_read(char *buf, k_size_t size)
{
    k_size_t take = 0;

    if (buf == NULL) {
        return KERR_INVAL;
    }

    if (size < sizeof(struct input_event)) {
        return KERR_INVAL;
    }

    while (take + sizeof(struct input_event) <= size) {
        struct input_event event;

        /* The keyboard of the PS/2 first and the one of the USB after it:
         * a machine has one of the two and the reader of the device does not
         * know which (ADR 0040). */
        if (!port_x86_64_input_event(&event) && !port_x86_64_usb_event(&event)) {
            break;
        }

        memcpy(buf + take, &event, sizeof(event));
        take += sizeof(event);
    }

    /* No event of the user is not an end of the input: the reader comes back
     * after a nap, which is what an interface of a keyboard does. */
    if (take == 0) {
        return KERR_AGAIN;
    }

    return (int)take;
}

static void inputdev_open(void)
{
    if (g_open_count == 0) {
        port_x86_64_input_capture(true);
        port_x86_64_usb_capture(true);
    }

    g_open_count++;
}

static void inputdev_close(void)
{
    if (g_open_count > 0) {
        g_open_count--;
    }

    if (g_open_count == 0) {
        port_x86_64_input_capture(false);
        port_x86_64_usb_capture(false);
    }
}

static const struct devfs_operations s_operations = {
    .read = inputdev_read,
    .open = inputdev_open,
    .close = inputdev_close,
};

void port_x86_64_input_register(void)
{
    if (devfs_add_device("input0", &s_operations) != 0) {
        kwarn("input: cannot add /dev/input0 to the devfs");
    }
}
