/*
 * Micro OS — the devfs.
 */
#ifndef MICRO_OS_DEVFS_DEVFS_H
#define MICRO_OS_DEVFS_DEVFS_H

#include "kernel/types.h"

/* Mount the devfs at the given point of the tree. */
int devfs_register(const char *point);

/*
 * The operations of one device. A device is a file whose read, write and
 * control commands a driver answers; a field that is empty is a call the
 * device does not have, and the caller learns EINVAL from the VFS. `open` and
 * `close` run once for every descriptor that names the device, which is how a
 * device that takes the keyboard knows that somebody is listening.
 */
struct devfs_operations {
    int (*read)(char *buf, k_size_t size);
    int (*write)(const char *buf, k_size_t size);
    int (*ioctl)(k_u32_t request, k_u64_t arg);
    void (*open)(void);
    void (*close)(void);
};

/*
 * Add a device to the table of the devfs with the operations of its driver:
 * "input0" here means the file /dev/input0 once the devfs is mounted. The name
 * has to stay alive (a string literal of the driver does) and a name belongs
 * to exactly one device. Call it at boot, before anybody opens a file.
 *
 * Returns 0, or -1 when the name is empty or taken, when the device has no
 * operation at all, or when the table is full.
 */
int devfs_add_device(const char *name, const struct devfs_operations *operations);

/*
 * Add a device that only reads and writes; the form the drivers of the board
 * use. It is devfs_add_device() with the other three operations empty.
 */
int devfs_add(const char *name, int (*read)(char *buf, k_size_t size),
              int (*write)(const char *buf, k_size_t size));

#endif /* MICRO_OS_DEVFS_DEVFS_H */
