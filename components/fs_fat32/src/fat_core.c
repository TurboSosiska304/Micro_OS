/*
 * Micro OS — the volume of a FAT filesystem: the boot sector, the table of
 * the clusters and the stream of a file.
 *
 * Everything here is a reader. The only writes to a medium of this project
 * are the ones of MicroFS, which owns the volume it formatted (ADR 0009).
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

#define FAT_DIR_ENTRY_SIZE 32u
#define FAT_BOOT_SIGNATURE 0xAA55u

#define FAT12_MAX_CLUSTERS 4085u
#define FAT16_MAX_CLUSTERS 65525u

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
