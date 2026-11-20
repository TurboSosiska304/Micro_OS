/*
 * Micro OS — the block devices of the system.
 *
 * A table of pointers, no more: the devices live in the platform, the
 * kernel only knows where they are. The table is guarded by the critical
 * section because the shell may mount a device while another task is
 * enumerating them - the sections are three instructions long.
 */
#include "kernel/blockdev.h"

#include "kernel/port.h"

static const struct k_blockdev *g_devices[K_BLOCKDEV_MAX];
static k_size_t g_count;

int k_blockdev_register(const struct k_blockdev *dev)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL || dev->ops->write == NULL) {
        return -1;
    }

    if (dev->sector_size == 0 || dev->sectors == 0) {
        return -1;
    }

    k_port_enter_critical();

    if (g_count >= K_BLOCKDEV_MAX) {
        k_port_exit_critical();
        return -1;
    }

    g_devices[g_count++] = dev;

    k_port_exit_critical();

    return 0;
}

int k_blockdev_unregister(const struct k_blockdev *dev)
{
    k_size_t i;
    int rc = -1;

    if (dev == NULL) {
        return -1;
    }

    k_port_enter_critical();

    for (i = 0; i < g_count; i++) {
        if (g_devices[i] != dev) {
            continue;
        }

        /* The table keeps its order: the devices after this one move down a
         * place, so "the n-th device" still means what it meant. */
        for (; i + 1 < g_count; i++) {
            g_devices[i] = g_devices[i + 1];
        }

        g_devices[--g_count] = NULL;
        rc = 0;
        break;
    }

    k_port_exit_critical();

    return rc;
}

k_size_t k_blockdev_count(void)
{
    k_size_t count;

    k_port_enter_critical();
    count = g_count;
    k_port_exit_critical();

    return count;
}

const struct k_blockdev *k_blockdev_at(k_size_t index)
{
    const struct k_blockdev *dev = NULL;

    k_port_enter_critical();

    if (index < g_count) {
        dev = g_devices[index];
    }

    k_port_exit_critical();

    return dev;
}

const struct k_blockdev *k_blockdev_find(const char *name)
{
    const struct k_blockdev *found = NULL;
    k_size_t i;

    if (name == NULL) {
        return NULL;
    }

    k_port_enter_critical();

    for (i = 0; i < g_count; i++) {
        const char *other = g_devices[i]->name;
        k_size_t j = 0;

        while (other[j] != '\0' && name[j] != '\0' && other[j] == name[j]) {
            j++;
        }

        if (other[j] == '\0' && name[j] == '\0') {
            found = g_devices[i];
            break;
        }
    }

    k_port_exit_critical();

    return found;
}
