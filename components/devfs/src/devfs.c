/*
 * Micro OS — the devfs.
 *
 * The devices of the system: the console of the board and the sink that
 * throws everything away.
 */
#include "devfs/devfs.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/tty.h"
#include "kernel/vfs.h"

#define DEV_NAME_MAX 16

/*
 * The devices that the system registers while it boots, beside the two
 * that are always there. The registration is done once, at boot, before
 * anybody opens a file: the table does not need a lock for that.
 */
#define DEV_EXTRA_MAX 4

struct dev_file {
    const char *name;
    struct devfs_operations operations;
};

/* The console of the board: the terminal of the task that opens it
 * (kernel/tty.h). `read` is the line discipline - a line of a person with the
 * echo and the editing of a terminal, or the raw bytes of a program that put
 * the terminal in raw mode - and `write` is the console of the machine. */
static int tty_read(char *buf, k_size_t size)
{
    int rc = k_tty_read(buf, size);

    /* An empty line is a read of no bytes and not the end of the file: the
     * device answers zero, and a negative answer is the error of the call. */
    return rc;
}

static int tty_write(const char *buf, k_size_t size)
{
    k_console_write(buf, size);
    return (int)size;
}

/* The mode of the terminal, the size of its screen and the bytes that wait:
 * TCGETS, TCSETS, TIOCGWINSZ and FIONREAD (ADR 0051 §3). */
static int tty_ioctl(k_u32_t request, k_u64_t arg)
{
    return k_tty_ioctl(request, arg);
}

static void tty_open(void)
{
    k_tty_open();
}

static void tty_close(void)
{
    k_tty_close();
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
    {"tty0",
     {.read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .open = tty_open, .close = tty_close}},
    {"tty",
     {.read = tty_read, .write = tty_write, .ioctl = tty_ioctl, .open = tty_open, .close = tty_close}},
    {"null", {.read = null_read, .write = null_write}},
};

static const k_size_t s_device_count = K_ARRAY_SIZE(s_devices);

/* The devices that were added by the drivers of the board. */
static struct dev_file s_extra[DEV_EXTRA_MAX];
static k_size_t s_extra_count;

/* The point the filesystem is mounted at: its own root is a directory even
 * though no device is called by the name of the point. */
static char s_point[VFS_PATH_MAX];

/* The two tables are read as one: the fixed ones first, the added after. */
static k_size_t device_count(void)
{
    return s_device_count + s_extra_count;
}

static const struct dev_file *device_at(k_size_t index)
{
    if (index < s_device_count) {
        return &s_devices[index];
    }

    index -= s_device_count;
    return (index < s_extra_count) ? &s_extra[index] : NULL;
}

static const struct dev_file *find_device(const char *name)
{
    k_size_t i;

    for (i = 0; i < device_count(); i++) {
        const struct dev_file *dev = device_at(i);

        if (dev != NULL && strcmp(dev->name, name) == 0) {
            return dev;
        }
    }

    return NULL;
}

int devfs_add_device(const char *name, const struct devfs_operations *operations)
{
    if (name == NULL || name[0] == '\0' || operations == NULL) {
        return -1;
    }

    if (operations->read == NULL && operations->write == NULL && operations->ioctl == NULL &&
        operations->open == NULL && operations->close == NULL) {
        return -1; /* a device nobody can talk to is not a device */
    }

    if (find_device(name) != NULL) {
        return -1; /* the name of a device belongs to exactly one */
    }

    if (s_extra_count >= DEV_EXTRA_MAX) {
        return -1;
    }

    /*
     * The name is remembered as it is: the caller has to keep it alive
     * (a string literal of a driver does). The operations are copied, so a
     * driver may register a table of its own. A driver registers at boot,
     * before anybody opens the device.
     */
    s_extra[s_extra_count].name = name;
    s_extra[s_extra_count].operations = *operations;
    s_extra_count++;

    return 0;
}

