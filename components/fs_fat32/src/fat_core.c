/*
 * Micro OS — the volume of a FAT filesystem: the boot sector, the table of
 * the clusters and the stream of a file.
 *
 * The volume is read and written. A write goes through the same sector cache
 * as a read: the cache is dropped after every write, so the next read sees
 * the medium and not the copy of it that the reader had. The order of the
 * changes is the one that survives a power cut best with no journal to
 * replay: a cluster joins a chain only after its own entry says it is the
 * end, and a directory entry names a cluster only after the cluster is
 * written.
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

#define FAT_DIR_ENTRY_SIZE 32u
#define FAT_BOOT_SIGNATURE 0xAA55u

#define FAT12_MAX_CLUSTERS 4085u
#define FAT16_MAX_CLUSTERS 65525u

/* The FSInfo sector of a FAT32 volume: the two signatures, the signature
 * that ends the 512 bytes, and the two numbers the world reads there. */
#define FAT_FSINFO_BYTES 512u
#define FAT_FSINFO_LEAD 0x41615252u
#define FAT_FSINFO_STRUCT 0x61417272u
#define FAT_FSINFO_TRAIL 0xAA550000u
#define FAT_FSINFO_FREE_OFFSET 488u
#define FAT_FSINFO_NEXT_OFFSET 492u
#define FAT_FSINFO_UNKNOWN 0xFFFFFFFFu

static int count_free(struct fat_volume *vol, k_u32_t *count, k_u32_t *first_free);
static int fsinfo_write(struct fat_volume *vol);
static void fsinfo_init(struct fat_volume *vol, const k_u8_t *boot);

