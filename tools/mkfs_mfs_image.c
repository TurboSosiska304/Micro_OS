/*
 * Micro OS — building an image with a MicroFS volume (host side).
 *
 * The format of the volume is not written here: this file is a block device
 * over a range of a regular file, and components/mfs formats it and puts the
 * files into it through the calls the board uses. A tool that copied the
 * layout would be a second implementation of MicroFS, and the first change
 * of the format would leave the two disagreeing.
 */
#include "mkfs_mfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/blockdev.h"
#include "kernel/types.h"
#include "mfs/mfs.h"

/* The unit of the partition tables, and the one of the filesystem. */
#define MKFS_SECTOR_BYTES 512u
#define MKFS_BLOCK_BYTES ((k_u32_t)MFS_BLOCK_SIZE)
#define MKFS_MBR_SIGNATURE 510u
#define MKFS_MBR_ENTRIES 446u
#define MKFS_MBR_TYPE_LINUX 0x83u

/* A volume of the tool: a range of a file, in the blocks of MicroFS. */
struct mkfs_device {
    int fd;
    off_t base;
    k_u32_t sectors;
};

static int mkfs_xfer(struct mkfs_device *dev, bool writing, k_u32_t sector, void *buf,
                     k_size_t count)
{
    k_u8_t *bytes = (k_u8_t *)buf;
    off_t offset = dev->base + (off_t)sector * (off_t)MKFS_BLOCK_BYTES;
    size_t len = (size_t)count * (size_t)MKFS_BLOCK_BYTES;
    size_t done = 0;

    while (done < len) {
        ssize_t n = writing ? pwrite(dev->fd, bytes + done, len - done, offset + (off_t)done)
                            : pread(dev->fd, bytes + done, len - done, offset + (off_t)done);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        done += (size_t)n;
    }

    return 0;
}

static int mkfs_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct mkfs_device *dev = (struct mkfs_device *)ctx;

    if (buf == NULL || count == 0 || sector >= dev->sectors ||
        count > (k_size_t)dev->sectors - sector) {
        return -1;
    }

    return mkfs_xfer(dev, false, sector, buf, count);
}

static int mkfs_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct mkfs_device *dev = (struct mkfs_device *)ctx;

    if (buf == NULL || count == 0 || sector >= dev->sectors ||
        count > (k_size_t)dev->sectors - sector) {
        return -1;
    }

    return mkfs_xfer(dev, true, sector, (void *)buf, count);
}

static int mkfs_sync(void *ctx)
{
    struct mkfs_device *dev = (struct mkfs_device *)ctx;

    return fsync(dev->fd) == 0 ? 0 : -1;
}

static const struct k_blockdev_ops g_mkfs_ops = {
    .read = mkfs_read,
    .write = mkfs_write,
    .erase = NULL,
    .sync = mkfs_sync,
};

/*
 * The MBR of the image: one entry of type Linux, the first sector and the
 * length in the 512-byte units of the table. The CHS fields carry the "use
 * the LBA" value every tool writes when the geometry is not known.
 */
static int write_mbr(int fd, k_u32_t total_sectors, k_u32_t start_lba)
{
    k_u8_t sector[MKFS_SECTOR_BYTES];
    k_u32_t length = total_sectors - start_lba;

    memset(sector, 0, sizeof(sector));

    sector[MKFS_MBR_ENTRIES + 1] = 0xFEu; /* the head of the start */
    sector[MKFS_MBR_ENTRIES + 2] = 0xFFu;
    sector[MKFS_MBR_ENTRIES + 3] = 0xFFu;
    sector[MKFS_MBR_ENTRIES + 4] = MKFS_MBR_TYPE_LINUX;
    sector[MKFS_MBR_ENTRIES + 5] = 0xFEu; /* the head of the end */
    sector[MKFS_MBR_ENTRIES + 6] = 0xFFu;
    sector[MKFS_MBR_ENTRIES + 7] = 0xFFu;
    sector[MKFS_MBR_ENTRIES + 8] = (k_u8_t)(start_lba & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 9] = (k_u8_t)((start_lba >> 8) & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 10] = (k_u8_t)((start_lba >> 16) & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 11] = (k_u8_t)((start_lba >> 24) & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 12] = (k_u8_t)(length & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 13] = (k_u8_t)((length >> 8) & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 14] = (k_u8_t)((length >> 16) & 0xFFu);
    sector[MKFS_MBR_ENTRIES + 15] = (k_u8_t)((length >> 24) & 0xFFu);

    sector[MKFS_MBR_SIGNATURE] = 0x55u;
    sector[MKFS_MBR_SIGNATURE + 1] = 0xAAu;

    if (pwrite(fd, sector, sizeof(sector), 0) != (ssize_t)sizeof(sector)) {
        return -1;
    }

    return 0;
}

