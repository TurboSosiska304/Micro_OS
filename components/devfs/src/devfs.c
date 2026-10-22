/*
 * Micro OS — the devfs.
 *
 * The devices of the system: the console of the board and the sink that
 * throws everything away.
 */
#include "devfs/devfs.h"

#include <stdio.h>
#include <string.h>

#include "kernel/port.h"
#include "kernel/vfs.h"

#define DEV_NAME_MAX 16

struct dev_file {
    const char *name;
    int (*read)(char *buf, k_size_t size);
    int (*write)(const char *buf, k_size_t size);
};

/* The console of the board. */
static int tty_read(char *buf, k_size_t size)
{
    int rc = k_port_console_readline(buf, size);

    return rc < 0 ? 0 : rc;
}

static int tty_write(const char *buf, k_size_t size)
{
    k_port_write(buf, size);
    return (int)size;
}

/* The sink: everything that is written disappears. */
static int null_read(char *buf, k_size_t size)
{
    (void)buf;
    (void)size;
    return 0;
}

static int null_write(const char *buf, k_size_t size)
{
    (void)buf;
    return (int)size;
}

static const struct dev_file s_devices[] = {
    {"tty0", tty_read, tty_write},
    {"null", null_read, null_write},
};

static const k_size_t s_device_count = K_ARRAY_SIZE(s_devices);

static const struct dev_file *find_device(const char *name)
{
    k_size_t i;

    for (i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].name, name) == 0) {
            return &s_devices[i];
        }
    }

    return NULL;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return path;
    }

    return slash + 1;
}

static int devfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    const char *name = base_name(path);
    const struct dev_file *dev = find_device(name);
    const struct dev_file **slot;

    K_UNUSED(ctx);
    K_UNUSED(flags);

    if (dev == NULL) {
        return -2;
    }

    /* The device is remembered, not its name: the table can change. */
    slot = (const struct dev_file **)out->priv;
    if (slot != NULL) {
        *slot = dev;
    }

    return 0;
}

static int devfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev == NULL || dev->read == NULL) {
        return -2;
    }

    return dev->read((char *)buf, size);
}

static int devfs_write(void *ctx, struct vfs_file *file, const void *buf, k_size_t size)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev == NULL || dev->write == NULL) {
        return -2;
    }

    return dev->write((const char *)buf, size);
}

static int devfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                      k_size_t *count)
{
    k_size_t i;
    k_size_t found = 0;

    K_UNUSED(ctx);
    K_UNUSED(path);

    for (i = 0; i < s_device_count && found < max; i++) {
        memset(&out[found], 0, sizeof(out[found]));
        snprintf(out[found].name, sizeof(out[found].name), "%s", s_devices[i].name);
        out[found].type = 3; /* a device */
        found++;
    }

    if (count != NULL) {
        *count = found;
    }

    return 0;
}

static int devfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    const struct dev_file *dev = find_device(base_name(path));

    K_UNUSED(ctx);

    if (dev == NULL) {
        return -2;
    }

    memset(out, 0, sizeof(*out));
    out->type = 3;

    return 0;
}

static const struct vfs_operations g_operations = {
    .open = devfs_open,
    .read = devfs_read,
    .write = devfs_write,
    .close = NULL,
    .list = devfs_list,
    .stat = devfs_stat,
    .mkdir = NULL,
    .unlink = NULL,
    .strerror = NULL,
};

int devfs_register(const char *point)
{
    return vfs_mount(point, &g_operations, NULL);
}
