/*
 * Micro OS — ext2, read and written.
 *
 * The second filesystem of the world that the kernel reads, and the first
 * one with inodes: a superblock, a table of group descriptors, an inode for
 * every object and blocks that a file reaches through the direct entries
 * and one, two or three levels of indirection (ADR 0019).
 *
 * A record of ext2 keeps a mode, an owner and the moments of a change, so
 * `stat`, `statx` and `ls -l` answer with what the medium holds. The volume
 * is also written, because a root needs a `/tmp`, a `/var` and a log
 * (ADR 0027): a file is created, written, cut and removed, a directory is
 * made, and the mode and the owner of an object change. The writer speaks
 * the direct blocks and one table of pointers; a file that would grow past
 * it is `KERR_NOSPC` rather than a layout nobody writes.
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
 * extent tree, a 64-bit volume) that this filesystem does not speak.
 */
int ext2_vfs_mount(const char *point, const char *device);

/*
 * Put the type in the registry of the VFS, so that vfs_mount_type(point,
 * "ext2", device) finds it. An image that includes this filesystem calls
 * it at the boot, before anything asks for an ext2 volume; the host tests
 * call it themselves.
 */
int ext2_vfs_register_type(void);

/*
 * Cut a file of the mounted volume to `size` bytes. The VFS of this release
 * has no truncate of its own - the descriptor cuts a file when it opens it
 * with O_TRUNC - so the operation is reached through this call by the tests
 * and by whatever offers a truncate to a program; it is the same block
 * freeing a write of the adapter uses.
 */
int ext2_vfs_truncate(const char *path, k_u64_t size);

/* The room for the first problem that ext2_check() finds. */
#define EXT2_CHECK_MESSAGE_MAX 96

/*
 * What the check of a volume found. The counters describe the volume as the
 * medium holds it; `errors` is 0 for a sound volume and the first problem is
 * the sentence in `first`.
 */
struct ext2_check_report {
    k_u32_t groups;
    k_u32_t blocks;
    k_u32_t inodes;
    k_u32_t free_blocks;
    k_u32_t free_inodes;
    k_u32_t files;
    k_u32_t directories;
    k_u32_t errors;
    k_u32_t fixed; /* the changes a repair applied; zero for a check */
    char first[EXT2_CHECK_MESSAGE_MAX];
};

/*
 * Check the volume of this filesystem that is mounted at `point`, read-only.
 *
 * Unlike the check of FAT, the call does not mount a device for itself: this
 * filesystem holds one volume, and the one a machine boots from is mounted
 * already. Returns 0 when the report was made, even when it names problems;
 * the caller reads report.errors. KERR_NOENT is nothing mounted, KERR_INVAL
 * a point that is not the volume of this filesystem.
 *
 * The check covers the superblock and the counters of the groups against
 * their bitmaps, and the chain of the directories from the root: the
 * records of every block, the two dot entries, the inode of every name and
 * the block that holds it. It never writes and it does not account for the
 * inodes no name reaches - that is the work of e2fsck, the tool of the
 * format, which every image of these tests is given to.
 */
int ext2_check(const char *point, struct ext2_check_report *report);

/*
 * Check the mounted volume and repair what the medium itself proves: the
 * counters of the groups and of the superblock are recomputed from the
 * bitmaps, the two bitmaps are rebuilt from the records that the tree
 * reaches, the link count of every inode becomes the number of the names
 * that reach it, the dot entries and the size of a directory are put right,
 * and a record that no name reaches is reconnected as `#<inode>` under
 * /lost+found when it is a file (a directory or another object is given
 * back, because the tree below it is not a name any more).
 *
 * The call is the writer of the check, like the repair of MicroFS: with the
 * lock of the volume, through the writer of the filesystem, and the volume
 * is checked again after the pass. `report->fixed` is what it changed and
 * `report->errors` is what is still wrong; a report of zero errors after a
 * repair is a volume e2fsck of the host accepts.
 *
 * The memory of the pass grows with the volume: one bit per block, one bit
 * per inode and the two of the walk. A volume whose map does not fit the
 * memory is reported and left alone; KERR_NOMEM is that answer.
 */
int ext2_repair(const char *point, struct ext2_check_report *report);

/* The text of an error of this filesystem, for the shell and the tests. */
const char *ext2_strerror(int error);

#endif /* MICRO_OS_FS_EXT2_EXT2_H */
