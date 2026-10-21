/*
 * Micro OS — MicroFS (MFS).
 *
 * The filesystem of the board, implemented as described in
 * docs/memory.md. The volume is divided into three sections:
 *
 *   +-----------------------+---------------+--------+
 *   |         DATA          |     META      |  INFO  |
 *   +-----------------------+---------------+--------+
 *   0                  META_start      INFO_start    N
 *
 * The addresses of the sections are not stored as constants, they are
 * computed from the size of the volume, so a volume of any capacity
 * works the same way.
 *
 * DATA   the contents of the files and of the directories
 * META   the bitmap of the free blocks and the table of the inodes
 * INFO   the header of the filesystem
 */
#ifndef MICRO_OS_MFS_MFS_H
#define MICRO_OS_MFS_MFS_H

#include "kernel/types.h"

#define MFS_MAGIC 0x4D465331u /* "MFS1" */
#define MFS_VERSION 1
#define MFS_BLOCK_SIZE 1024
#define MFS_INODE_SIZE 16
#define MFS_NAME_MAX 32
#define MFS_PATH_MAX 128
#define MFS_ROOT_INODE 1

/* The flags of an inode. */
#define MFS_INODE_USED 0x01u
#define MFS_INODE_DIR 0x02u

/* The errors of the filesystem. */
#define MFS_OK 0
#define MFS_ENOENT (-2)
#define MFS_EEXIST (-3)
#define MFS_ENOSPC (-4)
#define MFS_EIO (-5)
#define MFS_EINVAL (-6)
#define MFS_ENOTDIR (-7)
#define MFS_EISDIR (-8)
#define MFS_ENOTEMPTY (-9)
#define MFS_E2BIG (-10)

/* The flags of the open flags, they follow the POSIX ones. */
#define MFS_O_RDONLY 0x0000
#define MFS_O_WRONLY 0x0001
#define MFS_O_RDWR 0x0002
#define MFS_O_CREAT 0x0100
#define MFS_O_TRUNC 0x0200
#define MFS_O_APPEND 0x0400

/* The block device under the filesystem. */
struct mfs_device {
    k_u32_t blocks; /* the number of the blocks of the volume */
    int (*read_block)(void *ctx, k_u32_t block, void *buf);
    int (*write_block)(void *ctx, k_u32_t block, const void *buf);
    int (*sync)(void *ctx); /* write the changes out, may be NULL */
    void *ctx;
};

/* The header of the filesystem, it lives in the INFO section. */
struct mfs_superblock {
    k_u32_t magic;
    k_u32_t version;
    k_u32_t block_size;
    k_u32_t total_blocks;
    k_u32_t data_start;
    k_u32_t data_blocks;
    k_u32_t meta_start;
    k_u32_t meta_blocks;
    k_u32_t info_start;
    k_u32_t info_blocks;
    k_u32_t root_inode;
    k_u32_t features;
};

/* One record of the table of the inodes. */
struct mfs_inode {
    k_u32_t flags;       /* MFS_INODE_* */
    k_u32_t first_block; /* the first block of the data */
    k_u32_t size;        /* the size of the data in bytes */
    k_u32_t entries;     /* the number of the entries of a directory */
};

/*
 * The header of an entry of a directory. The name follows the header
 * directly, it is not zero terminated on the volume.
 */
struct mfs_dirent_header {
    k_u32_t inode;
    k_u16_t name_len;
    k_u8_t type; /* the flags of the inode */
    k_u8_t reserved;
};

/* One entry of a directory, as the caller sees it. */
struct mfs_dirent {
    k_u32_t inode;
    k_u16_t name_len;
    k_u8_t type;
    k_u8_t reserved;
    k_u32_t size; /* the size of the object in bytes */
    char name[MFS_NAME_MAX + 1];
};

/* An open file. */
struct mfs_file {
    k_u32_t inode;
    k_u32_t first_block; /* the first block of the chain of the file */
    k_u32_t size;
    k_u32_t offset;
    int flags;
    bool dirty;
};

/* The information about the filesystem. */
struct mfs_info {
    k_u32_t total_blocks;
    k_u32_t data_blocks;
    k_u32_t data_free;
    k_u32_t meta_blocks;
    k_u32_t inodes_total;
    k_u32_t inodes_used;
    k_u32_t inodes_free;
    k_u32_t block_size;
};

/*
 * Prepare a volume: the layout is computed, the bitmap and the table of
 * the inodes are created and the root directory is made.
 */
int mfs_format(struct mfs_device *dev, k_u32_t max_records);

/* Mount a volume. The header is read and checked. */
int mfs_mount(struct mfs_device *dev);

/* Unmount the volume. */
void mfs_unmount(void);

bool mfs_is_mounted(void);

/* The information about the mounted volume. */
int mfs_get_info(struct mfs_info *out);

/* The operations on the files and the directories. */
int mfs_mkdir(const char *path);
int mfs_create(const char *path);
int mfs_open(const char *path, int flags, struct mfs_file *out);
int mfs_read(struct mfs_file *file, void *buf, k_size_t size);
int mfs_write(struct mfs_file *file, const void *buf, k_size_t size);
int mfs_close(struct mfs_file *file);
int mfs_unlink(const char *path);
int mfs_stat(const char *path, struct mfs_inode *out, struct mfs_dirent *dirent);
int mfs_list(const char *path, struct mfs_dirent *out, k_size_t max);

/* The name of an error, for the shell. */
const char *mfs_strerror(int error);

#endif /* MICRO_OS_MFS_MFS_H */
