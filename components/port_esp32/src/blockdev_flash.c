/*
 * Micro OS — the flash of the ESP32 as a block device.
 *
 * The partition `mfs` of partitions.csv becomes the device "flash0": the
 * filesystem asks for a sector, and this file turns it into a read, a write
 * or an erase of the partition. Nothing of the filesystem is known here -
 * the device speaks blocks and the port decides what a block is.
 *
 * The flash of the ESP32 can only clear the bits, it cannot set them back.
 * A sector that is written over old data has to be erased first, and the
 * unit of the erase (4 KB) is larger than the sector of the filesystem
 * (1 KB). The sector is therefore held in the memory: the writes collect in
 * it, and the erase and the program happen when the filesystem says that
 * its work is done. The old driver of MicroFS did the same, and it is the
 * difference between one erase per group of writes and one per write.
 */
#include "port_esp32.h"

#include <string.h>

#include "esp_partition.h"
#include "kernel/blockdev.h"
#include "kernel/log.h"
#include "kernel/types.h"

#define FLASH_PARTITION_LABEL "mfs"
#define FLASH_DEVICE_NAME "flash0"

/*
 * The sector of the device is the block of MicroFS. The number is written
 * here instead of being taken from mfs/mfs.h because the port must not
 * depend on a filesystem: a mismatch is caught by the mount, which refuses
 * a device whose sector is not its block.
 */
#define FLASH_BLOCK_SIZE 1024u

/* The erase unit of the flash: esp_partition_erase_range erases this much. */
#define FLASH_ERASE_SIZE 4096u

static const esp_partition_t *s_partition;

/*
 * The erase sector that is being changed, kept in the memory so that the
 * writes of one sector cost a single erase. The buffer is not on the stack:
 * four kilobytes do not fit into the stack of a task.
 */
static struct {
    k_u8_t data[FLASH_ERASE_SIZE];
    k_u32_t index;
    bool valid;
    bool dirty;
} s_cache;

static bool flash_in_range(k_u32_t sector, k_size_t count)
{
    if (s_partition == NULL || count == 0) {
        return false;
    }

    if (sector >= s_partition->size / FLASH_BLOCK_SIZE) {
        return false;
    }

    return count <= s_partition->size / FLASH_BLOCK_SIZE - sector;
}

/* Write the cached erase sector back, erasing it first. */
static int flash_flush(void)
{
    if (!s_cache.valid || !s_cache.dirty) {
        return 0;
    }

    if (esp_partition_erase_range(s_partition, (size_t)s_cache.index * FLASH_ERASE_SIZE,
                                  FLASH_ERASE_SIZE) != ESP_OK) {
        kerror("flash0: cannot erase the sector %u", (unsigned)s_cache.index);
        return -1;
    }

    if (esp_partition_write(s_partition, (size_t)s_cache.index * FLASH_ERASE_SIZE, s_cache.data,
                            FLASH_ERASE_SIZE) != ESP_OK) {
        kerror("flash0: cannot write the sector %u", (unsigned)s_cache.index);
        return -1;
    }

    s_cache.dirty = false;

    return 0;
}

static int flash_read_one(k_u32_t sector, void *buf)
{
    size_t offset = (size_t)sector * FLASH_BLOCK_SIZE;
    size_t index = offset / FLASH_ERASE_SIZE;

    /* The cache is newer than the flash. */
    if (s_cache.valid && s_cache.index == index) {
        memcpy(buf, s_cache.data + offset % FLASH_ERASE_SIZE, FLASH_BLOCK_SIZE);
        return 0;
    }

    if (esp_partition_read(s_partition, offset, buf, FLASH_BLOCK_SIZE) != ESP_OK) {
        kerror("flash0: cannot read the sector %u", (unsigned)sector);
        return -1;
    }

    return 0;
}

static int flash_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    k_u8_t *out = (k_u8_t *)buf;
    k_size_t i;

    K_UNUSED(ctx);

    if (buf == NULL || !flash_in_range(sector, count)) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (flash_read_one(sector + (k_u32_t)i, out + i * FLASH_BLOCK_SIZE) != 0) {
            return -1;
        }
    }

    return 0;
}

