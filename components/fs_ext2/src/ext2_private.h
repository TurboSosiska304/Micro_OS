/*
 * Micro OS — the internals of the ext2 reader and writer.
 *
 * The volume is kept in one static slot: the board has one card and the
 * tests have one image, and a second volume of the same type is a second
 * mount of the same reader, not a second design. The lock is the recursive
 * mutex MicroFS and FAT use, because the walk of a path reads a directory
 * and then the inode it names.
 *
 * The writer changes the medium in the same shape as the reader reads it:
 * a block bitmap of a group, the counters of the group and of the
 * superblock, an inode and the records of a directory. The block buffer is
 * one buffer of the volume, protected by the lock, and no call keeps a
 * block of it across a return.
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

static inline void ext2_put16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

static inline void ext2_put32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
    p[2] = (k_u8_t)((value >> 16) & 0xFFu);
    p[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* The permission and the kind bits of an inode, the two words of the VFS. */
#define EXT2_MODE_PERM 07777u
#define EXT2_MODE_FILE 0644u
#define EXT2_MODE_DIR 0755u

/* One inode as the rest of the writer sees it. */
struct ext2_inode {
    k_u32_t number;
    k_u16_t mode;
    k_u16_t uid;
    k_u16_t gid;
    k_u16_t links;
    k_u32_t flags;
    k_u64_t size;
    k_u64_t blocks; /* in sectors of 512 bytes, as the format keeps it */
    k_u32_t atime;
    k_u32_t ctime;
    k_u32_t mtime;
    k_u32_t dtime; /* the moment an inode was deleted, zero while alive */
    k_u32_t block[15];
};

/* One group descriptor: where the two bitmaps and the inode table are. */
struct ext2_group {
    k_u32_t number;
    k_u32_t block_bitmap;
    k_u32_t inode_bitmap;
    k_u32_t inode_table;
    k_u16_t free_blocks;
    k_u16_t free_inodes;
    k_u16_t used_dirs;
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
    /* The group where the next allocation starts: ext2 looks for a free
     * block after the last one it used, and the volume remembers where
     * that was without reading every descriptor in front of it. */
    k_u32_t alloc_hint;
    k_u32_t inode_hint;
    /* True once a block or an inode of the volume changed: an unmount of a
     * volume that was only read writes nothing at all. */
    bool dirty;

    /* The sector of the device that the cache holds; it belongs to the
     * volume and is protected by the lock, and no call keeps it across a
     * return. */
    k_u32_t cached_sector;
    bool cache_valid;
    k_u8_t sector[EXT2_DEV_SECTOR_MAX];

    /* One block of the volume, for the writer. It is not a cache: a call
     * fills it, changes it and writes it back, and the next call may
     * overwrite it. */
    k_u8_t block_buf[EXT2_BLOCK_MAX];
};

/* The byte where the table of the group descriptors begins: the block after
 * the superblock, which is block 2 of a volume of 1 KiB blocks and block 1
 * of a larger one. */
static inline k_u64_t ext2_descriptor_start(const struct ext2_volume *vol)
{
    return (k_u64_t)(vol->first_data_block + 1u) * vol->block_size;
}

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

/* Find one name in a directory, KERR_NOENT when it is not there. */
int ext2_dir_find(struct ext2_volume *vol, const struct ext2_inode *dir, const char *name,
                  k_u32_t *inode);

/* --- the writer --- */

/* Write `size` bytes of the volume, refusing a request past its end. The
 * sector cache is dropped: the medium may now disagree with it. */
int ext2_write_bytes(struct ext2_volume *vol, k_u64_t offset, const void *buf, k_size_t size);

/* One block of the volume. A hole is not a block and a number outside the
 * volume is KERR_IO. */
int ext2_read_block(struct ext2_volume *vol, k_u32_t block, void *buf);
int ext2_write_block(struct ext2_volume *vol, k_u32_t block, const void *buf);

/* Push the counters of the superblock to the medium and sync the device. */
int ext2_volume_flush(struct ext2_volume *vol);

/* The descriptor of a group, read and written as the 32 bytes it is. */
int ext2_group_read(struct ext2_volume *vol, k_u32_t index, struct ext2_group *out);
int ext2_group_write(struct ext2_volume *vol, const struct ext2_group *group);

/*
 * Write an inode back. `preserve` keeps the fields this writer does not
 * model (an ACL, the address of a fragment, the second half of a larger
 * inode): a change of a mode must not erase them. A fresh inode passes
 * false and every field it does not name is zero.
 */
int ext2_inode_write(struct ext2_volume *vol, const struct ext2_inode *inode, bool preserve);

/* Take a block or an inode from the bitmaps of the groups; a block comes
 * back zeroed. KERR_NOSPC when the volume has none of them left. */
int ext2_alloc_block(struct ext2_volume *vol, k_u32_t *block);
int ext2_free_block(struct ext2_volume *vol, k_u32_t block);
int ext2_alloc_inode(struct ext2_volume *vol, k_u32_t *number);
int ext2_free_inode(struct ext2_volume *vol, k_u32_t number);

/*
 * The block that holds a logical block, allocated when `create` is set and
 * it is a hole. The writer speaks the direct blocks and one level of
 * indirection; a file that needs more is KERR_NOSPC rather than a second
 * layout nobody writes (ADR 0019, M2.2 of the plan).
 */
int ext2_inode_get_block(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t logical,
                         k_u32_t *block, bool create);

/* The bytes of a regular file at a position, allocating as it goes. */
int ext2_file_write(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t offset,
                    const void *buf, k_size_t size, k_size_t *got);

/* Cut an inode to `size` bytes; growing leaves a hole, and the blocks of
 * the tail are given back to the bitmaps. */
int ext2_truncate_inode(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t size);

/* Add or remove one record of a directory; the inode of the directory is
 * updated in place and the caller writes it. */
int ext2_dir_insert(struct ext2_volume *vol, struct ext2_inode *dir, const char *name,
                    k_u32_t inode, k_u8_t type);
int ext2_dir_remove(struct ext2_volume *vol, struct ext2_inode *dir, const char *name);

/* Split a path of the volume into its parent and the last name. */
int ext2_split_parent(const char *path, char *parent, k_size_t parent_size, char *name,
                      k_size_t name_size);

/* The three changes a name of the volume takes. */
int ext2_create_file(struct ext2_volume *vol, const char *path, k_u32_t mode, k_u32_t *number);
int ext2_make_dir(struct ext2_volume *vol, const char *path, k_u32_t mode);
int ext2_remove(struct ext2_volume *vol, const char *path);

/* The moment of the medium: the seconds of Unix, and never zero, because a
 * zero of a deleted inode is the mark of a record that was never used. */
k_u32_t ext2_now(void);

/* The minimum check of a mounted volume, behind ext2_check(). */
int ext2_check_volume(struct ext2_volume *vol, struct ext2_check_report *report);

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
