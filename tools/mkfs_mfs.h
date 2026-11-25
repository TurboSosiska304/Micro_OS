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
 * The moment the tool writes into every record. The default of the command
 * is SOURCE_DATE_EPOCH, and 0 when it is not set: one date for every build,
 * so that two runs over the same inputs give the same image. Zero is
 * "unknown" in a MicroFS record, which is a truthful answer for an image
 * that was not made by a moving clock.
 */
#define MKFS_MFS_SOURCE_DATE_EPOCH 0u

/*
 * Write the whole image: `size_mb` megabytes, an MBR with one Linux
 * partition at `start_lba` (in 512-byte sectors), a formatted MicroFS
 * volume in that partition, and every entry of the list in it. The
 * directories of a path that the list does not name are made on the way.
 * `epoch` is the moment of every record.
 *
 * Returns 0 on success and -1 on a failure, with the reason on stderr.
 */
int mkfs_mfs_build_image(const char *image_path, k_u32_t size_mb, k_u32_t start_lba,
                         const struct mkfs_mfs_entry *entries, k_size_t count, k_u64_t epoch);

/*
 * The same with the flags of the format: `MKFS_MFS_JOURNAL` writes a version
 * 4 volume, the format of ADR 0047, and 0 writes the version 3 volume of
 * 1.8. The image of the machine asks for the journal; a test that wants the
 * old format asks for none.
 */
#define MKFS_MFS_JOURNAL 0x1u

/*
 * Write the volume alone, at the first byte of the file: no MBR and no
 * partition table. It is what a partition of another table holds - the volume
 * of the stick of ADR 0056 - and the caller is the partition table, not this
 * tool.
 */
#define MKFS_MFS_VOLUME_ONLY 0x2u

int mkfs_mfs_build_image_ex(const char *image_path, k_u32_t size_mb, k_u32_t start_lba,
                            const struct mkfs_mfs_entry *entries, k_size_t count, k_u64_t epoch,
                            k_u32_t flags);

#endif /* MICRO_OS_TOOLS_MKFS_MFS_H */