static int flash_write_one(k_u32_t sector, const void *buf)
{
    size_t offset = (size_t)sector * FLASH_BLOCK_SIZE;
    size_t index = offset / FLASH_ERASE_SIZE;

    if (!s_cache.valid || s_cache.index != index) {
        /* Another sector: what the cache holds has to reach the flash. */
        if (flash_flush() != 0) {
            return -1;
        }

        if (esp_partition_read(s_partition, index * FLASH_ERASE_SIZE, s_cache.data,
                               FLASH_ERASE_SIZE) != ESP_OK) {
            kerror("flash0: cannot read the sector %u", (unsigned)index);
            return -1;
        }

        s_cache.index = (k_u32_t)index;
        s_cache.valid = true;
    }

    memcpy(s_cache.data + offset % FLASH_ERASE_SIZE, buf, FLASH_BLOCK_SIZE);
    s_cache.dirty = true;

    return 0;
}

static int flash_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t i;

    K_UNUSED(ctx);

    if (buf == NULL || !flash_in_range(sector, count)) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (flash_write_one(sector + (k_u32_t)i, in + i * FLASH_BLOCK_SIZE) != 0) {
            return -1;
        }
    }

    return 0;
}

static int flash_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    size_t offset = (size_t)sector * FLASH_BLOCK_SIZE;
    size_t size = (size_t)count * FLASH_BLOCK_SIZE;

    K_UNUSED(ctx);

    if (!flash_in_range(sector, count)) {
        return -1;
    }

    /*
     * The erase of the flash works in whole erase sectors. A range that
     * cuts one of them in half would erase the neighbours of the range, so
     * it is refused instead of being rounded out.
     */
    if (offset % FLASH_ERASE_SIZE != 0 || size % FLASH_ERASE_SIZE != 0) {
        kerror("flash0: the erase of %u sectors at %u is not aligned to %u bytes", (unsigned)count,
               (unsigned)offset, (unsigned)FLASH_ERASE_SIZE);
        return -1;
    }

    if (flash_flush() != 0) {
        return -1;
    }

    if (esp_partition_erase_range(s_partition, offset, size) != ESP_OK) {
        kerror("flash0: cannot erase %u bytes at %u", (unsigned)size, (unsigned)offset);
        return -1;
    }

    /* What the cache held is gone: the next read has to go to the flash. */
    if (s_cache.valid && (size_t)s_cache.index * FLASH_ERASE_SIZE < offset + size &&
        offset < ((size_t)s_cache.index + 1u) * FLASH_ERASE_SIZE) {
        s_cache.valid = false;
        s_cache.dirty = false;
    }

    return 0;
}

static int flash_sync(void *ctx)
{
    K_UNUSED(ctx);
    return flash_flush();
}

static const struct k_blockdev_ops s_ops = {
    .read = flash_read,
    .write = flash_write,
    .erase = flash_erase,
    .sync = flash_sync, /* the device holds the writes until it is told */
};

/* The device lives as long as the system: the kernel keeps the pointer. */
static struct k_blockdev s_device;

void port_esp32_blockdev_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                           FLASH_PARTITION_LABEL);
    if (s_partition == NULL) {
        kerror("flash0: the partition '%s' is not found", FLASH_PARTITION_LABEL);
        return;
    }

    if (s_partition->size % FLASH_BLOCK_SIZE != 0) {
        kwarn("flash0: %u bytes are left out at the end of the partition",
              (unsigned)(s_partition->size % FLASH_BLOCK_SIZE));
    }

    s_device.name = FLASH_DEVICE_NAME;
    s_device.sector_size = FLASH_BLOCK_SIZE;
    s_device.sectors = (k_u32_t)(s_partition->size / FLASH_BLOCK_SIZE);
    s_device.ops = &s_ops;
    s_device.ctx = NULL;

    if (k_blockdev_register(&s_device) != 0) {
        kerror("flash0: the device is not registered");
        return;
    }

    kinfo("flash0: %u sector(s) of %u bytes on the partition '%s'", (unsigned)s_device.sectors,
          (unsigned)s_device.sector_size, FLASH_PARTITION_LABEL);
}
