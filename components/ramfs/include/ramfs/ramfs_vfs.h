/*
 * Micro OS — the ramfs, the adapter to the VFS.
 */
#ifndef MICRO_OS_RAMFS_RAMFS_VFS_H
#define MICRO_OS_RAMFS_RAMFS_VFS_H

#include "kernel/types.h"

/*
 * The heap that a write of a person may not take. A write that would leave
 * less than this free answers ENOSPC, so the machine keeps running when /tmp
 * is full.
 */
#ifndef RAMFS_RESERVE_KB
#define RAMFS_RESERVE_KB 1024u
#endif

/* Mount the ramfs at the given point of the tree. */
int ramfs_vfs_register(const char *point);

/* Register the type "ramfs" for vfs_mount_type(). */
int ramfs_vfs_register_type(void);

/* Forget the volume: free every node and every block. */
int ramfs_vfs_reset(void);

#endif /* MICRO_OS_RAMFS_RAMFS_VFS_H */