/* The directories of a path, made from the root down. */
static int ensure_parents(const char *path)
{
    char work[MFS_PATH_MAX];
    size_t length = strlen(path);
    size_t i;

    if (length == 0 || length >= sizeof(work) || path[0] != '/') {
        return -1;
    }

    memcpy(work, path, length + 1);

    for (i = 1; i < length; i++) {
        struct mfs_inode inode;
        int rc;

        if (work[i] != '/') {
            continue;
        }

        work[i] = '\0';

        rc = mfs_stat(work, &inode, NULL);
        if (rc == MFS_ENOENT) {
            rc = mfs_mkdir(work);
        }

        if (rc != MFS_OK && rc != MFS_EEXIST) {
            fprintf(stderr, "mkfs: cannot make the directory %s (%s)\n", work, mfs_strerror(rc));
            return -1;
        }

        work[i] = '/';
    }

    return 0;
}

static int write_object(const struct mkfs_mfs_entry *entry)
{
    struct mfs_file file;
    const k_u8_t *bytes = entry->data;
    k_u8_t *owned = NULL;
    k_size_t size = entry->size;
    int rc;

    if (ensure_parents(entry->path) != 0) {
        return -1;
    }

    if (entry->directory) {
        rc = mfs_mkdir(entry->path);

        if (rc != MFS_OK && rc != MFS_EEXIST) {
            fprintf(stderr, "mkfs: cannot make %s (%s)\n", entry->path, mfs_strerror(rc));
            return -1;
        }

        rc = mfs_chmod(entry->path, entry->mode != 0 ? entry->mode : MKFS_MFS_MODE_DIR);

        if (rc != MFS_OK) {
            fprintf(stderr, "mkfs: cannot set the mode of %s (%s)\n", entry->path,
                    mfs_strerror(rc));
            return -1;
        }

        return 0;
    }

    if (entry->source != NULL) {
        FILE *source = fopen(entry->source, "rb");
        long length;

        if (source == NULL) {
            fprintf(stderr, "mkfs: cannot open %s: %s\n", entry->source, strerror(errno));
            return -1;
        }

        if (fseek(source, 0, SEEK_END) != 0 || (length = ftell(source)) < 0 ||
            fseek(source, 0, SEEK_SET) != 0) {
            fprintf(stderr, "mkfs: cannot measure %s\n", entry->source);
            fclose(source);
            return -1;
        }

        owned = (k_u8_t *)malloc((size_t)length + 1u);
        if (owned == NULL) {
            fprintf(stderr, "mkfs: out of memory for %s\n", entry->source);
            fclose(source);
            return -1;
        }

        if (length > 0 && fread(owned, 1, (size_t)length, source) != (size_t)length) {
            fprintf(stderr, "mkfs: cannot read %s\n", entry->source);
            free(owned);
            fclose(source);
            return -1;
        }

        fclose(source);
        bytes = owned;
        size = (k_size_t)length;
    }

    rc = mfs_create(entry->path);

    if (rc != MFS_OK && rc != MFS_EEXIST) {
        fprintf(stderr, "mkfs: cannot create %s (%s)\n", entry->path, mfs_strerror(rc));
        free(owned);
        return -1;
    }

    rc = mfs_open(entry->path, MFS_O_WRONLY | MFS_O_TRUNC, &file);

    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot open %s (%s)\n", entry->path, mfs_strerror(rc));
        free(owned);
        return -1;
    }

    if (size > 0) {
        rc = mfs_write(&file, bytes, size);

        if (rc != (int)size) {
            fprintf(stderr, "mkfs: %s took %d byte(s) of %u\n", entry->path, rc, (unsigned)size);
            mfs_close(&file);
            free(owned);
            return -1;
        }
    }

    rc = mfs_close(&file);
    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot close %s (%s)\n", entry->path, mfs_strerror(rc));
        free(owned);
        return -1;
    }

    rc = mfs_chmod(entry->path, entry->mode != 0 ? entry->mode : MKFS_MFS_MODE_FILE);

    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot set the mode of %s (%s)\n", entry->path, mfs_strerror(rc));
        free(owned);
        return -1;
    }

    free(owned);

    return 0;
}

