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

#include "kernel/blockdev.h"
#include "kernel/types.h"

#define MFS_MAGIC 0x4D465331u /* "MFS1" */
/*
 * The format of today and the one of the volumes that came before it.
 *
 * A version 1 volume keeps 16-byte records with no time in them; a version
 * 2 volume keeps 32-byte records with the two moments of Unix seconds.
 * The mount reads both, and nothing is rewritten to make an old volume
 * new: its records have no place for a time, so a time is read as zero,
 * which means "unknown" (ADR 0013).
 */
#define MFS_VERSION 2
#define MFS_VERSION_LEGACY 1
#define MFS_BLOCK_SIZE 1024

/*
 * The features of the volume. A volume of an older system does not have
 * the bits set and is mounted the way it always was.
 */
#define MFS_FEATURE_CRC 0x0001u  /* the header carries a checksum */
#define MFS_FEATURE_TIME 0x0002u /* the records carry mtime and ctime */
#define MFS_INODE_SIZE 32
#define MFS_INODE_SIZE_LEGACY 16
#define MFS_NAME_MAX 32
#define MFS_PATH_MAX 128
#define MFS_ROOT_INODE 1

/* The flags of an inode. */
#define MFS_INODE_USED 0x01u
#define MFS_INODE_DIR 0x02u

/* The bits of a record that say what the object is, not what may be done to it. */
#define MFS_INODE_TYPE_MASK (MFS_INODE_USED | MFS_INODE_DIR)

/*
 * The mode and the owner of an object are packed into the upper bits of
 * the flags of its record (ADR 0005). The format of the volume has no
 * field of its own for them, and a volume of 0.3.x has to keep mounting,
 * so the two low bits keep their meaning and the rest of the word holds
 * the numbers. The masks below are the masks of the values, not of the
 * fields: a value is moved by the shift before it is put into the flags.
 */
#define MFS_MODE_SHIFT 8
#define MFS_MODE_BITS 9
#define MFS_MODE_MASK 0x1FFu /* rwx for the user, the group and the rest */

#define MFS_OWNER_SHIFT 17
#define MFS_OWNER_BITS 15
#define MFS_OWNER_MASK 0x7FFFu /* a uid */

_Static_assert(MFS_MODE_SHIFT + MFS_MODE_BITS <= MFS_OWNER_SHIFT, "the mode runs into the owner");
_Static_assert(MFS_OWNER_SHIFT + MFS_OWNER_BITS <= 32, "the owner does not fit into the flags");
_Static_assert(MFS_MODE_BITS == 9 && MFS_OWNER_BITS == 15,
               "the design of ADR 0005 says 9 and 15 bits");

/* What a new file and a new directory are given, as in Linux. */
#define MFS_MODE_DEFAULT 0644u
#define MFS_MODE_DIR_DEFAULT 0755u

/*
 * A zero in the mode bits is not a mode: the records of a volume of 0.3.x
 * have no modes at all, and mfs_inode_mode() reads such a record as the
 * mode of the design for its type - 0755 for a directory, 0644 for a
 * file. Nothing is rewritten to make that true: the volume keeps the
 * records it was written with.
 *
 * The price is that "chmod 000" cannot be expressed: the zero already
 * means "the record predates the modes", and mfs_chmod() refuses it
 * instead of storing a number that would not come back. This is a
 * conscious choice while the board has one user (ADR 0005).
 */

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
#define MFS_ENOTSUP (-11)
#define MFS_ENOMEM (-12)

/* The flags of the open flags, they follow the POSIX ones. */
#define MFS_O_RDONLY 0x0000
#define MFS_O_WRONLY 0x0001
#define MFS_O_RDWR 0x0002
#define MFS_O_CREAT 0x0100
#define MFS_O_TRUNC 0x0200
#define MFS_O_APPEND 0x0400
/*
 * The open of a directory for reading its entries. Without it mfs_open()
 * answers MFS_EISDIR for a directory, the way it always did.
 */
#define MFS_O_DIRECTORY 0x0800

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
    k_u32_t crc; /* the checksum of the header, MFS_FEATURE_CRC */
};