static bool is_power_of_two(k_u32_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

/*
 * Read bytes of the medium through the cache of the device sector. A FAT
 * volume speaks its own sector (512 bytes most of the time) while the
 * block layer of this kernel hands out 1 KiB, so every read is placed
 * inside the sector of the device and the sector is read once.
 */
static int read_device(struct fat_volume *vol, k_u64_t offset, void *buf, k_size_t size)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t dev_size = vol->dev->sector_size;

    if (dev_size == 0 || dev_size > FAT_DEV_SECTOR_MAX) {
        return KERR_NOTSUP;
    }

    if (vol->dev->ops == NULL || vol->dev->ops->read == NULL) {
        return KERR_NOTSUP;
    }

    while (size > 0) {
        k_u32_t sector = (k_u32_t)(offset / dev_size);
        k_u32_t inside = (k_u32_t)(offset % dev_size);
        k_size_t chunk = dev_size - inside;

        if (sector >= vol->dev->sectors) {
            return KERR_IO;
        }

        if (chunk > size) {
            chunk = size;
        }

        if (!vol->cache_valid || vol->cached_sector != sector) {
            if (vol->dev->ops->read(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                return KERR_IO;
            }
            vol->cached_sector = sector;
            vol->cache_valid = true;
        }

        memcpy(dst, vol->sector + inside, chunk);
        dst += chunk;
        offset += chunk;
        size -= chunk;
    }

    return 0;
}

/* Read bytes of the volume and refuse a request that runs past its end. */
int fat_read_volume(struct fat_volume *vol, k_u64_t offset, void *buf, k_size_t size)
{
    if (offset > vol->total_bytes || size > vol->total_bytes - offset) {
        return KERR_IO;
    }

    return read_device(vol, offset, buf, size);
}

static k_u64_t cluster_offset(const struct fat_volume *vol, k_u32_t cluster)
{
    return (k_u64_t)(cluster - 2u) * vol->cluster_bytes +
           (k_u64_t)vol->first_data_sector * vol->bytes_per_sector;
}

static int fat_size_check(const struct fat_volume *vol)
{
    k_u64_t entries = (k_u64_t)vol->cluster_count + 2u;
    k_u64_t needed;

    switch (vol->type) {
    case 12:
        needed = (entries * 3u + 1u) / 2u;
        break;
    case 16:
        needed = entries * 2u;
        break;
    case 32:
        needed = entries * 4u;
        break;
    default:
        return KERR_INVAL;
    }

    return (needed <= (k_u64_t)vol->fat_sectors * vol->bytes_per_sector) ? 0 : KERR_INVAL;
}

int fat_volume_mount(struct fat_volume *vol, const struct k_blockdev *dev)
{
    k_u8_t boot[512];
    k_u32_t bytes_per_sector;
    k_u32_t sectors_per_cluster;
    k_u32_t total16;
    k_u32_t total32;
    k_u32_t fat16;
    k_u32_t fat32;
    k_u64_t device_bytes;
    int rc;

    if (vol == NULL || dev == NULL) {
        return KERR_INVAL;
    }

    k_mutex_destroy(&vol->lock);
    memset(vol, 0, sizeof(*vol));
    vol->dev = dev;

    rc = read_device(vol, 0, boot, sizeof(boot));
    if (rc != 0) {
        return rc;
    }

    if (fat_le16(boot + 510) != FAT_BOOT_SIGNATURE) {
        return KERR_INVAL;
    }

    bytes_per_sector = fat_le16(boot + 11);
    sectors_per_cluster = boot[13];
    total16 = fat_le16(boot + 19);
    fat16 = fat_le16(boot + 22);
    total32 = fat_le32(boot + 32);
    fat32 = fat_le32(boot + 36);

    if (!is_power_of_two(bytes_per_sector) || bytes_per_sector < 512u || bytes_per_sector > 4096u) {
        return KERR_NOTSUP;
    }

    if (!is_power_of_two(sectors_per_cluster) || sectors_per_cluster > 128u) {
        return KERR_INVAL;
    }

    if (fat_le16(boot + 14) == 0 || boot[16] == 0 || boot[16] > 4u) {
        return KERR_INVAL;
    }

    if (total16 != 0) {
        vol->total_sectors = total16;
    } else {
        vol->total_sectors = total32;
    }

    vol->fat_sectors = (fat16 != 0) ? fat16 : fat32;
    if (vol->total_sectors == 0 || vol->fat_sectors == 0) {
        return KERR_INVAL;
    }

    vol->bytes_per_sector = bytes_per_sector;
    vol->sectors_per_cluster = sectors_per_cluster;
    vol->reserved_sectors = fat_le16(boot + 14);
    vol->num_fats = boot[16];
    vol->root_entries = fat_le16(boot + 17);
    vol->cluster_bytes = sectors_per_cluster * bytes_per_sector;

    if (vol->cluster_bytes > 65536u) {
        return KERR_INVAL;
    }

    if (fat16 == 0) {
        /* A FAT32 volume has no fixed root: its root is a chain like any
         * other directory, and the entries of a fixed root are not there. */
        if (vol->root_entries != 0) {
            return KERR_INVAL;
        }
        vol->type = 32;
        vol->root_cluster = fat_le32(boot + 44);
        if (vol->root_cluster < 2u) {
            return KERR_INVAL;
        }
    } else {
        /* The old formats have a fixed root, so a volume that says it has
         * none has no directory tree at all. */
        if (vol->root_entries == 0) {
            return KERR_INVAL;
        }
        vol->type = 0; /* decided by the count of the clusters below */
        vol->root_cluster = 0;
        vol->root_dir_sectors =
            (vol->root_entries * FAT_DIR_ENTRY_SIZE + bytes_per_sector - 1u) / bytes_per_sector;
    }

    vol->root_dir_start = vol->reserved_sectors + vol->num_fats * vol->fat_sectors;
    vol->first_data_sector = vol->root_dir_start + vol->root_dir_sectors;

    if (vol->first_data_sector >= vol->total_sectors) {
        return KERR_INVAL;
    }

    vol->cluster_count = (vol->total_sectors - vol->first_data_sector) / vol->sectors_per_cluster;

    if (vol->cluster_count == 0) {
        return KERR_INVAL;
    }

    if (vol->type == 0) {
        if (vol->cluster_count < FAT12_MAX_CLUSTERS) {
            vol->type = 12;
        } else if (vol->cluster_count < FAT16_MAX_CLUSTERS) {
            vol->type = 16;
        } else {
            /* A FAT16 table cannot name a cluster this far. */
            return KERR_INVAL;
        }
    }

    if (vol->type == 32) {
        if (vol->cluster_count >= 0x0FFFFFF5u) {
            return KERR_INVAL;
        }
        if (vol->root_cluster >= vol->cluster_count + 2u) {
            return KERR_INVAL;
        }
    }

    rc = fat_size_check(vol);
    if (rc != 0) {
        return rc;
    }

    vol->total_bytes = (k_u64_t)vol->total_sectors * vol->bytes_per_sector;
    device_bytes = (k_u64_t)dev->sectors * dev->sector_size;
    if (vol->total_bytes > device_bytes) {
        return KERR_INVAL;
    }

    /* The summary of the free clusters lives on the medium of a FAT32
     * volume. A volume that was made by this machine has one; an image
     * whose summary is missing or stale is still readable, and the table
     * answers for it when the summary cannot. */
    fsinfo_init(vol, boot);

    vol->mounted = true;
    kinfo("fat: FAT%u mounted, %u clusters of %u bytes", (unsigned)vol->type,
          (unsigned)vol->cluster_count, (unsigned)vol->cluster_bytes);

    return 0;
}

void fat_volume_unmount(struct fat_volume *vol)
{
    if (vol == NULL) {
        return;
    }

    vol->mounted = false;
    vol->cache_valid = false;
    k_mutex_destroy(&vol->lock);
}

int fat_next_cluster(struct fat_volume *vol, k_u32_t cluster, k_u32_t *next)
{
    k_u64_t offset;
    k_u32_t value;
    k_u8_t raw[4];
    int rc;

    if (vol == NULL || next == NULL || cluster < 2u || cluster >= vol->cluster_count + 2u) {
        return KERR_INVAL;
    }

    switch (vol->type) {
    case 12:
        offset = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector + cluster + cluster / 2u;
        rc = fat_read_volume(vol, offset, raw, 2);
        if (rc != 0) {
            return rc;
        }
        value = fat_le16(raw);
        value = ((cluster & 1u) != 0u) ? (value >> 4) : (value & 0x0FFFu);
        if (value == 0 || value == 0x0FF7u) {
            return KERR_IO;
        }
        if (value >= 0x0FF8u) {
            *next = 0;
            return 0;
        }
        break;
    case 16:
        offset = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector + (k_u64_t)cluster * 2u;
        rc = fat_read_volume(vol, offset, raw, 2);
        if (rc != 0) {
            return rc;
        }
        value = fat_le16(raw);
        if (value == 0 || value == 0xFFF7u) {
            return KERR_IO;
        }
        if (value >= 0xFFF8u) {
            *next = 0;
            return 0;
        }
        break;
    case 32:
        offset = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector + (k_u64_t)cluster * 4u;
        rc = fat_read_volume(vol, offset, raw, 4);
        if (rc != 0) {
            return rc;
        }
        value = fat_le32(raw) & 0x0FFFFFFFu;
        if (value == 0 || value == 0x0FFFFFF7u) {
            return KERR_IO;
        }
        if (value >= 0x0FFFFFF8u) {
            *next = 0;
            return 0;
        }
        break;
    default:
        return KERR_INVAL;
    }

    if (value < 2u || value >= vol->cluster_count + 2u) {
        return KERR_IO;
    }

    *next = value;
    return 0;
}

/* The cluster that is `index` steps down the chain; 0 when the chain ends. */
static int chain_at(struct fat_volume *vol, k_u32_t first, k_u32_t index, k_u32_t *out)
{
    k_u32_t cluster = first;
    k_u32_t steps = 0;
    int rc;

    if (index == 0) {
        *out = first;
        return 0;
    }

    if (first < 2u) {
        *out = 0;
        return 0;
    }

    while (index > 0) {
        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != 0) {
            return rc;
        }
        if (cluster == 0) {
            *out = 0;
            return 0;
        }
        index--;
        steps++;
        /* A chain that is longer than the volume is a loop, not a chain. */
        if (steps > vol->cluster_count + 2u) {
            return KERR_IO;
        }
    }

    *out = cluster;
    return 0;
}