/*
 * The one moment of the image. The filesystem asks for it for every record
 * it stamps; a static number is all a builder needs, and the tool is one
 * process with one image.
 */
static k_u64_t s_epoch;

static k_u64_t mkfs_fixed_time(void)
{
    return s_epoch;
}

static int build_image(const char *image_path, k_u32_t size_mb, k_u32_t start_lba,
                       const struct mkfs_mfs_entry *entries, k_size_t count)
{
    struct mkfs_device volume;
    struct k_blockdev device;
    off_t image_bytes;
    k_u32_t total_sectors;
    k_size_t i;
    int fd;
    int rc;

    if (image_path == NULL || size_mb == 0 || start_lba == 0 || (entries == NULL && count > 0)) {
        fprintf(stderr, "mkfs: a bad image, a bad start or a bad list\n");
        return -1;
    }

    image_bytes = (off_t)size_mb * 1024 * 1024;
    total_sectors = (k_u32_t)(image_bytes / MKFS_SECTOR_BYTES);

    if (start_lba >= total_sectors) {
        fprintf(stderr, "mkfs: the partition starts past the end of the image\n");
        return -1;
    }

    fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "mkfs: cannot create %s: %s\n", image_path, strerror(errno));
        return -1;
    }

    if (ftruncate(fd, image_bytes) != 0) {
        fprintf(stderr, "mkfs: cannot size %s: %s\n", image_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (write_mbr(fd, total_sectors, start_lba) != 0) {
        fprintf(stderr, "mkfs: cannot write the MBR of %s: %s\n", image_path, strerror(errno));
        close(fd);
        return -1;
    }

    volume.fd = fd;
    volume.base = (off_t)start_lba * MKFS_SECTOR_BYTES;
    volume.sectors = (k_u32_t)((image_bytes - volume.base) / MKFS_BLOCK_BYTES);

    device.name = "mkfs";
    device.sector_size = MKFS_BLOCK_BYTES;
    device.sectors = volume.sectors;
    device.ops = &g_mkfs_ops;
    device.ctx = &volume;

    rc = mfs_format(&device, 256);
    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot format the volume (%s)\n", mfs_strerror(rc));
        close(fd);
        return -1;
    }

    rc = mfs_mount(&device);
    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot mount the new volume (%s)\n", mfs_strerror(rc));
        close(fd);
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (write_object(&entries[i]) != 0) {
            mfs_unmount();
            close(fd);
            return -1;
        }
    }

    rc = mfs_sync();
    if (rc != MFS_OK) {
        fprintf(stderr, "mkfs: cannot flush the volume (%s)\n", mfs_strerror(rc));
        mfs_unmount();
        close(fd);
        return -1;
    }

    mfs_unmount();

    if (fsync(fd) != 0) {
        fprintf(stderr, "mkfs: cannot flush %s: %s\n", image_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (close(fd) != 0) {
        fprintf(stderr, "mkfs: cannot close %s: %s\n", image_path, strerror(errno));
        return -1;
    }

    return 0;
}

int mkfs_mfs_build_image(const char *image_path, k_u32_t size_mb, k_u32_t start_lba,
                         const struct mkfs_mfs_entry *entries, k_size_t count, k_u64_t epoch)
{
    int rc;

    /*
     * The records of the image carry one date, not the clock of the run:
     * the same inputs have to give the same bytes. The source is put back
     * afterwards, because the tests and the simulator share the process.
     */
    s_epoch = epoch;
    mfs_set_time_source(mkfs_fixed_time);

    rc = build_image(image_path, size_mb, start_lba, entries, count);

    mfs_set_time_source(NULL);

    return rc;
}
