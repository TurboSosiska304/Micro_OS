/*
 * Micro OS — ext2, read-only.
 *
 * The second filesystem of the world that the kernel reads, and the first
 * one with inodes: a superblock, a table of group descriptors, an inode for
 * every object and blocks that a file reaches through the direct entries
 * and one, two or three levels of indirection (ADR 0019).
 *
 * The volume is never written. A record of ext2 keeps a mode, an owner and
 * the moments of a change, so `stat`, `statx` and `ls -l` answer with what
 * the medium holds; `write`, `mkdir`, `unlink`, `chmod` and `chown` are
 * KERR_NOTSUP, which is how the VFS says that a filesystem has no such
 * call.
 */
#ifndef MICRO_OS_FS_EXT2_EXT2_H
#define MICRO_OS_FS_EXT2_EXT2_H

#include "kernel/types.h"

/* The longest name an ext2 directory entry can keep. */
#define EXT2_NAME_MAX 255

/*
 * Mount the volume of the block device at a point of the tree. The device
 * is looked up by name in the registry of the kernel, the way MicroFS and
 * FAT do it, so this call has no platform in it.
 *
 * Returns 0, or a KERR_* code: KERR_NOENT for a name the registry does not
 * know, KERR_INVAL for a superblock that is not an ext2 volume or a
 * geometry the reader cannot walk, KERR_IO for a medium that did not
 * answer, KERR_NOTSUP for a feature of a newer filesystem (a journal, an
 * extent tree, a 64-bit volume) that this reader does not speak.
 */
int ext2_vfs_mount(const char *point, const char *device);

/*
 * Put the type in the registry of the VFS, so that vfs_mount_type(point,
 * "ext2", device) finds it. An image that includes this filesystem calls
 * it at the boot, before anything asks for an ext2 volume; the host tests
 * call it themselves.
 */
int ext2_vfs_register_type(void);

/* The text of an error of this filesystem, for the shell and the tests. */
const char *ext2_strerror(int error);

#endif /* MICRO_OS_FS_EXT2_EXT2_H */