int fat_read_chain(struct fat_volume *vol, k_u32_t first_cluster, k_u64_t offset, void *buf,
                   k_size_t size, k_size_t *got)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t cluster;
    k_u32_t index;
    k_u32_t inside;
    k_size_t done = 0;
    int rc;

    if (vol == NULL || got == NULL) {
        return KERR_INVAL;
    }

    *got = 0;

    if (size == 0) {
        return 0;
    }

    if (first_cluster < 2u) {
        return KERR_IO;
    }

    index = (k_u32_t)(offset / vol->cluster_bytes);
    inside = (k_u32_t)(offset % vol->cluster_bytes);

    rc = chain_at(vol, first_cluster, index, &cluster);
    if (rc != 0 || cluster == 0) {
        return rc;
    }

    while (done < size) {
        k_size_t chunk = vol->cluster_bytes - inside;

        if (chunk > size - done) {
            chunk = size - done;
        }

        rc = fat_read_volume(vol, cluster_offset(vol, cluster) + inside, dst + done, chunk);
        if (rc != 0) {
            return rc;
        }

        done += chunk;
        inside = 0;

        if (done == size) {
            break;
        }

        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != 0) {
            return rc;
        }
        if (cluster == 0) {
            break;
        }
    }

    *got = done;
    return 0;
}