/* One record of the table of the inodes. */
struct mfs_inode {
    k_u32_t flags;       /* MFS_INODE_* */
    k_u32_t first_block; /* the first block of the data */
    k_u32_t size;        /* the size of the data in bytes */
    k_u32_t entries;     /* the number of the entries of a directory */
    /*
     * The moments of the last change of the data and of the record, in the
     * seconds of Unix. A volume of version 1 has no room for them, and the
     * reader answers zero, which means "unknown" (ADR 0013).
     */
    k_u64_t mtime; /* the last change of the content */
    k_u64_t ctime; /* the last change of the record, the mode or the owner */
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

/*
 * The layout of these structures is the layout of the volume: the fields
 * are whole words and the records have the same size on every processor
 * the system runs on. A change of one of them is a change of the format
 * of the volume, and the checks below make it a conscious one.
 */
_Static_assert(sizeof(struct mfs_superblock) == 52, "the header of the volume changed");
_Static_assert(sizeof(struct mfs_inode) == 32, "a record of the table of the inodes changed");
_Static_assert(MFS_INODE_SIZE_LEGACY == 16, "the record of a legacy volume changed");
_Static_assert(sizeof(struct mfs_dirent_header) == 8, "an entry of a directory changed");

/*
 * One entry of a directory, as the caller sees it. The type is the low
 * two bits of the flags of the object, the mode and the owner are the
 * same numbers the inode carries: a listing needs no second lookup.
 */
struct mfs_dirent {
    k_u32_t inode;
    k_u16_t name_len;
    k_u8_t type;
    k_u8_t reserved;
    k_u32_t size;  /* the size of the object in bytes */
    k_u32_t mode;  /* MFS_MODE_* of the object */
    k_u32_t owner; /* the uid of the object */
    char name[MFS_NAME_MAX + 1];
};

/*
 * An open file.
 *
 * The chain of a file is a list: the only way to its n-th block is the walk
 * through the n blocks before it, and every step reads the header of a block
 * of the device. A read that starts the walk at the head for every block
 * costs the square of the blocks of the file, which a program of a megabyte
 * feels as seconds of a start. The descriptor therefore keeps the block that
 * it used last (`chain_block`) and the index of that block (`chain_index`):
 * a read after it walks forward from there, one step per block, and only a
 * seek back to an earlier block starts at the head again. The cursor
 * describes the chain of the open file, not the volume, and it is dropped
 * whenever the chain the descriptor works with changes its head. The
 * filesystem keeps no table of the open files (see mfs_unmount()), so a
 * chain that is changed under an open descriptor - a name removed or
 * replaced while its file is still open - is outside the contract, as it
 * was before the cursor existed.
 */
struct mfs_file {
    k_u32_t inode;
    k_u32_t first_block; /* the first block of the chain of the file */
    k_u32_t size;
    k_u32_t offset;
    int flags;
    bool dirty;
    bool chain_valid;    /* chain_block is a block of this chain */
    k_u32_t chain_block; /* the block of the chain the cursor names */
    k_u32_t chain_index; /* the index of that block from the head, 0 based */
    /*
     * The number of the blocks of the chain, once the descriptor knows it.
     * The open does not walk the chain: the first write counts it and the
     * growth keeps the number, so a file written in many small calls pays
     * one walk and not one per call - the write side of the cursor above.
     */
    k_u32_t chain_blocks;
    bool chain_blocks_valid;
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

/* The room for the first problem that mfs_check() finds. */
#define MFS_CHECK_MESSAGE_MAX 96

/*
 * What mfs_check() found. The counters describe the volume as it is on the
 * device: the files and the directories that are in use, the data blocks
 * that the objects own and the blocks that the map of the free blocks
 * leaves for them.
 */
struct mfs_check_report {
    k_u32_t files;
    k_u32_t directories;
    k_u32_t free_blocks;
    k_u32_t used_blocks;
    k_u32_t data_blocks;
    k_u32_t errors;  /* the number of the problems, 0 for a sound volume */
    k_u32_t fixed;   /* the repairs that mfs_repair() applied, 0 for a check */
    k_u32_t unfixed; /* the problems left after the repair pass */
    /* The first problem in English, an empty string for a sound volume. */
    char first[MFS_CHECK_MESSAGE_MAX];
};

/*
 * Prepare a volume: the layout is computed, the bitmap and the table of
 * the inodes are created and the root directory is made.
 *
 * The device has to speak in the blocks of the filesystem: a sector of
 * another size is refused, because the layout of the volume is written in
 * blocks of MFS_BLOCK_SIZE and a device that reads more or less than that
 * would silently hand out the wrong bytes.
 */
int mfs_format(const struct k_blockdev *dev, k_u32_t max_records);

/* Mount a volume. The header is read and checked. */
int mfs_mount(const struct k_blockdev *dev);

/* Unmount the volume. */
/*
 * Forget the volume. What was written reaches the flash first, so the
 * volume is the same when it is mounted again. Every file that was opened
 * has to be closed before this is called: the filesystem keeps no table
 * of the open files.
 */
void mfs_unmount(void);

/*
 * Bring the changes to the device: the map of the free blocks and
 * everything that is waiting in the buffer of the device. Every operation
 * that takes or gives back blocks has to end with it, otherwise a restart
 * of the board gives the same block to two objects.
 */
int mfs_sync(void);

bool mfs_is_mounted(void);

/* The information about the mounted volume. */
int mfs_get_info(struct mfs_info *out);

/*
 * Check the mounted volume. Nothing is written, repaired or reformatted:
 * the header, the table of the inodes, the entries of the directories and
 * the map of the free blocks are only read, and the volume stays the way
 * it was.
 *
 * The header of the volume is read again from the device, since the copy
 * in the memory is the one of the mount; the table of the inodes is then
 * walked record by record, every chain of the data blocks is followed and
 * the tree of the directories is walked from the root, with a limit on the
 * depth, so that a loop in the tree is found instead of followed.
 *
 * Returns MFS_OK when the check ran to the end: report->errors is 0 for a
 * sound volume and greater than 0 for a damaged one, with the first
 * problem in report->first. A volume that is damaged is still reported
 * this way, because the call itself did its work; the error code of the
 * call speaks about the call. MFS_EINVAL means that there is no mounted
 * volume or no report. MFS_EIO means that the device failed to read a
 * block: the check stops there, the counters are partial and the block is
 * the first problem.
 */
int mfs_check(struct mfs_check_report *report);

/*
 * Check the mounted volume and repair the damage that the format itself
 * explains. The volume is walked exactly as mfs_check() walks it and the
 * report is about the volume after the pass, not about the plan; fixed says
 * how many changes were made.
 *
 * A repair is a reader of the medium first: each change has to be proven by
 * what the medium says, and a question the medium cannot answer is left for
 * a person and counted in unfixed. The rules:
 *
 *   - a data block marked used that no object holds goes back to the free
 *     map; a block an object holds that the map calls free is taken again
 *     (the opposite would give it to the next writer and lose the object);
 *   - an entry of a directory that names a record outside the table or a
 *     free record is removed, and an entry whose record disagrees with it
 *     about the type takes the type of the record; a name that holds a slash,
 *     the later of two equal names and the later of two entries of one
 *     directory that name one record are removed, because none of them can
 *     be reached through a path (this format has no second name of a file);
 *   - a chain that loops or leaves the data section is cut at the last good
 *     block, the second of two objects that name the same block gives it up
 *     (the first owner in the walk of the tree keeps it, the way FAT does),
 *     and a size that does not match its chain is put right: a chain shorter
 *     than the file cuts the size down, a chain longer than the file gives
 *     the tail back;
 *   - the counter of the entries of a directory is recomputed from its
 *     entries;
 *   - a record that no name reaches is a create or an unlink that stopped
 *     half way: it is freed, and so are its blocks, because nothing on the
 *     medium can give the name back. This is the only "half-written record"
 *     the format lets a reader detect - a record carries no checksum, so a
 *     record half written byte by byte looks like a record;
 *   - a header that is damaged is not touched: the volume would be a
 *     different volume, and the mount would refuse it. The pass does not
 *     write a byte in that case.
 *
 * The files of a volume have no checksum either, so a half-written *content*
 * of a file is not detected and not repaired: the repair promises a volume
 * that can be mounted and walked, not the data of a write a power cut
 * interrupted.
 *
 * The header of the volume stores no count of the free blocks or the free
 * records; the counters of the report are computed from the table and the
 * map at every check, so there is no counter to put right outside the
 * entries of a directory.
 *
 * Like mfs_check(), MFS_EINVAL means no mounted volume or no report, and
 * MFS_EIO means that the device failed in a way that stopped the walk.
 */
int mfs_repair(struct mfs_check_report *report);

/* The operations on the files and the directories. */
int mfs_mkdir(const char *path);
int mfs_create(const char *path);
int mfs_open(const char *path, int flags, struct mfs_file *out);
/*
 * The next entries of a directory that was opened with MFS_O_DIRECTORY. The
 * position is the one of the open file: the call starts where the previous
 * one stopped, and the end of the directory is an empty answer. Anything
 * that is not a directory answers MFS_ENOTDIR.
 */
int mfs_readdir(struct mfs_file *file, struct mfs_dirent *out, k_size_t max, k_size_t *count);
int mfs_read(struct mfs_file *file, void *buf, k_size_t size);
int mfs_write(struct mfs_file *file, const void *buf, k_size_t size);
/*
 * Cut an open file to `size` bytes, the `ftruncate` of POSIX: the blocks of
 * the tail go back to the map, a larger size is filled with zeros, and the
 * position of the descriptor does not move. The descriptor has to be open
 * for writing; the size is a 32-bit number of the format, so a wider one is
 * MFS_E2BIG and a size the free blocks cannot hold is MFS_ENOSPC.
 */
int mfs_truncate(struct mfs_file *file, k_u64_t size);
int mfs_close(struct mfs_file *file);
int mfs_unlink(const char *path);
/*
 * Rename an object of the volume (ADR 0024 §5). A file moves to any
 * directory of the volume; a directory only keeps its name in its own
 * parent, because moving the tree below it is a walk this format does not
 * promise to finish (MFS_ENOTSUP). A name that is already there and is a
 * file is replaced, as rename of POSIX does; a name of a directory is
 * MFS_EISDIR, because the tree below it would be lost.
 *
 * The entry of the new name is written before the old one goes, so a
 * volume that loses the power in the middle has a name too many (which the
 * check reports) and never a name too few.
 */
int mfs_rename(const char *from, const char *to);
int mfs_stat(const char *path, struct mfs_inode *out, struct mfs_dirent *dirent);
int mfs_list(const char *path, struct mfs_dirent *out, k_size_t max);

/*
 * The mode of an inode is read and written through these, so that the
 * callers - the shell, the syscalls, the check of the access - never have
 * to know where the bits live. The mode is refused outside the nine bits
 * of the rwx triads and the owner outside the fifteen bits of a uid, so
 * that a number that does not fit can never spill into the type. A mode
 * of zero is refused too, see above: it means "the record predates the
 * modes".
 */
k_u32_t mfs_inode_mode(const struct mfs_inode *inode);
k_u32_t mfs_inode_owner(const struct mfs_inode *inode);
int mfs_chmod(const char *path, k_u32_t mode);
int mfs_chown(const char *path, k_u32_t owner);

/* The name of an error, for the shell. */
const char *mfs_strerror(int error);

/*
 * The source of the moments of the records.
 *
 * The default is the wall clock of the kernel (ADR 0013). A tool that
 * builds an image sets one fixed moment instead: the same sources and the
 * same objects then give the same bytes, which is what a reproducible
 * image means. NULL gives the clock of the kernel back. The source is
 * asked for every stamp, so a caller that wants a moving time can keep the
 * default and a caller that wants one date does not have to touch the
 * records after they are written.
 */
typedef k_u64_t (*mfs_now_fn)(void);

void mfs_set_time_source(mfs_now_fn now);

#endif /* MICRO_OS_MFS_MFS_H */
