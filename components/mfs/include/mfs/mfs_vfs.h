/*
 * Micro OS — the adapter between the filesystem and the VFS.
 */
#ifndef MICRO_OS_MFS_MFS_VFS_H
#define MICRO_OS_MFS_MFS_VFS_H

#include "kernel/types.h"

/* The number of the entries that the list operation returns at most. */
#define MFS_MAX_ENTRIES 64

/* Mount the filesystem at the given point of the tree. */
int mfs_vfs_register(const char *point);

/*
 * Put the type "mfs" in the registry of the VFS, so that
 * vfs_mount_type(point, "mfs", device) finds it. The boot of an image that
 * can mount a volume of the flash by name calls it before anything asks for
 * one; the tests call it themselves.
 *
 * MicroFS keeps one volume at a time: the second mount of the type reads
 * the medium of the first. A system with two MicroFS volumes at once is a
 * change of the filesystem, not of this adapter.
 */
int mfs_vfs_register_type(void);

/*
 * Mount the volume of the block device `device` at the given point of the
 * tree. A volume that the medium never held is formatted first; any other
 * error keeps the data of the user.
 */
int mfs_vfs_mount(const char *point, const char *device);

/*
 * Make a new volume on the block device `device`. The volume must not be
 * used: it is unmounted first. The whole content of the device is lost.
 */
int mfs_vfs_format(const char *device);

#endif /* MICRO_OS_MFS_MFS_VFS_H */