/*
 * Write bytes of the medium through the cache of the device sector. A write
 * that does not fill its sector is read first, because the other half of a
 * 1 KiB sector belongs to another 512-byte sector of the volume. The cache
 * is dropped after every write, so a read that follows cannot answer with
 * an older copy of the medium.
 */
static int write_device(struct fat_volume *vol, k_u64_t offset, const void *buf, k_size_t size)
{
    const k_u8_t *src = (const k_u8_t *)buf;
    k_u32_t dev_size = vol->dev->sector_size;

    if (dev_size == 0 || dev_size > FAT_DEV_SECTOR_MAX) {
        return KERR_NOTSUP;
    }

    if (vol->dev->ops == NULL || vol->dev->ops->write == NULL) {
        return KERR_NOTSUP;
    }

    while (size > 0) {
        k_u32_t sector = (k_u32_t)(offset / dev_size);
        k_u32_t inside = (k_u32_t)(offset % dev_size);
        k_size_t chunk = dev_size - inside;

        if (sector >= vol->dev->sectors) {
            return KERR_IO;
        }

        if (chunk > size) {
            chunk = size;
        }

        if (inside == 0 && chunk == dev_size) {
            if (vol->dev->ops->write(vol->dev->ctx, sector, src, 1) != 0) {
                vol->cache_valid = false;
                return KERR_IO;
            }
        } else {
            if (!vol->cache_valid || vol->cached_sector != sector) {
                if (vol->dev->ops->read(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                    vol->cache_valid = false;
                    return KERR_IO;
                }
                vol->cached_sector = sector;
                vol->cache_valid = true;
            }

            memcpy(vol->sector + inside, src, chunk);

            if (vol->dev->ops->write(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                vol->cache_valid = false;
                return KERR_IO;
            }
        }

        vol->cache_valid = false;
        src += chunk;
        offset += chunk;
        size -= chunk;
    }

    return 0;
}

int fat_write_volume(struct fat_volume *vol, k_u64_t offset, const void *buf, k_size_t size)
{
    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (buf == NULL && size != 0) {
        return KERR_INVAL;
    }

    if (offset > vol->total_bytes || size > vol->total_bytes - offset) {
        return KERR_IO;
    }

    return write_device(vol, offset, buf, size);
}

int fat_flush(struct fat_volume *vol)
{
    if (vol == NULL || vol->dev == NULL || vol->dev->ops == NULL || vol->dev->ops->sync == NULL) {
        return 0;
    }

    return (vol->dev->ops->sync(vol->dev->ctx) == 0) ? 0 : KERR_IO;
}

/* The clusters the table calls free, and the first one of them. */
static int count_free(struct fat_volume *vol, k_u32_t *count, k_u32_t *first_free)
{
    k_u32_t cluster;
    k_u32_t free_count = 0;
    k_u32_t first = 0;
    int rc;

    for (cluster = 2u; cluster < vol->cluster_count + 2u; cluster++) {
        k_u32_t value;

        rc = fat_entry_get(vol, cluster, &value);
        if (rc != 0) {
            return rc;
        }

        if (value == 0) {
            if (first == 0) {
                first = cluster;
            }
            free_count++;
        }
    }

    *count = free_count;
    if (first_free != NULL) {
        *first_free = first;
    }

    return 0;
}

/* Write the summary of the free clusters, and of the next one to try, to
 * the FSInfo sector and to the copy beside the backup boot sector. */
static int fsinfo_write(struct fat_volume *vol)
{
    k_u8_t raw[FAT_FSINFO_BYTES];
    k_u64_t offset;
    k_u32_t free_count;
    k_u32_t next;
    int rc;

    if (vol == NULL || vol->type != 32 || vol->fsinfo_sector == 0) {
        return 0;
    }

    offset = (k_u64_t)vol->fsinfo_sector * vol->bytes_per_sector;
    if (fat_read_volume(vol, offset, raw, sizeof(raw)) != 0) {
        memset(raw, 0, sizeof(raw));
    }

    free_count = vol->free_known ? vol->free_clusters : FAT_FSINFO_UNKNOWN;
    next = (vol->next_free >= 2u && vol->next_free < vol->cluster_count + 2u) ? vol->next_free
                                                                              : FAT_FSINFO_UNKNOWN;

    fat_put32(raw, FAT_FSINFO_LEAD);
    fat_put32(raw + 484u, FAT_FSINFO_STRUCT);
    fat_put32(raw + FAT_FSINFO_FREE_OFFSET, free_count);
    fat_put32(raw + FAT_FSINFO_NEXT_OFFSET, next);
    fat_put32(raw + 508u, FAT_FSINFO_TRAIL);

    rc = fat_write_volume(vol, offset, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    /* The backup of the sector lives beside the backup boot sector. A
     * volume that names none keeps its summary in one place. */
    if (vol->fsinfo_backup != 0) {
        rc = fat_write_volume(vol, (k_u64_t)vol->fsinfo_backup * vol->bytes_per_sector, raw,
                              sizeof(raw));
    }

    return rc;
}

/* Read the summary of a FAT32 volume, or count the table when the summary
 * is absent or was never filled in. The mount only reads. */
static void fsinfo_init(struct fat_volume *vol, const k_u8_t *boot)
{
    k_u8_t raw[FAT_FSINFO_BYTES];
    k_u32_t sector;
    k_u32_t backup;
    k_u32_t count;
    k_u32_t first = 0;

    vol->fsinfo_sector = 0;
    vol->fsinfo_backup = 0;
    vol->free_known = false;
    vol->free_clusters = 0;
    vol->next_free = 2;

    if (vol->type != 32) {
        return;
    }

    sector = fat_le16(boot + 48);
    backup = fat_le16(boot + 50);

    if (sector >= 1u && sector < vol->reserved_sectors &&
        (k_u64_t)(sector + 1u) * vol->bytes_per_sector <= vol->total_bytes) {
        vol->fsinfo_sector = sector;
    }

    if (backup >= 1u && backup + 1u < vol->reserved_sectors &&
        (k_u64_t)(backup + 2u) * vol->bytes_per_sector <= vol->total_bytes) {
        vol->fsinfo_backup = backup + 1u;
    }

    if (vol->fsinfo_sector != 0 &&
        fat_read_volume(vol, (k_u64_t)vol->fsinfo_sector * vol->bytes_per_sector, raw,
                        sizeof(raw)) == 0 &&
        fat_le32(raw) == FAT_FSINFO_LEAD && fat_le32(raw + 484u) == FAT_FSINFO_STRUCT &&
        fat_le32(raw + 508u) == FAT_FSINFO_TRAIL) {
        k_u32_t stored_free = fat_le32(raw + FAT_FSINFO_FREE_OFFSET);
        k_u32_t stored_next = fat_le32(raw + FAT_FSINFO_NEXT_OFFSET);

        if (stored_free <= vol->cluster_count) {
            vol->free_clusters = stored_free;
            vol->free_known = true;
        }

        if (stored_next >= 2u && stored_next < vol->cluster_count + 2u) {
            vol->next_free = stored_next;
        }
    }

    /* A summary that says "unknown", or one that is not there at all, is a
     * reason to ask the table: the number a checker reads has to be true. */
    if (!vol->free_known) {
        if (count_free(vol, &count, &first) == 0) {
            vol->free_clusters = count;
            vol->free_known = true;
            if (first != 0) {
                vol->next_free = first;
            }
        }
    }
}

int fat_fsinfo_recount(struct fat_volume *vol)
{
    k_u32_t count;
    k_u32_t first = 0;
    int rc;

    if (vol == NULL) {
        return KERR_INVAL;
    }

    rc = count_free(vol, &count, &first);
    if (rc != 0) {
        return rc;
    }

    vol->free_clusters = count;
    vol->free_known = true;
    if (first != 0) {
        vol->next_free = first;
    }

    return fsinfo_write(vol);
}

/* The value that ends a chain on this volume. */
static k_u32_t end_of_chain(const struct fat_volume *vol)
{
    switch (vol->type) {
    case 12:
        return 0x0FFFu;
    case 16:
        return 0xFFFFu;
    default:
        return 0x0FFFFFFFu;
    }
}

static int table_offset(const struct fat_volume *vol, k_u32_t cluster, k_u64_t *out)
{
    k_u64_t base = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector;

    switch (vol->type) {
    case 12:
        *out = base + cluster + cluster / 2u;
        return 0;
    case 16:
        *out = base + (k_u64_t)cluster * 2u;
        return 0;
    case 32:
        *out = base + (k_u64_t)cluster * 4u;
        return 0;
    default:
        return KERR_INVAL;
    }
}

int fat_entry_get(struct fat_volume *vol, k_u32_t cluster, k_u32_t *value)
{
    k_u8_t raw[4];
    k_u64_t offset;
    k_u32_t word;
    int rc;

    if (vol == NULL || value == NULL || cluster < 2u || cluster >= vol->cluster_count + 2u) {
        return KERR_INVAL;
    }

    rc = table_offset(vol, cluster, &offset);
    if (rc != 0) {
        return rc;
    }

    switch (vol->type) {
    case 12:
        rc = fat_read_volume(vol, offset, raw, 2);
        if (rc != 0) {
            return rc;
        }
        word = fat_le16(raw);
        *value = ((cluster & 1u) != 0u) ? (word >> 4) : (word & 0x0FFFu);
        return 0;
    case 16:
        rc = fat_read_volume(vol, offset, raw, 2);
        if (rc != 0) {
            return rc;
        }
        *value = fat_le16(raw);
        return 0;
    case 32:
        rc = fat_read_volume(vol, offset, raw, 4);
        if (rc != 0) {
            return rc;
        }
        *value = fat_le32(raw) & 0x0FFFFFFFu;
        return 0;
    default:
        return KERR_INVAL;
    }
}

int fat_entry_set(struct fat_volume *vol, k_u32_t cluster, k_u32_t value)
{
    k_u8_t raw[4];
    k_u64_t base;
    k_u32_t fat;
    int rc;

    if (vol == NULL || cluster < 2u || cluster >= vol->cluster_count + 2u) {
        return KERR_INVAL;
    }

    base = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector;

    /* Every table says the same thing: a volume with two copies and one of
     * them stale is a volume a checker will not understand. */
    for (fat = 0; fat < vol->num_fats; fat++) {
        k_u64_t offset = base + (k_u64_t)fat * vol->fat_sectors * vol->bytes_per_sector;

        switch (vol->type) {
        case 12: {
            k_u32_t word;

            offset += cluster + cluster / 2u;
            rc = fat_read_volume(vol, offset, raw, 2);
            if (rc != 0) {
                return rc;
            }
            word = fat_le16(raw);
            if ((cluster & 1u) == 0u) {
                word = (word & 0xF000u) | (value & 0x0FFFu);
            } else {
                word = (word & 0x000Fu) | ((value & 0x0FFFu) << 4);
            }
            fat_put16(raw, (k_u16_t)word);
            rc = fat_write_volume(vol, offset, raw, 2);
            break;
        }
        case 16:
            offset += (k_u64_t)cluster * 2u;
            fat_put16(raw, (k_u16_t)value);
            rc = fat_write_volume(vol, offset, raw, 2);
            break;
        case 32:
            offset += (k_u64_t)cluster * 4u;
            rc = fat_read_volume(vol, offset, raw, 4);
            if (rc != 0) {
                return rc;
            }
            /* The four high bits of a FAT32 entry belong to nobody here. */
            fat_put32(raw, (fat_le32(raw) & 0xF0000000u) | (value & 0x0FFFFFFFu));
            rc = fat_write_volume(vol, offset, raw, 4);
            break;
        default:
            return KERR_INVAL;
        }

        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/* One cluster of the table: 0 when it was free and is now the end of a
 * chain, 1 when it was taken, a negative KERR_* when the medium failed. */
static int alloc_at(struct fat_volume *vol, k_u32_t candidate, k_u32_t *cluster)
{
    k_u32_t value;
    int rc;

    rc = fat_entry_get(vol, candidate, &value);
    if (rc != 0) {
        return rc;
    }

    if (value != 0) {
        return 1;
    }

    rc = fat_entry_set(vol, candidate, end_of_chain(vol));
    if (rc != 0) {
        return rc;
    }

    *cluster = candidate;
    return 0;
}

static int alloc_scan(struct fat_volume *vol, k_u32_t from, k_u32_t to, k_u32_t *cluster)
{
    k_u32_t candidate;

    for (candidate = from; candidate <= to; candidate++) {
        int rc = alloc_at(vol, candidate, cluster);

        if (rc <= 0) {
            return rc;
        }
    }

    return 1;
}

/* Take one cluster out of the map and keep the summary in step in memory,
 * without writing the summary: a chain that grows by many clusters writes
 * it once at the end, not once per cluster. */
static int alloc_one(struct fat_volume *vol, k_u32_t *cluster)
{
    k_u32_t last;
    k_u32_t start = 2u;
    int rc;

    last = vol->cluster_count + 1u;

    /* The FSInfo of a FAT32 volume says where the last allocation stopped;
     * the scan starts there and wraps once, so a volume that is written at
     * the end is not walked from the first cluster every time. */
    if (vol->type == 32 && vol->next_free >= 2u && vol->next_free <= last) {
        start = vol->next_free;
    }

    rc = alloc_scan(vol, start, last, cluster);
    if (rc == 1) {
        rc = alloc_scan(vol, 2u, start - 1u, cluster);
    }
    if (rc != 0) {
        return (rc == 1) ? KERR_NOSPC : rc;
    }

    if (vol->type == 32) {
        if (vol->free_known && vol->free_clusters > 0) {
            vol->free_clusters--;
        }
        vol->next_free = (*cluster < last) ? *cluster + 1u : 2u;
    }

    return 0;
}

int fat_alloc_cluster(struct fat_volume *vol, k_u32_t *cluster)
{
    int rc;

    if (vol == NULL || cluster == NULL) {
        return KERR_INVAL;
    }

    rc = alloc_one(vol, cluster);
    if (rc != 0) {
        return rc;
    }

    return (vol->type == 32) ? fsinfo_write(vol) : 0;
}

int fat_chain_free(struct fat_volume *vol, k_u32_t first)
{
    k_u32_t cluster = first;
    k_u32_t steps = 0;
    k_u32_t freed = 0;
    int rc = 0;

    if (vol == NULL) {
        return KERR_INVAL;
    }

    while (cluster >= 2u && cluster < vol->cluster_count + 2u) {
        k_u32_t next;

        /* A chain that is longer than the volume is a loop: stop before a
         * broken table turns a removal into an endless walk. */
        if (++steps > vol->cluster_count) {
            rc = KERR_IO;
            break;
        }

        rc = fat_entry_get(vol, cluster, &next);
        if (rc != 0) {
            break;
        }

        if (next == 0) {
            /* The cluster is free already: there is no chain to give back. */
            rc = 0;
            break;
        }

        rc = fat_entry_set(vol, cluster, 0);
        if (rc != 0) {
            break;
        }

        freed++;

        if (next >= vol->cluster_count + 2u) {
            break;
        }

        cluster = next;
    }

    /* The summary of the free clusters follows every chain that went back
     * to the map, including the one a failed write rolled back. */
    if (vol->type == 32 && freed != 0) {
        if (vol->free_known) {
            vol->free_clusters += freed;
            if (vol->free_clusters > vol->cluster_count) {
                vol->free_clusters = vol->cluster_count;
            }
        }
        if (first >= 2u && first < vol->cluster_count + 2u) {
            vol->next_free = first;
        }

        {
            int write_rc = fsinfo_write(vol);

            if (rc == 0) {
                rc = write_rc;
            }
        }
    }

    return rc;
}

int fat_chain_count(struct fat_volume *vol, k_u32_t first, k_u32_t *count)
{
    k_u32_t cluster = first;
    k_u32_t steps = 0;

    if (vol == NULL || count == NULL) {
        return KERR_INVAL;
    }

    *count = 0;

    if (first < 2u) {
        return 0;
    }

    while (cluster >= 2u) {
        k_u32_t next;
        int rc;

        if (++steps > vol->cluster_count) {
            return KERR_IO;
        }

        rc = fat_next_cluster(vol, cluster, &next);
        if (rc != 0) {
            return rc;
        }

        if (next == 0) {
            break;
        }

        cluster = next;
    }

    *count = steps;
    return 0;
}

int fat_clusters_free(struct fat_volume *vol, k_u32_t *count)
{
    k_u32_t free_count;
    int rc;

    if (vol == NULL || count == NULL) {
        return KERR_INVAL;
    }

    /* The summary of a FAT32 volume is kept by every allocation and every
     * removal, and the mount counted the table when the summary was not
     * there. The old formats have no such sector and are walked. */
    if (vol->type == 32 && vol->free_known) {
        *count = vol->free_clusters;
        return 0;
    }

    rc = count_free(vol, &free_count, NULL);
    if (rc != 0) {
        return rc;
    }

    *count = free_count;
    return 0;
}

int fat_zero_cluster(struct fat_volume *vol, k_u32_t cluster)
{
    k_u8_t zeros[128];
    k_u32_t done = 0;
    int rc;

    if (vol == NULL || cluster < 2u || cluster >= vol->cluster_count + 2u) {
        return KERR_INVAL;
    }

    memset(zeros, 0, sizeof(zeros));

    while (done < vol->cluster_bytes) {
        k_size_t chunk = vol->cluster_bytes - done;

        if (chunk > sizeof(zeros)) {
            chunk = sizeof(zeros);
        }

        rc = fat_write_volume(vol, cluster_offset(vol, cluster) + done, zeros, chunk);
        if (rc != 0) {
            return rc;
        }

        done += (k_u32_t)chunk;
    }

    return 0;
}

int fat_chain_extend(struct fat_volume *vol, k_u32_t *first, k_u32_t need)
{
    k_u32_t count = 0;
    k_u32_t old_last = 0;
    k_u32_t segment = 0;
    k_u32_t previous = 0;
    k_u32_t cluster;
    int rc = 0;

    if (vol == NULL || first == NULL) {
        return KERR_INVAL;
    }

    if (need == 0) {
        return 0;
    }

    if (*first >= 2u) {
        rc = fat_chain_count(vol, *first, &count);
        if (rc != 0) {
            return rc;
        }

        cluster = *first;
        for (;;) {
            k_u32_t next;

            rc = fat_next_cluster(vol, cluster, &next);
            if (rc != 0) {
                return rc;
            }
            if (next == 0) {
                break;
            }
            cluster = next;
        }
        previous = cluster;
        old_last = cluster;
    }

    while (count < need) {
        k_u32_t fresh;

        rc = alloc_one(vol, &fresh);
        if (rc != 0) {
            break;
        }

        rc = fat_zero_cluster(vol, fresh);
        if (rc != 0) {
            (void)fat_chain_free(vol, fresh);
            break;
        }

        if (previous == 0) {
            *first = fresh;
        } else {
            rc = fat_entry_set(vol, previous, fresh);
            if (rc != 0) {
                (void)fat_chain_free(vol, fresh);
                break;
            }
        }

        if (segment == 0) {
            segment = fresh;
        }

        previous = fresh;
        count++;
    }

    if (count >= need) {
        /* The summary of the free clusters moves once for the whole chain,
         * not once for every cluster of it. A medium that refuses it is a
         * medium that did not take the extension either: the file keeps
         * the clusters it had. */
        if (vol->type != 32) {
            return 0;
        }

        rc = fsinfo_write(vol);
        if (rc == 0) {
            return 0;
        }
    }

    /* The volume is full, or the medium refused the summary: the file
     * keeps the clusters it had. */
    if (segment != 0) {
        (void)fat_chain_free(vol, segment);
        if (old_last == 0) {
            *first = 0;
        } else {
            (void)fat_entry_set(vol, old_last, end_of_chain(vol));
        }
    }

    return (rc != 0) ? rc : KERR_NOSPC;
}

int fat_write_chain(struct fat_volume *vol, k_u32_t first_cluster, k_u64_t offset, const void *buf,
                    k_size_t size, k_size_t *done)
{
    const k_u8_t *src = (const k_u8_t *)buf;
    k_u32_t cluster;
    k_u32_t index;
    k_u32_t inside;
    k_size_t written = 0;
    int rc;

    if (vol == NULL || done == NULL) {
        return KERR_INVAL;
    }

    *done = 0;

    if (size == 0) {
        return 0;
    }

    if (first_cluster < 2u || buf == NULL) {
        return KERR_INVAL;
    }

    index = (k_u32_t)(offset / vol->cluster_bytes);
    inside = (k_u32_t)(offset % vol->cluster_bytes);

    rc = chain_at(vol, first_cluster, index, &cluster);
    if (rc != 0) {
        return rc;
    }
    if (cluster == 0) {
        return KERR_IO;
    }

    while (written < size) {
        k_size_t chunk = vol->cluster_bytes - inside;

        if (chunk > size - written) {
            chunk = size - written;
        }

        rc = fat_write_volume(vol, cluster_offset(vol, cluster) + inside, src + written, chunk);
        if (rc != 0) {
            return rc;
        }

        written += chunk;
        inside = 0;

        if (written == size) {
            break;
        }

        rc = fat_next_cluster(vol, cluster, &cluster);
        if (rc != 0) {
            return rc;
        }
        if (cluster == 0) {
            return KERR_IO; /* the caller promised the chain was long enough */
        }
    }

    *done = written;
    return 0;
}