int devfs_add(const char *name, int (*read)(char *buf, k_size_t size),
              int (*write)(const char *buf, k_size_t size))
{
    struct devfs_operations operations;

    memset(&operations, 0, sizeof(operations));
    operations.read = read;
    operations.write = write;

    return devfs_add_device(name, &operations);
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

    /* The point of the mount is a directory and is opened for `readdir`; a
     * device is the node below it. */
    if ((flags & VFS_O_DIRECTORY) != 0) {
        if (s_point[0] == '\0' || strcmp(path, s_point) != 0) {
            return -2;
        }
        if (out->priv != NULL) {
            memset(out->priv, 0, VFS_FILE_DATA_SIZE);
        }

        return 0;
    }

    if (dev == NULL) {
        return -2;
    }

    /* The device is remembered, not its name: the table can change. */
    slot = (const struct dev_file **)out->priv;
    if (slot != NULL) {
        *slot = dev;
    }

    /* A device that takes a resource for as long as it is open hears about
     * it here: `/dev/input0` is the keyboard of the interface while the
     * interface holds it (ADR 0028). */
    if (dev->operations.open != NULL) {
        dev->operations.open();
    }

    return 0;
}

static int devfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev == NULL || dev->operations.read == NULL) {
        return -2;
    }

    return dev->operations.read((char *)buf, size);
}

static int devfs_write(void *ctx, struct vfs_file *file, const void *buf, k_size_t size)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev == NULL || dev->operations.write == NULL) {
        return -2;
    }

    return dev->operations.write((const char *)buf, size);
}

static int devfs_ioctl(void *ctx, struct vfs_file *file, k_u32_t request, k_u64_t arg)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev == NULL || dev->operations.ioctl == NULL) {
        return KERR_INVAL;
    }

    return dev->operations.ioctl(request, arg);
}

static int devfs_close(void *ctx, struct vfs_file *file)
{
    const struct dev_file *const *slot = (const struct dev_file *const *)file->priv;
    const struct dev_file *dev = (slot != NULL) ? *slot : NULL;

    K_UNUSED(ctx);

    if (dev != NULL && dev->operations.close != NULL) {
        dev->operations.close();
    }

    return 0;
}

static int devfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                      k_size_t *count)
{
    k_size_t i;
    k_size_t found = 0;

    K_UNUSED(ctx);
    K_UNUSED(path);

    for (i = 0; i < device_count() && found < max; i++) {
        const struct dev_file *dev = device_at(i);

        if (dev == NULL) {
            continue;
        }

        memset(&out[found], 0, sizeof(out[found]));
        snprintf(out[found].name, sizeof(out[found].name), "%s", dev->name);
        out[found].type = 3; /* a device */
        found++;
    }

    if (count != NULL) {
        *count = found;
    }

    return 0;
}

/*
 * The next entries of /dev: the position of the descriptor is the number of
 * devices already answered, and the table is walked from its start.
 */
static int devfs_readdir(void *ctx, struct vfs_file *file, struct vfs_dirent *out, k_size_t max,
                         k_size_t *count)
{
    k_size_t i;
    k_size_t found = 0;
    k_size_t seen = 0;
    k_size_t skip = (k_size_t)file->offset;

    K_UNUSED(ctx);

    for (i = 0; i < device_count() && found < max; i++) {
        const struct dev_file *dev = device_at(i);

        if (dev == NULL) {
            continue;
        }
        if (seen++ < skip) {
            continue;
        }

        memset(&out[found], 0, sizeof(out[found]));
        snprintf(out[found].name, sizeof(out[found].name), "%s", dev->name);
        out[found].type = 3; /* a device */
        found++;
    }

    file->offset += seen;
    if (count != NULL) {
        *count = found;
    }

    return 0;
}

static int devfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    const struct dev_file *dev;

    K_UNUSED(ctx);

    /* The point of the mount is a directory of the tree even though no
     * device is called by its name: `ls /dev` stats the point before it
     * lists the devices below it. */
    if (s_point[0] != '\0' && strcmp(path, s_point) == 0) {
        memset(out, 0, sizeof(*out));
        out->type = 2;
        return 0;
    }

    dev = find_device(base_name(path));

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
    .ioctl = devfs_ioctl,
    .truncate = NULL, /* a device is not a file that can be cut */
    .close = devfs_close,
    .list = devfs_list,
    .readdir = devfs_readdir,
    .stat = devfs_stat,
    .mkdir = NULL,
    .unlink = NULL,
    .strerror = NULL,
};

int devfs_register(const char *point)
{
    k_size_t len;
    int rc;

    if (point == NULL) {
        return -1;
    }

    len = strlen(point);
    if (len >= sizeof(s_point)) {
        return -1;
    }

    rc = vfs_mount(point, &g_operations, NULL);
    if (rc != 0) {
        return rc;
    }

    memcpy(s_point, point, len + 1);

    return 0;
}
