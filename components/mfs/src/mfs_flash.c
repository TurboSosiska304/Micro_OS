/*
 * Micro OS — the MicroFS on the partition of the flash.
 *
 * The board has a partition of 5.75 MB for the filesystem. The volume is
 * formatted the first time, after that the data of the user survives the
 * restarts.
 *
 * The flash of the ESP32 can only clear the bits, it cannot set them back.
 * A block that is written over old data would need the bits to go from zero
 * to one, so the sector is erased and written again. To keep it fast, the
 * whole sector is kept in the memory and written out when it is complete.
 */
#include "mfs/mfs.h"
#include "mfs/mfs_flash.h"

#include <string.h>

#include "esp_partition.h"
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "mfs/mfs_vfs.h"

#define MFS_PARTITION_LABEL "mfs"
#define FLASH_SECTOR_SIZE 4096

static const esp_partition_t *s_partition;

/*
 * The sector that is being changed is kept in the memory. The flash of the
 * ESP32 can only clear the bits, so a sector that is written over old data
 * has to be erased and written again.
 */
static uint8_t s_sector[FLASH_SECTOR_SIZE];
static size_t s_sector_index;
static bool s_sector_valid;
static bool s_sector_dirty;

static int flash_flush(void)
{
    if (!s_sector_valid || !s_sector_dirty) {
        return 0;
    }

    if (esp_partition_erase_range(s_partition, s_sector_index * FLASH_SECTOR_SIZE,
                                  FLASH_SECTOR_SIZE) != ESP_OK) {
        kerror("mfs: cannot erase the sector %u", (unsigned)s_sector_index);
        return -1;
    }

    if (esp_partition_write(s_partition, s_sector_index * FLASH_SECTOR_SIZE, s_sector,
                            FLASH_SECTOR_SIZE) != ESP_OK) {
        kerror("mfs: cannot write the sector %u", (unsigned)s_sector_index);
        return -1;
    }

    s_sector_dirty = false;

    return 0;
}

static int flash_read_block(void *ctx, k_u32_t block, void *buf)
{
    size_t offset = (size_t)block * MFS_BLOCK_SIZE;
    size_t index = offset / FLASH_SECTOR_SIZE;

    K_UNUSED(ctx);

    if (offset + MFS_BLOCK_SIZE > s_partition->size) {
        return -1;
    }

    /* The sector in the memory is newer than the flash. */
    if (s_sector_valid && s_sector_index == index) {
        memcpy(buf, s_sector + (offset % FLASH_SECTOR_SIZE), MFS_BLOCK_SIZE);
        return 0;
    }

    return esp_partition_read(s_partition, offset, buf, MFS_BLOCK_SIZE) == ESP_OK ? 0 : -1;
}

static int flash_write_block(void *ctx, k_u32_t block, const void *buf)
{
    size_t offset = (size_t)block * MFS_BLOCK_SIZE;
    size_t index = offset / FLASH_SECTOR_SIZE;

    K_UNUSED(ctx);

    if (offset + MFS_BLOCK_SIZE > s_partition->size) {
        return -1;
    }

    if (!s_sector_valid || s_sector_index != index) {
        /* Another sector: the old one has to be written out. */
        if (flash_flush() != 0) {
            return -1;
        }

        if (esp_partition_read(s_partition, index * FLASH_SECTOR_SIZE, s_sector,
                               FLASH_SECTOR_SIZE) != ESP_OK) {
            kerror("mfs: cannot read the sector %u", (unsigned)index);
            return -1;
        }

        s_sector_index = index;
        s_sector_valid = true;
    }

    memcpy(s_sector + (offset % FLASH_SECTOR_SIZE), buf, MFS_BLOCK_SIZE);
    s_sector_dirty = true;

    return 0;
}

static int flash_sync(void *ctx)
{
    K_UNUSED(ctx);
    return flash_flush();
}

/* The volume of the partition, as the filesystem sees it. */
static void flash_make_device(struct mfs_device *out)
{
    memset(out, 0, sizeof(*out));
    out->blocks = s_partition->size / MFS_BLOCK_SIZE;
    out->read_block = flash_read_block;
    out->write_block = flash_write_block;
    out->sync = flash_sync;
    out->ctx = (void *)s_partition;
}

/*
 * The flash was never written when the block of the header is all ones:
 * that is the state the flash comes in. It is the only case in which the
 * board formats the volume on its own.
 */
static bool flash_is_fresh(void)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t last;
    k_size_t i;

    if (s_partition == NULL) {
        return false;
    }

    last = (k_u32_t)(s_partition->size / MFS_BLOCK_SIZE) - 1u;

    if (esp_partition_read(s_partition, (size_t)last * MFS_BLOCK_SIZE, block, MFS_BLOCK_SIZE) !=
        ESP_OK) {
        return false;
    }

    for (i = 0; i < MFS_BLOCK_SIZE; i++) {
        if (block[i] != 0xFF) {
            return false;
        }
    }

    return true;
}

int mfs_flash_format(void)
{
    struct mfs_device device;
    int rc;

    if (s_partition == NULL) {
        return -1;
    }

    /* The sector in the memory belongs to the old volume. */
    s_sector_valid = false;
    s_sector_dirty = false;

    flash_make_device(&device);

    rc = mfs_format(&device, 256);
    if (rc != MFS_OK) {
        return rc;
    }

    return 0;
}

int mfs_flash_mount(const char *point)
{
    struct mfs_device device;
    struct mfs_info info;
    int rc;

    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                           MFS_PARTITION_LABEL);
    if (s_partition == NULL) {
        kerror("mfs: the partition '%s' is not found", MFS_PARTITION_LABEL);
        return -1;
    }

    flash_make_device(&device);

    rc = mfs_mount(&device);
    if (rc != MFS_OK) {
        /*
         * The volume cannot be mounted. The data of the user is only
         * destroyed when the flash was really never written: the header
         * of the volume is all ones, that is the state the flash comes
         * in. Every other error (a damaged header, a failed read) keeps
         * the data and the board comes up without a filesystem - it is
         * better than quietly erasing it.
         */
        if (!flash_is_fresh()) {
            kerror("mfs: the volume cannot be mounted (rc=%d), the data is kept", rc);
            return rc;
        }

        kinfo("mfs: the volume is not formatted yet, formatting it");
        rc = mfs_flash_format();
        if (rc != 0) {
            kerror("mfs: cannot format the volume (rc=%d)", rc);
            return rc;
        }

        rc = mfs_mount(&device);
        if (rc != MFS_OK) {
            kerror("mfs: cannot mount the volume (rc=%d)", rc);
            return rc;
        }
    }

    if (mfs_get_info(&info) == MFS_OK) {
        kinfo("mfs: %u KB mounted, %u KB free, %u inodes", (unsigned)info.data_blocks,
              (unsigned)info.data_free, (unsigned)info.inodes_total);
    }

    return mfs_vfs_register(point);
}
