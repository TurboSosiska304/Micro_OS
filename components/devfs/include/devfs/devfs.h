/*
 * Micro OS — the devfs.
 */
#ifndef MICRO_OS_DEVFS_DEVFS_H
#define MICRO_OS_DEVFS_DEVFS_H

#include "kernel/types.h"

/* Mount the devfs at the given point of the tree. */
int devfs_register(const char *point);

/*
 * Add a device to the table of the devfs: "input" here means the file
 * /dev/input once the devfs is mounted. The name has to stay alive (a
 * string literal of the driver does) and a name belongs to exactly one
 * device. Call it at boot, before anybody opens a file.
 *
 * Returns 0, or -1 when the name is empty or taken or the table is full.
 */
int devfs_add(const char *name, int (*read)(char *buf, k_size_t size),
              int (*write)(const char *buf, k_size_t size));

#endif /* MICRO_OS_DEVFS_DEVFS_H */
