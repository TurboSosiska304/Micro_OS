/*
 * Micro OS — FAT12/16/32, read-only.
 *
 * The filesystem of the cards and of the disks the rest of the world
 * writes. It is a reader on purpose: the kernel has no journal, no cache of
 * the changes and no reason to be the second writer of a volume that a
 * camera, a laptop or a phone believes it owns (ADR 0009).
 */
#ifndef MICRO_OS_FS_FAT32_FAT_H
#define MICRO_OS_FS_FAT32_FAT_H

#include "kernel/types.h"

/*
 * Mount the volume of the block device at a point of the tree. The device
 * is looked up by name in the registry of the kernel, the way MicroFS does
 * it, so this call has no platform in it.
 *
 * Returns 0, or a KERR_* code: KERR_NOENT for a name the registry does not
 * know, KERR_INVAL for a boot sector that is not a FAT volume, KERR_IO for
 * a medium that did not answer, KERR_NOTSUP for a geometry this reader does
 * not speak.
 */
int fat_vfs_mount(const char *point, const char *device);

/*
 * Put the type in the registry of the VFS, so that vfs_mount_type(point,
 * "fat32", device) finds it. An image that includes this filesystem calls
 * it at the boot, before anything asks for a FAT volume; the host tests
 * call it themselves.
 */
int fat_vfs_register_type(void);

/* The text of an error of this filesystem, for the shell and the tests. */
const char *fat_strerror(int error);

#endif /* MICRO_OS_FS_FAT32_FAT_H */
