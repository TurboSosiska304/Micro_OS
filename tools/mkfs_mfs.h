/*
 * Micro OS — the builder of an image with a MicroFS volume.
 *
 * The image is what the x86-64 port boots from: an MBR, one partition and a
 * MicroFS volume inside it. The volume is written by the filesystem itself
 * (components/mfs), not by a second copy of the format in a tool: the
 * programs of /bin are ordinary files of a volume that a host test mounts
 * and reads with the same mfs_* calls the machine uses.
 */
#ifndef MICRO_OS_TOOLS_MKFS_MFS_H
#define MICRO_OS_TOOLS_MKFS_MFS_H

#include <stddef.h>

#include "kernel/types.h"

/* Where the partition of the volume starts: the 1 MiB every tool of every
 * system leaves before the first partition (ADR 0008). */
#define MKFS_MFS_START_LBA 2048u

/* The modes of the objects the tool writes. */
#define MKFS_MFS_MODE_DIR 0755u
#define MKFS_MFS_MODE_FILE 0644u
#define MKFS_MFS_MODE_PROGRAM 0755u

/* One object of the volume, in the order it is written. */
struct mkfs_mfs_entry {
    const char *path;   /* "/bin/hello" */
    const char *source; /* a file of the host, NULL when `data` is used */
    const k_u8_t *data; /* the bytes of the object when `source` is NULL */
    k_size_t size;      /* the size of `data` */
    k_u32_t mode;       /* MKFS_MFS_MODE_* */
    bool directory;     /* a directory has no content */
};

/*
 * Write the whole image: `size_mb` megabytes, an MBR with one Linux
 * partition at `start_lba` (in 512-byte sectors), a formatted MicroFS
 * volume in that partition, and every entry of the list in it. The
 * directories of a path that the list does not name are made on the way.
 *
 * Returns 0 on success and -1 on a failure, with the reason on stderr.
 */
int mkfs_mfs_build_image(const char *image_path, k_u32_t size_mb, k_u32_t start_lba,
                         const struct mkfs_mfs_entry *entries, k_size_t count);

#endif /* MICRO_OS_TOOLS_MKFS_MFS_H */
