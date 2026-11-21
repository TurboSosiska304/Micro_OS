/*
 * Micro OS — the internals of the FAT reader.
 *
 * The volume is described by its boot sector and kept in one static slot:
 * the board has one card and the tests have one image, and a second volume
 * of the same type is a second mount of the same reader, not a second
 * design. The lock is the same recursive mutex MicroFS uses, because the
 * scanner of a directory calls the reader of the clusters below it.
 */
#ifndef MICRO_OS_FS_FAT32_FAT_PRIVATE_H
#define MICRO_OS_FS_FAT32_FAT_PRIVATE_H

#include "fs_fat32/fat.h"

#include "kernel/blockdev.h"
#include "kernel/lock.h"
#include "kernel/types.h"
#include "kernel/vfs.h"

/* The longest name FAT can keep, in bytes of UTF-8 as well as in units. */
#define FAT_NAME_MAX 255
#define FAT_LFN_UNITS 260
#define FAT_LFN_ENTRIES 20

/* The largest sector of a device this reader accepts: the ports use 1 KiB. */
#define FAT_DEV_SECTOR_MAX 1024

/* The attributes of an entry of a directory. */
#define FAT_ATTR_READ_ONLY 0x01u
#define FAT_ATTR_HIDDEN 0x02u
#define FAT_ATTR_SYSTEM 0x04u
#define FAT_ATTR_VOLUME 0x08u
#define FAT_ATTR_DIR 0x10u
#define FAT_ATTR_ARCHIVE 0x20u
#define FAT_ATTR_LFN 0x0Fu

static inline k_u16_t fat_le16(const k_u8_t *p)
{
    return (k_u16_t)((k_u16_t)p[0] | ((k_u16_t)p[1] << 8));
}

static inline k_u32_t fat_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

/* One entry as the rest of the reader sees it. */
struct fat_entry {
    char name[FAT_NAME_MAX + 1];
    k_u32_t cluster;
    k_u32_t size;
    k_u8_t attr;
    bool dir;
};

struct fat_volume {
    const struct k_blockdev *dev;
    k_mutex_t lock;
    bool mounted;
    char point[VFS_PATH_MAX];

    k_u8_t type; /* 12, 16 or 32 */
    k_u32_t bytes_per_sector;
    k_u32_t sectors_per_cluster;
    k_u32_t reserved_sectors;
    k_u32_t num_fats;
    k_u32_t root_entries;
    k_u32_t total_sectors;
    k_u32_t fat_sectors;
    k_u32_t root_dir_sectors;
    k_u32_t root_dir_start;
    k_u32_t first_data_sector;
    k_u32_t cluster_count;
    k_u32_t root_cluster; /* 0 for the fixed root of FAT12/16 */
    k_u32_t cluster_bytes;
    k_u64_t total_bytes;

    /* The sector of the device that the last read touched, and the long
     * name that the scanner of a directory is building. Both belong to the
     * volume and are protected by the lock; no call keeps either across a
     * return. */
    k_u32_t cached_sector;
    bool cache_valid;
    k_u8_t sector[FAT_DEV_SECTOR_MAX];
    k_u16_t lfn[FAT_LFN_UNITS];
    k_u8_t lfn_count;
    k_u8_t lfn_next;
    k_u8_t lfn_checksum;
    bool lfn_active;
};

/* Mount and forget the volume of a device. */
int fat_volume_mount(struct fat_volume *vol, const struct k_blockdev *dev);
void fat_volume_unmount(struct fat_volume *vol);

/* The path of the tree without the point the volume is mounted at. */
const char *fat_relative(const struct fat_volume *vol, const char *path);

/* Find a path inside the volume. The root is "/" or an empty path. */
int fat_lookup(struct fat_volume *vol, const char *path, struct fat_entry *out);

/* Read bytes of the volume, refusing a request that runs past its end. */
int fat_read_volume(struct fat_volume *vol, k_u64_t offset, void *buf, k_size_t size);

/* Read a file from the chain that starts at `first_cluster`. */
int fat_read_chain(struct fat_volume *vol, k_u32_t first_cluster, k_u64_t offset, void *buf,
                   k_size_t size, k_size_t *got);

/*
 * The next entry of a directory. `pos` is the byte position in the stream
 * of the directory: the call moves it past the entry it returns, and a
 * return of 0 is the end of the directory.
 */
int fat_scan_next(struct fat_volume *vol, k_u32_t dir_cluster, k_u32_t *pos, struct fat_entry *out);

/* The next cluster of a chain, 0 at the end of the chain. */
int fat_next_cluster(struct fat_volume *vol, k_u32_t cluster, k_u32_t *next);

#endif /* MICRO_OS_FS_FAT32_FAT_PRIVATE_H */
