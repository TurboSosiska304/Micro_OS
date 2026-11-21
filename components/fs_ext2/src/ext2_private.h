/*
 * Micro OS — the internals of the ext2 reader.
 *
 * The volume is kept in one static slot: the board has one card and the
 * tests have one image, and a second volume of the same type is a second
 * mount of the same reader, not a second design. The lock is the recursive
 * mutex MicroFS and FAT use, because the walk of a path reads a directory
 * and then the inode it names.
 *
 * Nothing here writes: every primitive is a read of the medium, and the
 * cache holds one sector of the device for the length of a call.
 */
#ifndef MICRO_OS_FS_EXT2_EXT2_PRIVATE_H
#define MICRO_OS_FS_EXT2_EXT2_PRIVATE_H

#include "fs_ext2/ext2.h"

#include "kernel/blockdev.h"
#include "kernel/lock.h"
#include "kernel/types.h"
#include "kernel/vfs.h"

/* The superblock lives at the second kilobyte, whatever the block size is. */
#define EXT2_SUPERBLOCK_OFFSET 1024u
#define EXT2_SUPERBLOCK_SIZE 1024u
#define EXT2_SUPER_MAGIC 0xEF53u

/* The inode of the root directory of every ext2 volume. */
#define EXT2_ROOT_INODE 2u

/* The direct block pointers of an inode; the three next ones are indirect. */
#define EXT2_DIRECT_BLOCKS 12u

/* The shortest inode of the format; a longer one keeps more attributes. */
#define EXT2_INODE_MIN_SIZE 128u

/* The largest sector of a device this reader accepts: the ports use 1 KiB. */
#define EXT2_DEV_SECTOR_MAX 1024u

/* The largest block of the format this reader walks. */
#define EXT2_BLOCK_MAX 4096u

/*
 * The features that change the shape of the metadata. A volume with one
 * of them is not an ext2 volume this reader can walk, and saying so is
 * better than reading an extent tree as a list of direct pointers.
 */
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004u

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002u

#define EXT2_FEATURE_RO_COMPAT_HUGE_FILE 0x0008u

/* The kinds of an inode, the four high bits of i_mode. */
#define EXT2_S_IFMT 0xF000u
#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u

static inline k_u16_t ext2_le16(const k_u8_t *p)
{
    return (k_u16_t)((k_u16_t)p[0] | ((k_u16_t)p[1] << 8));
}

static inline k_u32_t ext2_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

/* One inode as the rest of the reader sees it. */
struct ext2_inode {
    k_u32_t number;
    k_u16_t mode;
    k_u16_t uid;
    k_u16_t gid;
    k_u16_t links;
    k_u32_t flags;
    k_u64_t size;
    k_u64_t blocks;
    k_u32_t atime;
    k_u32_t ctime;
    k_u32_t mtime;
    k_u32_t block[15];
};

/* One entry of a directory. The name is already out of the medium. */
struct ext2_dirent {
    char name[EXT2_NAME_MAX + 1];
    k_u32_t inode;
    k_u8_t type; /* the ext2 file type: 1 file, 2 directory, 0 unknown */
};

struct ext2_volume {
    const struct k_blockdev *dev;
    k_mutex_t lock;
    bool mounted;
    char point[VFS_PATH_MAX];

    k_u32_t block_size;
    k_u32_t first_data_block;
    k_u32_t blocks_count;
    k_u32_t inodes_count;
    k_u32_t blocks_per_group;
    k_u32_t inodes_per_group;
    k_u32_t inode_size;
    k_u32_t groups;
    k_u32_t feature_compat;
    k_u32_t feature_incompat;
    k_u32_t feature_ro_compat;
    k_u32_t free_blocks;
    k_u32_t free_inodes;
    k_u64_t volume_bytes;

    /* The sector of the device that the cache holds; it belongs to the
     * volume and is protected by the lock, and no call keeps it across a
     * return. */
    k_u32_t cached_sector;
    bool cache_valid;
    k_u8_t sector[EXT2_DEV_SECTOR_MAX];
};

/* Mount and forget the volume of a device. */
int ext2_volume_mount(struct ext2_volume *vol, const struct k_blockdev *dev);
void ext2_volume_unmount(struct ext2_volume *vol);

/* The path of the tree without the point the volume is mounted at. */
const char *ext2_relative(const struct ext2_volume *vol, const char *path);

/* Read `size` bytes of the volume, refusing a request past its end. */
int ext2_read_bytes(struct ext2_volume *vol, k_u64_t offset, void *buf, k_size_t size);

/*
 * Read one inode by its number. A number outside the table is KERR_IO: the
 * medium names an object it cannot hold, which is a damaged volume and not
 * a missing file.
 */
int ext2_read_inode(struct ext2_volume *vol, k_u32_t number, struct ext2_inode *out);

/* Find a path inside the volume. The root is "/" or an empty path. */
int ext2_lookup(struct ext2_volume *vol, const char *path, struct ext2_inode *out, k_u32_t *number);

/*
 * Read a file from the direct and the indirect blocks of its inode. A hole
 * of a sparse file is read as zeros; a block number outside the volume is
 * KERR_IO.
 */
int ext2_read_file(struct ext2_volume *vol, const struct ext2_inode *inode, k_u64_t offset,
                   void *buf, k_size_t size, k_size_t *got);

/*
 * The block that holds a logical block of an inode. `block` is zero for a
 * hole and the call is KERR_IO for a block outside the volume. The
 * directory scanner needs it to place a byte position of the stream in the
 * volume.
 */
int ext2_block_for_logical(struct ext2_volume *vol, const struct ext2_inode *inode, k_u64_t logical,
                           k_u32_t *block);

/*
 * The next entry of a directory. `pos` is a byte position in the stream of
 * the directory: the call moves it past the entry it returns, a return of
 * 1 is an entry, 0 is the end of the directory and a negative value is an
 * error. "." and ".." are entries like any other; the listing hides them.
 */
int ext2_dir_next(struct ext2_volume *vol, const struct ext2_inode *dir, k_u64_t *pos,
                  struct ext2_dirent *out);

/* The VFS type of an inode: 1 a file, 2 a directory, 3 something else. */
k_u8_t ext2_inode_vfs_type(const struct ext2_inode *inode);

/* The nine permission bits and the owner of an inode. */
static inline k_u32_t ext2_inode_mode(const struct ext2_inode *inode)
{
    return (k_u32_t)(inode->mode & 0777u);
}

static inline bool ext2_inode_is_dir(const struct ext2_inode *inode)
{
    return (inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
}

static inline bool ext2_inode_is_file(const struct ext2_inode *inode)
{
    return (inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG;
}

#endif /* MICRO_OS_FS_EXT2_EXT2_PRIVATE_H */
