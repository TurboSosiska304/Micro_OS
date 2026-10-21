/*
 * Micro OS — the devfs.
 */
#ifndef MICRO_OS_DEVFS_DEVFS_H
#define MICRO_OS_DEVFS_DEVFS_H

#include "kernel/types.h"

/* Mount the devfs at the given point of the tree. */
int devfs_register(const char *point);

#endif /* MICRO_OS_DEVFS_DEVFS_H */
