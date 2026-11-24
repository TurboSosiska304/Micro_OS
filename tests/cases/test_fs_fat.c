/*
 * Micro OS — the FAT reader next to MicroFS.
 *
 * The images are built by the test itself, byte by byte: a FAT volume is a
 * boot sector, two tables of clusters and a few directories, and the test
 * can write that as easily as mkfs.fat can. What the image contains is
 * known exactly, so a reader that finds a name, a size or a byte of a file
 * is answering about the bytes the test wrote and not about a tool of the
 * machine that runs the tests.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>

#include "fs_fat32/fat.h"
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"
#include "procfs/procfs.h"

#define TEST_DEV_SECTOR 1024u
#define TEST_ROOT_POINT "/mnt/fat"

/* The geometry of the three images the tests build. */
enum fat_kind {
    FAT_KIND_12,
    FAT_KIND_16,
    FAT_KIND_32,
};

struct fat_image {
    k_u8_t *data;
    k_u32_t total_sectors; /* sectors of the volume, of its own size */
    k_u32_t bytes_per_sector;
    k_u32_t sectors_per_cluster;
    k_u32_t reserved_sectors;
    k_u32_t num_fats;
    k_u32_t root_entries;
    k_u32_t fat_sectors;
    k_u32_t root_cluster;
    k_u32_t root_dir_start;
    k_u32_t first_data_sector;
    k_u32_t cluster_count;
    k_u32_t cluster_bytes;
    k_u32_t type; /* 12, 16 or 32 */
};

struct test_device {
    k_u8_t *data;
    k_u32_t sectors;
    bool fail_write; /* the repair has to survive a medium that stops */
};

static struct mfs_ram_device g_mfs_volume;
static struct k_blockdev g_mfs_device;
static struct fat_image g_image;
static struct test_device g_fat_device;
static struct k_blockdev g_fat_blockdev;
static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];
static k_u8_t g_io_buffer[8192];
static k_u32_t g_sync_count;

static void put16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

static void put32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
    p[2] = (k_u8_t)((value >> 16) & 0xFFu);
    p[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static k_u16_t get16(const k_u8_t *p)
{
    return (k_u16_t)((k_u16_t)p[0] | ((k_u16_t)p[1] << 8));
}

static k_u32_t get32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static k_u32_t cluster_offset(const struct fat_image *img, k_u32_t cluster)
{
    return img->first_data_sector * img->bytes_per_sector + (cluster - 2u) * img->cluster_bytes;
}

static k_u8_t *dir_slot(const struct fat_image *img, k_u32_t dir_cluster, k_u32_t index)
{
    if (dir_cluster == 0) {
        return img->data + img->root_dir_start * img->bytes_per_sector + index * 32u;
    }

    return img->data + cluster_offset(img, dir_cluster) + index * 32u;
}

static int image_create(struct fat_image *img, enum fat_kind kind)
{
    memset(img, 0, sizeof(*img));
    img->bytes_per_sector = 512;
    img->sectors_per_cluster = 1;
    img->num_fats = 2;

    switch (kind) {
    case FAT_KIND_12:
        img->type = 12;
        img->total_sectors = 512;
        img->reserved_sectors = 1;
        img->root_entries = 32;
        img->fat_sectors = 2;
        break;
    case FAT_KIND_16:
        img->type = 16;
        img->total_sectors = 5000;
        img->reserved_sectors = 1;
        img->root_entries = 32;
        img->fat_sectors = 20;
        break;
    case FAT_KIND_32:
        img->type = 32;
        img->total_sectors = 2048;
        img->reserved_sectors = 32;
        img->root_entries = 0;
        img->fat_sectors = 16;
        img->root_cluster = 2;
        break;
    default:
        return -1;
    }

    img->cluster_bytes = img->sectors_per_cluster * img->bytes_per_sector;
    img->root_dir_start = img->reserved_sectors + img->num_fats * img->fat_sectors;
    img->first_data_sector =
        img->root_dir_start +
        (img->root_entries * 32u + img->bytes_per_sector - 1u) / img->bytes_per_sector;
    img->cluster_count = (img->total_sectors - img->first_data_sector) / img->sectors_per_cluster;

    img->data = (k_u8_t *)calloc((size_t)img->total_sectors * img->bytes_per_sector, 1);
    if (img->data == NULL) {
        return -1;
    }

    return 0;
}

static void image_free(struct fat_image *img)
{
    free(img->data);
    memset(img, 0, sizeof(*img));
}

static void image_boot_sector(struct fat_image *img)
{
    k_u8_t *b = img->data;
    k_u32_t total16 = (img->type == 32) ? 0 : img->total_sectors;
    k_u32_t total32 = (img->type == 32) ? img->total_sectors : 0;
    const char *label = (img->type == 32)   ? "FAT32   "
                        : (img->type == 16) ? "FAT16   "
                                            : "FAT12   ";

    b[0] = 0xEB;
    b[1] = 0x3C;
    b[2] = 0x90;
    memcpy(b + 3, "MICROOS ", 8);
    put16(b + 11, (k_u16_t)img->bytes_per_sector);
    b[13] = (k_u8_t)img->sectors_per_cluster;
    put16(b + 14, (k_u16_t)img->reserved_sectors);
    b[16] = (k_u8_t)img->num_fats;
    put16(b + 17, (k_u16_t)img->root_entries);
    put16(b + 19, (k_u16_t)total16);
    b[21] = 0xF8;
    put16(b + 22, (k_u16_t)((img->type == 32) ? 0 : img->fat_sectors));
    put16(b + 24, 32);
    put16(b + 26, 64);
    put32(b + 28, 0);
    put32(b + 32, total32);

    if (img->type == 32) {
        put32(b + 36, img->fat_sectors);
        put32(b + 44, img->root_cluster);
        put16(b + 48, 1);
        put16(b + 50, 6);

        b[64] = 0x80;
        b[66] = 0x29;
        put32(b + 67, 0x12345678u);
        memcpy(b + 71, "MICROOS    ", 11);
        memcpy(b + 82, label, 8);
    } else {
        /* The extended boot record of FAT12/16 sits at 36, not at 64: the
         * label a checker reads there has to be the one of the root. */
        b[36] = 0x80;
        b[38] = 0x29;
        put32(b + 39, 0x12345678u);
        memcpy(b + 43, "MICROOS    ", 11);
        memcpy(b + 54, label, 8);
    }

    b[510] = 0x55;
    b[511] = 0xAA;
}

static k_u32_t fat_eoc(const struct fat_image *img)
{
    switch (img->type) {
    case 12:
        return 0x0FFFu;
    case 16:
        return 0xFFFFu;
    default:
        return 0x0FFFFFFFu;
    }
}

static void image_set_fat(struct fat_image *img, k_u32_t cluster, k_u32_t value)
{
    k_u32_t fat;

    for (fat = 0; fat < img->num_fats; fat++) {
        k_u8_t *base =
            img->data + (img->reserved_sectors + fat * img->fat_sectors) * img->bytes_per_sector;
        k_u32_t offset;

        switch (img->type) {
        case 12:
            offset = cluster + cluster / 2u;
            if ((cluster & 1u) == 0u) {
                put16(base + offset,
                      (k_u16_t)((get16(base + offset) & 0xF000u) | (value & 0x0FFFu)));
            } else {
                put16(base + offset,
                      (k_u16_t)((get16(base + offset) & 0x000Fu) | ((value & 0x0FFFu) << 4)));
            }
            break;
        case 16:
            put16(base + cluster * 2u, (k_u16_t)value);
            break;
        default:
            put32(base + cluster * 4u, value & 0x0FFFFFFFu);
            break;
        }
    }
}

/* The value of one cluster in one copy of the table, as the repair left it. */
static k_u32_t image_get_fat(const struct fat_image *img, k_u32_t fat, k_u32_t cluster)
{
    const k_u8_t *base =
        img->data + (img->reserved_sectors + fat * img->fat_sectors) * img->bytes_per_sector;

    switch (img->type) {
    case 12: {
        k_u32_t offset = cluster + cluster / 2u;
        k_u16_t word = get16(base + offset);

        return ((cluster & 1u) != 0u) ? (word >> 4) : (word & 0x0FFFu);
    }
    case 16:
        return get16(base + cluster * 2u);
    default:
        return get32(base + cluster * 4u) & 0x0FFFFFFFu;
    }
}

static void image_chain(struct fat_image *img, k_u32_t first, k_u32_t count)
{
    k_u32_t i;

    for (i = 0; i < count; i++) {
        image_set_fat(img, first + i, (i + 1u < count) ? (first + i + 1u) : fat_eoc(img));
    }
}

static void image_data(struct fat_image *img, k_u32_t cluster, const char *data)
{
    size_t len = strlen(data);

    memcpy(img->data + cluster_offset(img, cluster), data, len);
}

static void short_name11(const char *name, k_u8_t out[11])
{
    const char *dot = strchr(name, '.');
    size_t base = (dot != NULL) ? (size_t)(dot - name) : strlen(name);
    size_t i;

    memset(out, ' ', 11);

    /* The dot entries of a directory are not 8.3 names: "." is one dot
     * and ".." is two, and the reader skips them by the first byte. */
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        for (i = 0; i < strlen(name); i++) {
            out[i] = '.';
        }
        return;
    }

    if (base > 8u) {
        base = 8u;
    }

    for (i = 0; i < base; i++) {
        char c = name[i];
        out[i] = (k_u8_t)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
    }

    if (dot != NULL) {
        const char *ext = dot + 1;
        size_t elen = strlen(ext);

        if (elen > 3u) {
            elen = 3u;
        }

        for (i = 0; i < elen; i++) {
            char c = ext[i];
            out[8 + i] = (k_u8_t)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
        }
    }
}

static k_u8_t short_checksum(const k_u8_t name[11])
{
    k_u8_t sum = 0;
    k_size_t i;

    for (i = 0; i < 11; i++) {
        sum = (k_u8_t)(((sum & 1u) << 7) + (sum >> 1) + name[i]);
    }

    return sum;
}

static void image_lfn(struct fat_image *img, k_u32_t dir_cluster, k_u32_t *index, const char *name,
                      k_u8_t checksum)
{
    size_t units = strlen(name);
    size_t count = (units + 12u) / 13u;
    size_t sequence;

    for (sequence = count; sequence > 0; sequence--) {
        k_u8_t *entry = dir_slot(img, dir_cluster, (*index)++);
        size_t i;

        memset(entry, 0, 32);
        entry[0] = (k_u8_t)(sequence | ((sequence == count) ? 0x40u : 0u));
        entry[11] = 0x0Fu;
        entry[13] = checksum;

        for (i = 0; i < 13u; i++) {
            size_t position = (sequence - 1u) * 13u + i;
            k_u16_t unit;

            if (position < units) {
                unit = (k_u16_t)(k_u8_t)name[position];
            } else if (position == units) {
                unit = 0x0000u;
            } else {
                unit = 0xFFFFu;
            }

            if (i < 5u) {
                put16(entry + 1 + i * 2u, unit);
            } else if (i < 11u) {
                put16(entry + 14 + (i - 5u) * 2u, unit);
            } else {
                put16(entry + 28 + (i - 11u) * 2u, unit);
            }
        }
    }
}

static void image_dir_add(struct fat_image *img, k_u32_t dir_cluster, k_u32_t *index,
                          const char *short_name, const char *long_name, k_u8_t attr,
                          k_u32_t first_cluster, k_u32_t size)
{
    k_u8_t name11[11];
    k_u8_t checksum;
    k_u8_t *entry;

    short_name11(short_name, name11);
    checksum = short_checksum(name11);

    if (long_name != NULL) {
        image_lfn(img, dir_cluster, index, long_name, checksum);
    }

    entry = dir_slot(img, dir_cluster, (*index)++);
    memset(entry, 0, 32);
    memcpy(entry, name11, 11);
    entry[11] = attr;
    put16(entry + 26, (k_u16_t)(first_cluster & 0xFFFFu));
    if (img->type == 32) {
        put16(entry + 20, (k_u16_t)(first_cluster >> 16));
    }
    put32(entry + 28, size);
}

static void image_content(struct fat_image *img)
{
    k_u32_t root = (img->type == 32) ? img->root_cluster : 0;
    k_u32_t sub;
    k_u32_t hello;
    k_u32_t lfn_file;
    k_u32_t inner;
    k_u32_t nested;
    k_u32_t deep;
    k_u32_t long_file;
    k_u32_t index = 0;
    k_u32_t sub_index = 0;
    k_u32_t nested_index = 0;

    if (img->type == 32) {
        /* The FAT32 root is a chain of two clusters; the entries below fit
         * in the first one, and the second says the chain is real. */
        image_chain(img, 2, 2);
        sub = 4;
        nested = 5;
        hello = 6;
        lfn_file = 7;
        inner = 8;
        deep = 9;
        long_file = 10;
    } else {
        sub = 2;
        hello = 3;
        lfn_file = 4;
        inner = 5;
        nested = 0;
        deep = 0;
        long_file = 0;
    }

    image_chain(img, hello, 1);
    image_data(img, hello, "hello fat\n");

    image_chain(img, lfn_file, 1);
    image_data(img, lfn_file, "long name\n");

    image_chain(img, sub, 1);
    image_chain(img, inner, 1);
    image_data(img, inner, "inner\n");

    image_dir_add(img, root, &index, "HELLO.TXT", NULL, 0x20, hello, 10);
    image_dir_add(img, root, &index, "LONGFI~1.TXT", "Long File Name.txt", 0x20, lfn_file, 10);
    image_dir_add(img, root, &index, "SUB", NULL, 0x10, sub, 0);
    image_dir_add(img, root, &index, "EMPTY.TXT", NULL, 0x20, 0, 0);

    if (img->type == 32) {
        image_chain(img, nested, 1);
        image_chain(img, deep, 1);
        image_data(img, deep, "deep\n");

        image_chain(img, long_file, 1);
        image_data(img, long_file, "long file\n");

        image_dir_add(img, root, &index, "AVERYL~1.TXT",
                      "A rather long file name that will not fit.txt", 0x20, long_file, 10);
    }

    image_dir_add(img, sub, &sub_index, ".", NULL, 0x10, sub, 0);
    /* A child of the root names cluster 0 in "..", even on FAT32. */
    image_dir_add(img, sub, &sub_index, "..", NULL, 0x10, 0, 0);
    image_dir_add(img, sub, &sub_index, "INNER.TXT", NULL, 0x20, inner, 6);

    if (img->type == 32) {
        image_dir_add(img, sub, &sub_index, "NESTED", NULL, 0x10, nested, 0);
        image_dir_add(img, nested, &nested_index, ".", NULL, 0x10, nested, 0);
        image_dir_add(img, nested, &nested_index, "..", NULL, 0x10, sub, 0);
        image_dir_add(img, nested, &nested_index, "DEEP.TXT", NULL, 0x20, deep, 5);
    }

    /* The label of the boot sector has to be the entry of the root too, or
     * a checker removes the one in the boot sector. The entry sits after
     * the files, so the slots the tests name keep their numbers. */
    image_dir_add(img, root, &index, "MICROOS", NULL, 0x08, 0, 0);
}

static void image_fsinfo(struct fat_image *img)
{
    k_u8_t *fs;
    k_u32_t cluster;
    k_u32_t free_count = 0;
    k_u32_t next_free = 2;

    if (img->type != 32) {
        return;
    }

    for (cluster = 2u; cluster < img->cluster_count + 2u; cluster++) {
        if (image_get_fat(img, 0, cluster) == 0) {
            if (free_count == 0) {
                next_free = cluster;
            }
            free_count++;
        }
    }

    /* The summary of the free clusters, the way mkfs.fat writes it: the
     * driver keeps it true after this, and fsck.fat reads it. */
    fs = img->data + img->bytes_per_sector;
    memset(fs, 0, 512);
    put32(fs, 0x41615252u);
    put32(fs + 484, 0x61417272u);
    put32(fs + 488, free_count);
    put32(fs + 492, next_free);
    put32(fs + 508, 0xAA550000u);

    /* A FAT32 volume names a backup boot sector; fsck.fat compares it with
     * the boot sector and reads the summary beside it. */
    memcpy(img->data + 6u * img->bytes_per_sector, img->data, img->bytes_per_sector);
    memcpy(img->data + 7u * img->bytes_per_sector, fs, 512);
}

static void build_image(enum fat_kind kind)
{
    image_free(&g_image);

    if (image_create(&g_image, kind) != 0) {
        return;
    }

    image_boot_sector(&g_image);

    image_set_fat(&g_image, 0,
                  (kind == FAT_KIND_32)   ? 0x0FFFFFF8u
                  : (kind == FAT_KIND_16) ? 0xFFF8u
                                          : 0x0FF8u);
    image_set_fat(&g_image, 1, fat_eoc(&g_image));

    image_content(&g_image);
    image_fsinfo(&g_image);

    g_fat_device.data = g_image.data;
    g_fat_device.sectors = g_image.total_sectors * g_image.bytes_per_sector / TEST_DEV_SECTOR;
    g_fat_blockdev.sectors = g_fat_device.sectors;
}

static int test_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct test_device *dev = (struct test_device *)ctx;
    size_t offset = (size_t)sector * TEST_DEV_SECTOR;
    size_t size = (size_t)count * TEST_DEV_SECTOR;

    if (dev->data == NULL || sector >= dev->sectors || count > dev->sectors - sector) {
        return -1;
    }

    memcpy(buf, dev->data + offset, size);
    return 0;
}

static int test_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct test_device *dev = (struct test_device *)ctx;
    size_t offset = (size_t)sector * TEST_DEV_SECTOR;
    size_t size = (size_t)count * TEST_DEV_SECTOR;

    if (dev->fail_write) {
        return -1;
    }

    if (dev->data == NULL || sector >= dev->sectors || count > dev->sectors - sector) {
        return -1;
    }

    memcpy(dev->data + offset, buf, size);
    return 0;
}

static int test_sync(void *ctx)
{
    K_UNUSED(ctx);

    g_sync_count++;
    return 0;
}

static const struct k_blockdev_ops g_test_ops = {
    .read = test_read,
    .write = test_write,
    .erase = NULL,
    .sync = test_sync,
};

static void setup(void)
{
    if (mfs_ram_init(&g_mfs_volume, 128) != 0) {
        return;
    }

    mfs_ram_device(&g_mfs_volume, &g_mfs_device);
    mfs_format(&g_mfs_device, 32);
    mfs_mount(&g_mfs_device);
    mfs_vfs_register("/");

    memset(&g_fat_device, 0, sizeof(g_fat_device));
    g_fat_blockdev.name = "fat0";
    g_fat_blockdev.sector_size = TEST_DEV_SECTOR;
    g_fat_blockdev.sectors = 1;
    g_fat_blockdev.ops = &g_test_ops;
    g_fat_blockdev.ctx = &g_fat_device;
    k_blockdev_register(&g_fat_blockdev);

    fat_vfs_register_type();
}

static void teardown(void)
{
    vfs_umount(TEST_ROOT_POINT);
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_mfs_volume);
    k_blockdev_unregister(&g_fat_blockdev);
    image_free(&g_image);
    memset(&g_fat_device, 0, sizeof(g_fat_device));
    vfs_unregister_type("fat32");
}

/* Mount the image the test has just built, without building it again: a
 * repaired volume is mounted to prove what the repair left. */
static int mount_built(void)
{
    int rc = vfs_mkdir("/mnt");

    if (rc != 0 && rc != KERR_EXIST) {
        return rc;
    }

    return vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0");
}

static int mount_image(enum fat_kind kind)
{
    build_image(kind);
    if (g_image.data == NULL) {
        return KERR_INVAL;
    }

    return mount_built();
}

static int read_file(const char *path, char *buf, k_size_t size)
{
    struct vfs_file file;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, g_file_data);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_read(&file, buf, size);
    vfs_close(&file);

    return rc;
}

static void test_mount_and_stat(void)
{
    struct vfs_dirent entries[4];
    struct vfs_stat stat;
    k_size_t count = 0;

    ktest_begin("fat: a FAT32 volume is mounted next to MicroFS");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_mount_count(), 2);
    KT_CHECK_STR_EQ(vfs_mount_name(1), TEST_ROOT_POINT);

    /* The mount point of the FAT volume is a directory of the tree even
     * though the volume of the root knows nothing about it. */
    KT_CHECK_INT_EQ(vfs_list("/mnt", entries, 4, &count), 0);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_STR_EQ(entries[0].name, "fat");
    KT_CHECK_INT_EQ(entries[0].type, 2);

    /* The FAT root is a directory without a mode, the way procfs and devfs
     * are, and its objects carry the same zero. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT, &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);
    KT_CHECK_INT_EQ(stat.mode, 0);  /* FAT has no modes */
    KT_CHECK_INT_EQ(stat.owner, 0); /* and no owners */

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1);
    KT_CHECK_INT_EQ(stat.size, 10);
    KT_CHECK_INT_EQ(stat.mode, 0);
    KT_CHECK_INT_EQ(stat.owner, 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/sub", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    teardown();
}

static void test_read_and_case(void)
{
    char buffer[64];
    struct vfs_file file;
    int rc;

    ktest_begin("fat: a file is read and its name is found without case");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/hello.txt", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "hello fat\n");

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/sub/inner.txt", buffer, sizeof(buffer)), 6);
    KT_CHECK_STR_EQ(buffer, "inner\n");

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/sub/NESTED/deep.txt", buffer, sizeof(buffer)), 5);
    KT_CHECK_STR_EQ(buffer, "deep\n");

    /* An empty file has no cluster and no byte. */
    rc = vfs_open(TEST_ROOT_POINT "/EMPTY.TXT", VFS_O_RDONLY, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 0);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    /* The end of the file is an empty answer. */
    rc = vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, 4), 4);
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, 4), 4);
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, 4), 2);
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, 4), 0);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    teardown();
}

static void test_long_names(void)
{
    struct vfs_dirent entries[16];
    k_size_t count = 0;
    k_size_t i;
    int saw_long = 0;
    int saw_truncated = 0;
    char buffer[64];

    ktest_begin("fat: the long names come out of UTF-16 and the listing is bounded");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    KT_CHECK(count >= 5);

    for (i = 0; i < count; i++) {
        KT_CHECK(strlen(entries[i].name) <= VFS_NAME_MAX);

        if (strcmp(entries[i].name, "Long File Name.txt") == 0) {
            saw_long = 1;
            KT_CHECK_INT_EQ(entries[i].type, 1);
            KT_CHECK_INT_EQ(entries[i].size, 10);
        }

        if (strncmp(entries[i].name, "A rather long", 13) == 0) {
            saw_truncated = 1;
        }
    }

    KT_CHECK(saw_long);

    /* A name longer than the window of a dirent is cut in the listing, but
     * a path that names it whole still reaches the file. */
    KT_CHECK(saw_truncated);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/long file name.txt", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "long name\n");
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/a RATHER LONG FILE NAME THAT WILL NOT FIT.TXT",
                              buffer, sizeof(buffer)),
                    10);
    KT_CHECK_STR_EQ(buffer, "long file\n");

    teardown();
}

static void test_readdir(void)
{
    struct vfs_file file;
    struct vfs_dirent entries[8];
    k_size_t count = 0;
    k_size_t total = 0;
    k_size_t i;
    int saw_hello = 0;
    int saw_sub = 0;
    int saw_lfn = 0;
    int rc;

    ktest_begin("fat: a directory is read through its descriptor");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    rc = vfs_open(TEST_ROOT_POINT, VFS_O_RDONLY | VFS_O_DIRECTORY, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        for (;;) {
            KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 8, &count), 0);
            if (count == 0) {
                break;
            }
            total += count;
            for (i = 0; i < count; i++) {
                if (strcmp(entries[i].name, "HELLO.TXT") == 0) {
                    saw_hello = 1;
                }
                if (strcmp(entries[i].name, "SUB") == 0 && entries[i].type == 2) {
                    saw_sub = 1;
                }
                if (strcmp(entries[i].name, "Long File Name.txt") == 0) {
                    saw_lfn = 1;
                }
            }
            if (total > 64) {
                break;
            }
        }
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(total, 5); /* the root of the image: no dot entries */
    KT_CHECK(saw_hello);
    KT_CHECK(saw_sub);
    KT_CHECK(saw_lfn);

    /* The subdirectory hides its own "." and "..". */
    rc = vfs_open(TEST_ROOT_POINT "/sub", VFS_O_RDONLY | VFS_O_DIRECTORY, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 8, &count), 0);
        KT_CHECK_INT_EQ(count, 2);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    teardown();
}

static void test_errors(void)
{
    struct vfs_file file;
    struct vfs_dirent entries[1];
    struct vfs_stat stat;
    k_size_t count = 0;

    ktest_begin("fat: a path that is not there and an object of the wrong type");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/missing.txt", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT/inside", &stat), KERR_NOTDIR);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/missing.txt", VFS_O_RDONLY, &file, g_file_data),
                    KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/sub", VFS_O_RDONLY, &file, g_file_data), KERR_ISDIR);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY | VFS_O_DIRECTORY, &file, g_file_data),
        KERR_NOTDIR);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 1, &count), KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

static void test_no_mode_no_owner(void)
{
    struct vfs_file file;
    struct vfs_statfs volume;
    struct vfs_stat stat;
    char byte = 'x';

    ktest_begin("fat: a volume has no modes, no owners and no time");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_chmod(TEST_ROOT_POINT "/HELLO.TXT", 0644), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_chown(TEST_ROOT_POINT "/HELLO.TXT", 7), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_chgrp(TEST_ROOT_POINT "/HELLO.TXT", 7), KERR_NOTSUP);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0);
    KT_CHECK_INT_EQ(stat.owner, 0);

    /* The free map is a walk of the table, and the volume reports it. */
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &volume), 0);
    KT_CHECK_INT_EQ(volume.block_size, g_image.cluster_bytes);
    KT_CHECK_INT_EQ(volume.total_blocks, g_image.cluster_count);
    KT_CHECK(volume.free_blocks > 0);
    KT_CHECK(volume.free_blocks < volume.total_blocks);

    /* A descriptor that was opened for reading is not a place to write. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, &byte, 1), KERR_BADF);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* The text of the refusal does not come from the first mounted
     * filesystem: MicroFS does not know this code. */
    KT_CHECK_STR_EQ(vfs_strerror(KERR_NOTSUP), "operation not supported");

    teardown();
}

/* Write a whole buffer and close the descriptor; a number is the answer of
 * the filesystem, an error is its code. */
static int write_file(const char *path, const void *data, k_size_t size, int flags)
{
    struct vfs_file file;
    int rc;

    rc = vfs_open(path, VFS_O_RDWR | flags, &file, g_file_data);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_write(&file, data, size);
    vfs_close(&file);

    return rc;
}

/* ------------------------------------------------------------------------
 * The oracle of the rest of the world
 *
 * A volume of FAT is a promise to another machine, and the promise is not
 * kept by the reader agreeing with the writer that wrote the image. The
 * host tools of mtools and dosfsck are the other side of the promise: they
 * read what this filesystem wrote, and this filesystem reads and changes
 * what they wrote. The tests below hand the image to them through the host
 * shell of the machine that runs the tests.
 * --------------------------------------------------------------------- */

/* Run a command of the host; the answer is the code the program ended with. */
static int run_shell(const char *format, ...)
{
    char command[1024];
    va_list args;
    int status;

    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);

    status = system(command);
    if (status == -1) {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int tool_present(const char *tool)
{
    return run_shell("command -v %s >/dev/null 2>&1", tool) == 0;
}

/* A file of the host to hold the image and the answers of the tools. */
static int temp_path(char *path, k_size_t cap)
{
    const char *dir = getenv("TMPDIR");
    int fd;

    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }

    if (snprintf(path, cap, "%s/microos-fat-XXXXXX", dir) >= (int)cap) {
        return -1;
    }

    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }

    close(fd);
    return 0;
}

static int image_save(const char *path)
{
    size_t bytes = (size_t)g_image.total_sectors * g_image.bytes_per_sector;
    FILE *file = fopen(path, "wb");
    size_t wrote;

    if (file == NULL) {
        return -1;
    }

    wrote = fwrite(g_image.data, 1, bytes, file);
    fclose(file);

    return (wrote == bytes) ? 0 : -1;
}

/* Read an image that a host tool made and describe its geometry, so that
 * the reader of this system can mount it. */
static int image_load(const char *path)
{
    FILE *file;
    long length;
    k_u8_t *data;
    k_u32_t total16;
    k_u32_t total32;
    k_u32_t fat16;
    k_u32_t fat32;
    k_u32_t root_dir_sectors;

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 512 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    data = (k_u8_t *)malloc((size_t)length);
    if (data == NULL) {
        fclose(file);
        return -1;
    }

    if (fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);

    image_free(&g_image);

    total16 = get16(data + 19);
    total32 = get32(data + 32);
    fat16 = get16(data + 22);
    fat32 = get32(data + 36);

    g_image.data = data;
    g_image.bytes_per_sector = get16(data + 11);
    g_image.sectors_per_cluster = data[13];
    g_image.reserved_sectors = get16(data + 14);
    g_image.num_fats = data[16];
    g_image.root_entries = get16(data + 17);
    g_image.total_sectors = (total16 != 0) ? total16 : total32;
    g_image.fat_sectors = (fat16 != 0) ? fat16 : fat32;
    g_image.cluster_bytes = g_image.sectors_per_cluster * g_image.bytes_per_sector;
    root_dir_sectors =
        (g_image.root_entries * 32u + g_image.bytes_per_sector - 1u) / g_image.bytes_per_sector;
    g_image.root_dir_start = g_image.reserved_sectors + g_image.num_fats * g_image.fat_sectors;
    g_image.first_data_sector = g_image.root_dir_start + root_dir_sectors;
    g_image.cluster_count =
        (g_image.total_sectors - g_image.first_data_sector) / g_image.sectors_per_cluster;

    if (fat16 == 0) {
        g_image.type = 32;
        g_image.root_cluster = get32(data + 44);
    } else {
        g_image.type = (g_image.cluster_count < 4085u) ? 12 : 16;
        g_image.root_cluster = 0;
    }

    g_fat_device.data = g_image.data;
    g_fat_device.sectors = (k_u32_t)((size_t)length / TEST_DEV_SECTOR);
    g_fat_blockdev.sectors = g_fat_device.sectors;

    return 0;
}

static int host_file_read(const char *path, char *buf, k_size_t cap)
{
    FILE *file = fopen(path, "rb");
    size_t got;

    if (file == NULL) {
        return -1;
    }

    got = fread(buf, 1, cap - 1u, file);
    buf[got] = '\0';
    fclose(file);

    return (int)got;
}

static void test_write_create_read(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char buffer[64];

    ktest_begin("fat: a file is created, written, read and removed");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    g_sync_count = 0;

    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/new.txt", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(vfs_write(&file, " fat", 4), 4);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK(g_sync_count > 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/new.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1);
    KT_CHECK_INT_EQ(stat.size, 9);
    KT_CHECK_INT_EQ(stat.mode, 0);
    KT_CHECK_INT_EQ(stat.owner, 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/new.txt", buffer, sizeof(buffer)), 9);
    KT_CHECK_STR_EQ(buffer, "hello fat");

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/new.txt"), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/new.txt", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/new.txt"), KERR_NOENT);

    teardown();
}

static void test_write_rewrite_append(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char buffer[64];
    k_size_t i;

    ktest_begin("fat: a file is rewritten, appended and grown across clusters");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/plain.txt", "abcdef", 6, VFS_O_CREAT), 6);

    /* Overwrite in the middle: the descriptor decides where the bytes go. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/plain.txt", VFS_O_RDWR, &file, g_file_data), 0);
    file.offset = 2;
    KT_CHECK_INT_EQ(vfs_write(&file, "XY", 2), 2);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/plain.txt", buffer, sizeof(buffer)), 6);
    KT_CHECK_STR_EQ(buffer, "abXYef");

    /* Append: the write starts at the end of the file, not at the offset
     * the descriptor happens to have. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/plain.txt", "!", 1, VFS_O_APPEND), 1);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/plain.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 7);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/plain.txt", buffer, sizeof(buffer)), 7);
    KT_CHECK_STR_EQ(buffer, "abXYef!");

    /* Truncate: the chain goes back to the free map and the file is new. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/plain.txt", "z", 1, VFS_O_TRUNC), 1);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/plain.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 1);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/plain.txt", buffer, sizeof(buffer)), 1);
    KT_CHECK_STR_EQ(buffer, "z");

    /* A write of more than one cluster, byte for byte. */
    for (i = 0; i < sizeof(g_io_buffer); i++) {
        g_io_buffer[i] = (k_u8_t)('A' + (i % 26u));
    }
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/big.bin", g_io_buffer, sizeof(g_io_buffer),
                               VFS_O_CREAT | VFS_O_TRUNC),
                    (int)sizeof(g_io_buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/big.bin", (char *)g_io_buffer, sizeof(g_io_buffer)),
                    (int)sizeof(g_io_buffer));
    KT_CHECK_INT_EQ(g_io_buffer[0], 'A');
    KT_CHECK_INT_EQ(g_io_buffer[511], (k_u8_t)('A' + (511 % 26)));
    KT_CHECK_INT_EQ(g_io_buffer[512], (k_u8_t)('A' + (512 % 26)));
    KT_CHECK_INT_EQ(g_io_buffer[sizeof(g_io_buffer) - 1],
                    (k_u8_t)('A' + ((sizeof(g_io_buffer) - 1) % 26)));

    /* An existing file of the image keeps its entry and takes new bytes. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/HELLO.TXT", "rewritten", 9, VFS_O_TRUNC), 9);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/HELLO.TXT", buffer, sizeof(buffer)), 9);
    KT_CHECK_STR_EQ(buffer, "rewritten");

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/plain.txt"), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/big.bin"), 0);

    teardown();
}

static void test_mkdir_and_long_names(void)
{
    struct vfs_dirent entries[16];
    struct vfs_stat stat;
    k_size_t count = 0;
    k_size_t i;
    char buffer[64];
    int saw_dir = 0;

    ktest_begin("fat: a directory and a long name are created on the volume");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/A Long Directory Name"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/A Long Directory Name"), KERR_EXIST);

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/A Long Directory Name/a long file name inside.txt",
                               "inside", 6, VFS_O_CREAT),
                    6);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/A Long Directory Name/a long file name inside.txt",
                              buffer, sizeof(buffer)),
                    6);
    KT_CHECK_STR_EQ(buffer, "inside");

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i].name, "A Long Directory Name") == 0) {
            saw_dir = 1;
            KT_CHECK_INT_EQ(entries[i].type, 2);
        }
    }
    KT_CHECK(saw_dir);

    /* A directory that holds a file is not removable; unlink of a directory
     * is the wrong call of POSIX. */
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/A Long Directory Name"), KERR_ISDIR);
    KT_CHECK_INT_EQ(vfs_rmdir(TEST_ROOT_POINT "/A Long Directory Name"), KERR_NOTEMPTY);
    KT_CHECK_INT_EQ(
        vfs_unlink(TEST_ROOT_POINT "/A Long Directory Name/a long file name inside.txt"), 0);
    KT_CHECK_INT_EQ(vfs_rmdir(TEST_ROOT_POINT "/A Long Directory Name"), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/A Long Directory Name", &stat), KERR_NOENT);

    /* Two names that cut to the same 8.3 name: the second gets a "~N". */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/abcdefgh one.txt", "one", 3, VFS_O_CREAT), 3);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/abcdefgh two.txt", "two", 3, VFS_O_CREAT), 3);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/abcdefgh two.txt", buffer, sizeof(buffer)), 3);
    KT_CHECK_STR_EQ(buffer, "two");

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    {
        int saw_one = 0;
        int saw_two = 0;

        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "abcdefgh one.txt") == 0) {
                saw_one = 1;
            }
            if (strcmp(entries[i].name, "abcdefgh two.txt") == 0) {
                saw_two = 1;
            }
        }

        KT_CHECK(saw_one);
        KT_CHECK(saw_two);
    }

    /* A name that is a plain 8.3 name needs no long entry, and the listing
     * shows exactly what the caller wrote. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/SHORT.TXT", "s", 1, VFS_O_CREAT), 1);
    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    {
        int saw_short = 0;

        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "SHORT.TXT") == 0) {
                saw_short = 1;
            }
        }

        KT_CHECK(saw_short);
    }

    /* A name beyond ASCII: the units go down as UTF-16 and the listing
     * gives the same bytes back. */
    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/\xd0\xb8\xd0\xbc\xd1\x8f.txt", "utf8", 4, VFS_O_CREAT), 4);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(
        read_file(TEST_ROOT_POINT "/\xd0\xb8\xd0\xbc\xd1\x8f.txt", buffer, sizeof(buffer)), 4);
    KT_CHECK_STR_EQ(buffer, "utf8");
    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    {
        int saw_utf8 = 0;

        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "\xd0\xb8\xd0\xbc\xd1\x8f.txt") == 0) {
                saw_utf8 = 1;
            }
        }

        KT_CHECK(saw_utf8);
    }
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/\xd0\xb8\xd0\xbc\xd1\x8f.txt"), 0);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/abcdefgh one.txt"), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/abcdefgh two.txt"), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/SHORT.TXT"), 0);

    teardown();
}

static void test_free_space(void)
{
    struct vfs_statfs before;
    struct vfs_statfs after;
    struct vfs_statfs freed;
    k_u64_t used;

    ktest_begin("fat: the free clusters go down with a write and come back");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);
    KT_CHECK(before.free_blocks > 8);

    memset(g_io_buffer, 0x5A, sizeof(g_io_buffer));
    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/space.bin", g_io_buffer, 3 * 512 + 100, VFS_O_CREAT),
        3 * 512 + 100);

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    used = before.free_blocks - after.free_blocks;
    KT_CHECK_INT_EQ(used, 4); /* three clusters and the fourth for 100 bytes */

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/space.bin"), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &freed), 0);
    KT_CHECK_INT_EQ(freed.free_blocks, before.free_blocks);

    teardown();
}

static void test_remount_persists(void)
{
    struct vfs_stat stat;
    char buffer[32];
    k_u32_t fat_bytes;

    ktest_begin("fat: the writes are on the medium after a remount");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/kept.txt", "still here", 10, VFS_O_CREAT), 10);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/kept"), 0);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/kept/inside.txt", "deep", 4, VFS_O_CREAT), 4);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/kept.txt", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "still here");
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/kept/inside.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 4);

    /* Both copies of the table say the same thing: a write keeps them
     * together, the way a checker expects. */
    fat_bytes = g_image.fat_sectors * g_image.bytes_per_sector;
    KT_CHECK_INT_EQ(memcmp(g_image.data + g_image.reserved_sectors * g_image.bytes_per_sector,
                           g_image.data + (g_image.reserved_sectors + g_image.fat_sectors) *
                                              g_image.bytes_per_sector,
                           fat_bytes),
                    0);

    teardown();
}

static void test_fixed_root_write(enum fat_kind kind, const char *title)
{
    struct vfs_stat stat;
    char buffer[32];
    int rc;

    ktest_begin(title);

    setup();
    rc = mount_image(kind);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/WORK.TXT", "fixed root", 10, VFS_O_CREAT), 10);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/WORK.TXT", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "fixed root");

    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/DIR1"), 0);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/DIR1/IN.TXT", "inner", 5, VFS_O_CREAT), 5);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/DIR1/IN.TXT", buffer, sizeof(buffer)), 5);
    KT_CHECK_STR_EQ(buffer, "inner");

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/DIR1/IN.TXT"), 0);
    KT_CHECK_INT_EQ(vfs_rmdir(TEST_ROOT_POINT "/DIR1"), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/WORK.TXT"), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/WORK.TXT", &stat), KERR_NOENT);

    teardown();
}

static void test_read_only_medium(void)
{
    struct vfs_file file;
    k_u8_t byte = 0;

    ktest_begin("fat: a medium without a write of its own takes no write");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    /* The reader still works: a device that cannot be written is a card
     * whose switch says so, and the volume on it is readable. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_read(&file, g_io_buffer, 4), 4);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* Take the write away for the length of one open. */
    {
        static const struct k_blockdev_ops read_only_ops = {
            .read = test_read,
            .write = NULL,
            .erase = NULL,
            .sync = NULL,
        };
        const struct k_blockdev_ops *saved = g_fat_blockdev.ops;

        g_fat_blockdev.ops = &read_only_ops;
        KT_CHECK_INT_EQ(
            vfs_open(TEST_ROOT_POINT "/nope.txt", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data),
            KERR_NOTSUP);
        KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDWR, &file, g_file_data), 0);
        KT_CHECK_INT_EQ(vfs_write(&file, &byte, 1), KERR_NOTSUP);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
        g_fat_blockdev.ops = saved;
    }

    teardown();
}

static void test_full_volume(void)
{
    struct vfs_file file;
    struct vfs_statfs before;
    struct vfs_statfs after;
    struct vfs_stat stat;
    k_u32_t written = 0;
    int rc;

    ktest_begin("fat: a full volume says ENOSPC and gives the space back");

    setup();
    if (mount_image(FAT_KIND_12) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);
    KT_CHECK(before.free_blocks > 0);

    memset(g_io_buffer, 0xA5, 512);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/BIG.BIN", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data), 0);

    for (;;) {
        rc = vfs_write(&file, g_io_buffer, 512);
        if (rc < 0) {
            break;
        }
        written += (k_u32_t)rc;
        if (written > 1024u * 1024u) {
            break;
        }
    }

    KT_CHECK_INT_EQ(rc, KERR_NOSPC);
    KT_CHECK(written > 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* A write that does not fit leaves the file as it was: no half cluster
     * and no size that promises bytes that are not there. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/BIG.BIN", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, written);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/BIG.BIN"), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks);

    teardown();
}

static void test_full_fixed_root(void)
{
    char name[16];
    char path[48];
    struct vfs_file file;
    int created = 0;
    int rc = 0;
    int i;

    ktest_begin("fat: the fixed root of the old cards stops at its last entry");

    setup();
    if (mount_image(FAT_KIND_12) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    for (i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), "F%02d.TXT", i);
        snprintf(path, sizeof(path), "%s/%s", TEST_ROOT_POINT, name);

        rc = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data);
        if (rc != 0) {
            break;
        }
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
        created++;
    }

    KT_CHECK_INT_EQ(rc, KERR_NOSPC);
    KT_CHECK(created > 0);
    KT_CHECK(created < 32);

    /* The space of a removed entry is usable again. */
    snprintf(path, sizeof(path), "%s/F%02d.TXT", TEST_ROOT_POINT, created - 1);
    KT_CHECK_INT_EQ(vfs_unlink(path), 0);
    KT_CHECK_INT_EQ(vfs_open(path, VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

static void test_write_errors(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statfs volume;

    ktest_begin("fat: a write refuses what the volume cannot hold");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/nodir/file.txt", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data),
        KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/sub", VFS_O_RDWR, &file, g_file_data), KERR_ISDIR);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/HELLO.TXT"), KERR_EXIST);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/no.txt"), KERR_NOENT);

    /* An offset of 4 GiB is the end of what a FAT file can say. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/far.bin", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data), 0);
    file.offset = 0xFFFFFFF0u;
    KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), KERR_NOSPC);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/far.bin"), 0);

    /* A path into a file is not a directory. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/HELLO.TXT/inside", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data),
        KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &volume), 0);

    teardown();
}

static void check_fixed_root(enum fat_kind kind, const char *title)
{
    struct vfs_stat stat;
    struct vfs_dirent entries[8];
    k_size_t count = 0;
    char buffer[32];
    int rc;

    ktest_begin(title);

    setup();
    rc = mount_image(kind);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1);
    KT_CHECK_INT_EQ(stat.size, 10);
    KT_CHECK_INT_EQ(stat.mode, 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/hello.txt", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "hello fat\n");
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/sub/inner.txt", buffer, sizeof(buffer)), 6);
    KT_CHECK_STR_EQ(buffer, "inner\n");

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 8, &count), 0);
    KT_CHECK_INT_EQ(count, 4);

    teardown();
}

static void test_fat12_and_fat16(void)
{
    check_fixed_root(FAT_KIND_12, "fat: FAT12 with the fixed root of the old cards");
    check_fixed_root(FAT_KIND_16, "fat: FAT16 with the fixed root");
}

/*
 * The VFS answers a call a filesystem does not have with KERR_NOTSUP, not
 * with EPERM: the operation is not forbidden by the mode of an object, the
 * filesystem has no such call. procfs is the smallest filesystem with all
 * four of them empty.
 */
static void test_null_operations(void)
{
    struct vfs_statfs volume;

    ktest_begin("vfs: a call a filesystem does not have is ENOTSUP");

    setup();
    KT_CHECK_INT_EQ(procfs_register("/proc"), 0);

    KT_CHECK_INT_EQ(vfs_chmod("/proc/version", 0644), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_chown("/proc/version", 7), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_chgrp("/proc/version", 7), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_mkdir("/proc/newdir"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_unlink("/proc/version"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_statfs("/proc", &volume), KERR_NOTSUP);

    KT_CHECK_INT_EQ(vfs_umount("/proc"), 0);
    teardown();
}

static void test_types_and_bad_image(void)
{
    k_u8_t blank[4096];
    struct k_blockdev *dev = &g_fat_blockdev;
    k_u32_t saved_sectors;
    k_u8_t *saved_data;

    ktest_begin("fat: the type registry and a boot sector that is not one");

    setup();

    KT_CHECK_INT_EQ(vfs_type_count(), 1);
    KT_CHECK_STR_EQ(vfs_type_name(0), "fat32");
    KT_CHECK(vfs_type_name(vfs_type_count()) == NULL);

    KT_CHECK_INT_EQ(vfs_register_type("fat32", fat_vfs_mount), 0);
    KT_CHECK_INT_EQ(vfs_register_type(NULL, fat_vfs_mount), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_register_type("", fat_vfs_mount), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_register_type("a-name-longer-than-the-table", fat_vfs_mount), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_register_type("other", NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_unregister_type("no-such-type"), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "no-such-type", "fat0"), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "fat32", "no-such-device"), KERR_NOENT);

    /* A medium that is not a FAT volume is refused before a mount point is
     * made; the blank is all zeros and has no 55 AA of a boot sector. */
    memset(blank, 0, sizeof(blank));
    saved_data = g_fat_device.data;
    saved_sectors = dev->sectors;
    g_fat_device.data = blank;
    g_fat_device.sectors = sizeof(blank) / TEST_DEV_SECTOR;
    dev->sectors = g_fat_device.sectors;

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0"), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    g_fat_device.data = saved_data;
    g_fat_device.sectors = saved_sectors;
    dev->sectors = saved_sectors;

    teardown();
}

static void test_remount(void)
{
    ktest_begin("fat: the volume is mounted again after it is unmounted");

    setup();

    KT_CHECK_INT_EQ(mount_image(FAT_KIND_32), 0);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);

    teardown();
}

/* The bytes of the image, so that a check that wrote would be seen. */
static k_u64_t image_sum(const struct fat_image *img)
{
    size_t bytes = (size_t)img->total_sectors * img->bytes_per_sector;
    k_u64_t sum = 0;
    size_t i;

    for (i = 0; i < bytes; i++) {
        sum = sum * 131u + img->data[i];
    }

    return sum;
}

/*
 * The checker of the volume next to the reader. A clean image has to pass
 * whole, and every kind of damage the reader would meet has to be named:
 * the two copies of the table, a loop, a cross-link, a cluster outside the
 * volume, a broken long name, a lost cluster and a chain too short for its
 * file. Nothing here writes, so the image is the same after every call.
 */
static void test_check_clean(enum fat_kind kind, const char *title)
{
    struct fat_check_report report;
    struct vfs_statfs volume;
    k_u64_t before;
    int files;
    int directories;

    ktest_begin(title);

    setup();
    if (mount_image(kind) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    files = (kind == FAT_KIND_32) ? 6 : 4;
    directories = (kind == FAT_KIND_32) ? 2 : 1;

    before = image_sum(&g_image);
    g_sync_count = 0;

    KT_CHECK_INT_EQ(fat_check(TEST_ROOT_POINT, &report), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.type, g_image.type);
    KT_CHECK_INT_EQ(report.clusters, g_image.cluster_count);
    KT_CHECK_INT_EQ(report.files, (k_u32_t)files);
    KT_CHECK_INT_EQ(report.directories, (k_u32_t)directories);
    KT_CHECK_INT_EQ(report.lost_clusters, 0);

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &volume), 0);
    KT_CHECK_INT_EQ(report.free_clusters, volume.free_blocks);

    /* The device of a mounted volume is the same medium: the check of the
     * name answers with the mounted copy, not with a second reader. */
    KT_CHECK_INT_EQ(fat_check("fat0", &report), 0);
    KT_CHECK_INT_EQ(report.errors, 0);

    /* Read-only: nothing reached the medium and the device was not synced. */
    KT_CHECK_INT_EQ(g_sync_count, 0);
    KT_CHECK_INT_EQ(image_sum(&g_image), before);

    teardown();
}

static void test_check_wide(void)
{
    struct fat_check_report report;
    char path[32];
    int made;
    int i;

    ktest_begin("fat: the check follows many directories beside each other");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    made = 0;
    for (i = 0; i < 80; i++) {
        snprintf(path, sizeof(path), TEST_ROOT_POINT "/dir%02d", i);
        if (vfs_mkdir(path) != 0) {
            break;
        }
        made++;
    }

    KT_CHECK_INT_EQ(made, 80);

    KT_CHECK_INT_EQ(fat_check(TEST_ROOT_POINT, &report), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.directories, 82); /* SUB, NESTED and the eighty */
    KT_CHECK_INT_EQ(report.files, 6);

    teardown();
}

static void test_check_unmounted(void)
{
    struct fat_check_report report;

    ktest_begin("fat: a volume that is not mounted is checked by its device");

    setup();
    build_image(FAT_KIND_32);

    KT_CHECK_INT_EQ(fat_check("fat0", &report), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.files, 6);

    /* A boot sector that is not one is a problem of the report, not a
     * refusal of the call: the person asked about this medium. */
    g_image.data[510] = 0;
    KT_CHECK_INT_EQ(fat_check("fat0", &report), 0);
    KT_CHECK_INT_EQ(report.errors, 1);
    KT_CHECK(strncmp(report.first, "boot sector: ", 13) == 0);

    /* A point is a point and a device is a device. */
    KT_CHECK_INT_EQ(fat_check("no-such-device", &report), KERR_NOENT);
    KT_CHECK_INT_EQ(fat_check("/mnt/nowhere", &report), KERR_INVAL);
    KT_CHECK_INT_EQ(fat_check("", &report), KERR_INVAL);

    teardown();
}

static void check_broken(const char *expected)
{
    struct fat_check_report report;

    KT_CHECK_INT_EQ(fat_check("fat0", &report), 0);
    KT_CHECK(report.errors > 0);
    KT_CHECK(strstr(report.first, expected) != NULL);
}

static void test_check_broken(void)
{
    struct fat_check_report report;
    k_u8_t *lfn;

    ktest_begin("fat: the check names the table, the chains and the names that are broken");

    setup();

    /* One copy of the table says something else than the first. */
    build_image(FAT_KIND_32);
    g_image.data[(g_image.reserved_sectors + g_image.fat_sectors) * g_image.bytes_per_sector +
                 6u * 4u] ^= 0x01u;
    check_broken("copy");

    /* A chain that comes back to itself. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, 6);
    check_broken("loops");

    /* A chain that runs into the chain of another file. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 7, 6);
    check_broken("more than one");

    /* A cluster beyond the last one of the volume. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, g_image.cluster_count + 50u);
    check_broken("outside the volume");

    /* A long name whose checksum does not match its short entry. */
    build_image(FAT_KIND_32);
    lfn = dir_slot(&g_image, g_image.root_cluster, 1);
    lfn[13] ^= 0xFFu;
    lfn = dir_slot(&g_image, g_image.root_cluster, 2);
    lfn[13] ^= 0xFFu;
    check_broken("long name");

    /* A cluster marked used that no chain names. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, g_image.cluster_count + 1u, fat_eoc(&g_image));
    KT_CHECK_INT_EQ(fat_check("fat0", &report), 0);
    KT_CHECK(report.errors > 0);
    KT_CHECK_INT_EQ(report.lost_clusters, 1);
    KT_CHECK(strstr(report.first, "no chain names") != NULL);

    /* The size of a file promises more clusters than its chain holds. */
    build_image(FAT_KIND_32);
    put32(dir_slot(&g_image, g_image.root_cluster, 0) + 28, 1000);
    check_broken("shorter than the file");

    teardown();
}

/*
 * A repair is a write with a decision under it. The images below carry one
 * kind of damage each, the way the check test does; the repair has to name
 * the problem, fix the ones the medium explains, and leave a volume that
 * the check then calls sound. The bytes are compared around every read-only
 * call, because a checker that writes is a formatter nobody asked for.
 */
static void test_repair_lost(void)
{
    struct fat_check_report report;
    k_u64_t before;

    ktest_begin("fat: a repair gives the lost clusters back, and a check never writes");

    setup();
    build_image(FAT_KIND_32);

    image_set_fat(&g_image, g_image.cluster_count + 1u, fat_eoc(&g_image));
    before = image_sum(&g_image);
    g_sync_count = 0;

    /* Without the flag the walk is a report, and the image is untouched. */
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, false), 0);
    KT_CHECK_INT_EQ(report.errors > 0, 1);
    KT_CHECK_INT_EQ(report.lost_clusters, 1);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.unfixed, 0);
    KT_CHECK_INT_EQ(g_sync_count, 0);
    KT_CHECK_INT_EQ(image_sum(&g_image), before);

    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.unfixed, 0);
    KT_CHECK_INT_EQ(report.lost_found, 1);
    KT_CHECK_INT_EQ(report.lost_freed, 1);
    KT_CHECK_INT_EQ(report.lost_clusters, 0);
    KT_CHECK_INT_EQ(report.fixed >= 1, 1);
    KT_CHECK_INT_EQ(g_sync_count > 0, 1);

    /* Both copies of the table say the cluster is free again. */
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, g_image.cluster_count + 1u), 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 1, g_image.cluster_count + 1u), 0);

    /* A second pass has nothing left to do. */
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.lost_freed, 0);

    teardown();
}

static void expect_untouched(const struct fat_check_report *report, k_u64_t before)
{
    KT_CHECK(report->errors > 0);
    KT_CHECK_INT_EQ(report->fixed, 0);
    KT_CHECK_INT_EQ(g_sync_count, 0);
    KT_CHECK_INT_EQ(image_sum(&g_image), before);
}

static void test_repair_read_only(void)
{
    struct fat_check_report report;
    k_u64_t before;

    ktest_begin("fat: a damaged volume is byte for byte the same without -y");

    setup();

    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, 6);
    before = image_sum(&g_image);
    g_sync_count = 0;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, false), 0);
    expect_untouched(&report, before);

    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 7, 6);
    before = image_sum(&g_image);
    g_sync_count = 0;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, false), 0);
    expect_untouched(&report, before);

    build_image(FAT_KIND_32);
    dir_slot(&g_image, g_image.root_cluster, 1)[13] ^= 0xFFu;
    before = image_sum(&g_image);
    g_sync_count = 0;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, false), 0);
    expect_untouched(&report, before);

    build_image(FAT_KIND_32);
    put32(dir_slot(&g_image, g_image.root_cluster, 0) + 28, 1000);
    before = image_sum(&g_image);
    g_sync_count = 0;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, false), 0);
    expect_untouched(&report, before);

    teardown();
}

static void test_repair_chain(void)
{
    struct fat_check_report report;
    k_u8_t *entry;

    ktest_begin("fat: a repair cuts a loop, a cross-link and a cluster outside the volume");

    setup();

    /* A file whose only cluster names itself. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, 6);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.fixed >= 1, 1);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 6), fat_eoc(&g_image));

    KT_CHECK_INT_EQ(mount_built(), 0);
    {
        char text[16];

        KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/HELLO.TXT", text, sizeof(text)), 10);
        KT_CHECK(text[0] == 'h' && text[9] == '\n');
    }
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    /* A chain that runs into the chain of another file: the first name to
     * be walked keeps it, and this one ends at its own last cluster. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 7, 6);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 7), fat_eoc(&g_image));
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 6), fat_eoc(&g_image));

    /* A value that names a cluster the volume does not have. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, g_image.cluster_count + 50u);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 6), fat_eoc(&g_image));

    /* Two names of one chain, at its head: the second name becomes an
     * empty file and the data stays with the first. */
    build_image(FAT_KIND_32);
    entry = dir_slot(&g_image, g_image.root_cluster, 3);
    put16(entry + 26, 6);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    entry = dir_slot(&g_image, g_image.root_cluster, 3);
    KT_CHECK_INT_EQ(get16(entry + 26), 0);
    KT_CHECK_INT_EQ(get32(entry + 28), 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 6), fat_eoc(&g_image));

    /* A directory whose head is a file's cluster is left alone, and the
     * pass does not free any lost cluster either: the subtree it refused
     * to enter could be one. */
    build_image(FAT_KIND_32);
    put16(dir_slot(&g_image, g_image.root_cluster, 4) + 26, 6);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors > 0, 1);
    KT_CHECK_INT_EQ(report.unfixed, report.errors);
    KT_CHECK_INT_EQ(report.lost_found, 0);
    KT_CHECK_INT_EQ(get16(dir_slot(&g_image, g_image.root_cluster, 4) + 26), 6);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 4), fat_eoc(&g_image));

    teardown();
}

static void test_repair_loop_tail(void)
{
    struct fat_check_report report;
    struct vfs_file file;
    struct vfs_stat stat;
    k_u32_t first;
    k_u32_t second;
    k_u32_t third;

    ktest_begin("fat: a loop of a longer chain is cut where the cluster repeats");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    memset(g_io_buffer, 'x', 3u * 512u);
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/LOOP.BIN", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC,
                             &file, g_file_data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, g_io_buffer, 3u * 512u), 3 * 512);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/LOOP.BIN", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 3u * 512u);

    first = stat.inode;
    second = image_get_fat(&g_image, 0, first);
    third = image_get_fat(&g_image, 0, second);
    KT_CHECK(first >= 2u && second >= 2u && third >= 2u);

    /* The last cluster points back at the first: the chain loops. */
    image_set_fat(&g_image, third, first);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.lost_freed, 2);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, first), fat_eoc(&g_image));
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, second), 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, third), 0);

    KT_CHECK_INT_EQ(mount_built(), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/LOOP.BIN", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 512);
    {
        char text[8];

        KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/LOOP.BIN", text, sizeof(text)), 8);
        KT_CHECK(text[0] == 'x');
    }

    teardown();
}

static void test_repair_size(void)
{
    struct fat_check_report report;

    ktest_begin("fat: a repair puts the size of a file right against its chain");

    setup();

    /* A size that promises two clusters of a chain of one: the size is cut. */
    build_image(FAT_KIND_32);
    put32(dir_slot(&g_image, g_image.root_cluster, 0) + 28, 1000);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(get32(dir_slot(&g_image, g_image.root_cluster, 0) + 28), 512);

    /* A chain longer than its file: the tail goes back to the free map. */
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, 6, 200);
    image_set_fat(&g_image, 200, fat_eoc(&g_image));
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.lost_freed, 1);
    KT_CHECK_INT_EQ(get32(dir_slot(&g_image, g_image.root_cluster, 0) + 28), 10);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, 200), 0);

    /* An entry whose first cluster is not in the volume has no data left
     * to name: the entry goes. */
    build_image(FAT_KIND_32);
    put16(dir_slot(&g_image, g_image.root_cluster, 0) + 26, (k_u16_t)(g_image.cluster_count + 50u));
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 0)[0], 0xE5);

    /* The same for a directory: its subtree is already out of reach. */
    build_image(FAT_KIND_32);
    put16(dir_slot(&g_image, g_image.root_cluster, 4) + 26, (k_u16_t)(g_image.cluster_count + 50u));
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 4)[0], 0xE5);

    /* A long name in front of an entry that goes has nothing left to
     * name: the run is removed with it. */
    build_image(FAT_KIND_32);
    put16(dir_slot(&g_image, g_image.root_cluster, 10) + 26,
          (k_u16_t)(g_image.cluster_count + 50u));
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 6)[0], 0xE5);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 7)[0], 0xE5);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 8)[0], 0xE5);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 9)[0], 0xE5);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 10)[0], 0xE5);

    /* An empty file that names a chain is left: the chain could be the one
     * of a directory the walk has not reached yet, and freeing it would
     * throw the directory away. */
    build_image(FAT_KIND_32);
    put32(dir_slot(&g_image, g_image.root_cluster, 0) + 28, 0);
    put16(dir_slot(&g_image, g_image.root_cluster, 0) + 26, 4);
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 1);
    KT_CHECK_INT_EQ(report.unfixed, 1);
    KT_CHECK(strstr(report.first, "empty file") != NULL);
    KT_CHECK_INT_EQ(get16(dir_slot(&g_image, g_image.root_cluster, 4) + 26), 4);

    teardown();
}

static void test_repair_names(void)
{
    struct fat_check_report report;

    ktest_begin("fat: a repair removes a broken run of long names and keeps the 8.3 name");

    setup();

    /* One checksum that does not match the short entry: the whole run
     * cannot be read, so the run goes and the short name stays. */
    build_image(FAT_KIND_32);
    dir_slot(&g_image, g_image.root_cluster, 1)[13] ^= 0xFFu;
    dir_slot(&g_image, g_image.root_cluster, 2)[13] ^= 0xFFu;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 1)[0], 0xE5);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 2)[0], 0xE5);
    KT_CHECK_INT_EQ(memcmp(dir_slot(&g_image, g_image.root_cluster, 3), "LONGFI~1TXT", 11), 0);

    /* The first entry of the run was removed by somebody: what is left is
     * an orphan, and it is removed too. */
    build_image(FAT_KIND_32);
    dir_slot(&g_image, g_image.root_cluster, 1)[0] = 0xE5;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 2)[0], 0xE5);
    KT_CHECK_INT_EQ(memcmp(dir_slot(&g_image, g_image.root_cluster, 3), "LONGFI~1TXT", 11), 0);

    /* A short name that FAT does not allow: the file is only a name, so
     * the entry goes. A directory keeps its name and its subtree, and the
     * report says that it was left. */
    build_image(FAT_KIND_32);
    dir_slot(&g_image, g_image.root_cluster, 0)[0] = '*';
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 0)[0], 0xE5);

    build_image(FAT_KIND_32);
    dir_slot(&g_image, g_image.root_cluster, 4)[0] = '*';
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 1);
    KT_CHECK_INT_EQ(report.unfixed, 1);
    KT_CHECK_INT_EQ(dir_slot(&g_image, g_image.root_cluster, 4)[0], '*');

    teardown();
}

static void test_repair_medium(void)
{
    static const struct k_blockdev_ops read_only_ops = {
        .read = test_read,
        .write = NULL,
        .erase = NULL,
        .sync = NULL,
    };
    struct fat_check_report report;
    const struct k_blockdev_ops *saved;
    k_u64_t before;

    ktest_begin("fat: a repair that cannot write says so and leaves the medium");

    setup();
    build_image(FAT_KIND_32);
    image_set_fat(&g_image, g_image.cluster_count + 1u, fat_eoc(&g_image));
    before = image_sum(&g_image);

    /* A medium that does not answer at all: the check reports it and the
     * repair has nothing to write to. */
    g_fat_device.data = NULL;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 1);
    KT_CHECK(strstr(report.first, "did not answer") != NULL);
    g_fat_device.data = g_image.data;

    /* A medium with a read and no write: the repair stops at the first
     * change and the check still names the damage. */
    saved = g_fat_blockdev.ops;
    g_fat_blockdev.ops = &read_only_ops;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.errors > 0, 1);
    KT_CHECK_INT_EQ(report.unfixed, report.errors);
    KT_CHECK_INT_EQ(image_sum(&g_image), before);
    g_fat_blockdev.ops = saved;

    /* A write that fails in the middle: the pass stops and says so. */
    g_fat_device.fail_write = true;
    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors > 0, 1);
    KT_CHECK_INT_EQ(report.unfixed, report.errors);
    g_fat_device.fail_write = false;

    teardown();
}

static void test_repair_fixed_root(enum fat_kind kind, const char *title)
{
    struct fat_check_report report;
    k_u32_t hello = 3; /* the cluster of HELLO.TXT in both old formats */

    ktest_begin(title);

    setup();
    build_image(kind);

    /* A loop on a file and a cluster at the end of the volume that no
     * chain names: the two repairs of a volume with a fixed root. */
    image_set_fat(&g_image, hello, hello);
    image_set_fat(&g_image, g_image.cluster_count + 1u, fat_eoc(&g_image));

    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.fixed >= 2, 1);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, hello), fat_eoc(&g_image));
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 0, g_image.cluster_count + 1u), 0);
    KT_CHECK_INT_EQ(image_get_fat(&g_image, 1, g_image.cluster_count + 1u), 0);

    teardown();
}

static void test_repair_refused(void)
{
    struct fat_check_report report;
    k_u64_t before;

    ktest_begin("fat: a repair refuses a volume whose two stories disagree");

    setup();

    /* The copies of the table differ and a cluster is lost. The repair
     * cannot know which copy is the volume, so it writes nothing at all,
     * and the lost cluster is still lost when it returns. */
    build_image(FAT_KIND_32);
    g_image.data[(g_image.reserved_sectors + g_image.fat_sectors) * g_image.bytes_per_sector +
                 6u * 4u] ^= 0x01u;
    image_set_fat(&g_image, g_image.cluster_count + 1u, fat_eoc(&g_image));
    before = image_sum(&g_image);

    KT_CHECK_INT_EQ(fat_repair("fat0", &report, true), 0);
    KT_CHECK_INT_EQ(report.errors > 0, 1);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.lost_found, 0);
    KT_CHECK_INT_EQ(report.unfixed, report.errors);
    KT_CHECK(strstr(report.first, "copy") != NULL);
    KT_CHECK_INT_EQ(image_sum(&g_image), before);

    teardown();
}

#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
static void test_check_command(void)
{
    char buf[1024];
    size_t used;
    FILE *sink;

    ktest_begin("fat: fsck fat checks the point, the device and the usage");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        teardown();
        return;
    }

    port_host_set_sink(sink);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat " TEST_ROOT_POINT), KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    KT_CHECK(strstr(buf, "FAT32") != NULL);
    KT_CHECK(strstr(buf, "the volume is consistent") != NULL);

    /* A damaged volume is a failure of the command, and the first problem
     * comes from the volume and not from the shell. */
    image_set_fat(&g_image, 6, 6);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat fat0"), KSHELL_ERROR);

    /* The same volume with -y: the loop is cut, the volume is proven, and
     * the command says what it changed. */
    KT_CHECK_INT_EQ(kshell_execute("fsck fat fat0 -y"), KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';
    KT_CHECK(strstr(buf, "repair(s) applied") != NULL);
    KT_CHECK(strstr(buf, "0 problem(s) after the repair") != NULL);
    KT_CHECK(strstr(buf, "the volume is consistent") != NULL);

    /* A sound volume takes the flag as well and says nothing was left. */
    KT_CHECK_INT_EQ(kshell_execute("fsck fat fat0 --repair"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat " TEST_ROOT_POINT " -y"), KSHELL_OK);

    /* The words that this image cannot use are the usage line, and a point
     * that is not the FAT volume of this reader is named. */
    KT_CHECK_INT_EQ(kshell_execute("fsck fat"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat a b"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat -y"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat fat0 -y extra"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat /mnt/notfat"), KSHELL_ERROR);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';
    KT_CHECK(strstr(buf, "problem(s), the first:") != NULL);
    KT_CHECK(strstr(buf, "not a FAT volume of this machine") != NULL);

    /* MicroFS is still checked the way it was. */
    KT_CHECK_INT_EQ(kshell_execute("fsck"), KSHELL_OK);

    port_host_set_sink(NULL);

    teardown();
}
#endif

static void test_rename(void)
{
    struct vfs_stat stat;
    char buffer[64];

    ktest_begin("fat: rename moves a name and replaces a file");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/bin"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/tmp"), 0);

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/bin/prog.new", "program", 7, VFS_O_CREAT), (int)7);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/tmp/other", "second", 6, VFS_O_CREAT), 6);

    /* The name of the install: the new file becomes the old one. */
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/bin/prog.new", TEST_ROOT_POINT "/bin/prog"), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/bin/prog.new", &stat), KERR_NOENT);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/bin/prog", buffer, sizeof(buffer)), 7);
    KT_CHECK_STR_EQ(buffer, "program");

    /* Another directory of the same volume. */
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/bin/prog", TEST_ROOT_POINT "/tmp/prog"), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/tmp/prog", buffer, sizeof(buffer)), 7);
    KT_CHECK_STR_EQ(buffer, "program");

    /* A file already at the name is replaced, a directory is not. */
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/tmp/prog", TEST_ROOT_POINT "/tmp/other"), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/tmp/other", buffer, sizeof(buffer)), 7);
    KT_CHECK_STR_EQ(buffer, "program");

    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/tmp/other", TEST_ROOT_POINT "/tmp"), KERR_ISDIR);

    /* A directory keeps its parent and a name that is not there is no name. */
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/bin", TEST_ROOT_POINT "/tmp/bin"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/bin/nothing", TEST_ROOT_POINT "/bin/x"),
                    KERR_NOENT);

    /* The name that is gone is gone from the listing of its directory. */
    {
        struct vfs_dirent entries[16];
        k_size_t count = 0;

        KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/bin", entries, 16, &count), 0);
        KT_CHECK_INT_EQ(count, 0);
    }

    teardown();
}

static void test_truncate_zero(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statfs before;
    struct vfs_statfs after;
    char buffer[16];

    ktest_begin("fat: a file is truncated to zero and its chain comes back");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/cut.txt", g_io_buffer, 700, VFS_O_CREAT), 700);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/cut.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 700);

    /* O_TRUNC is the truncate of this ABI: the chain goes before a byte is
     * written, and a descriptor closed without a write leaves the file at
     * zero rather than at the size it had. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDWR | VFS_O_TRUNC, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/cut.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/cut.txt", buffer, sizeof(buffer)), 0);
    KT_CHECK_STR_EQ(buffer, "");

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/cut.txt"), 0);

    teardown();
}

static void test_mtime(void)
{
    struct vfs_statx info;
    k_u64_t stamp;

    ktest_begin("fat: an entry keeps the moment of its last change");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    /* An entry of the image was written by a builder that kept no date. */
    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT "/HELLO.TXT", &info), 0);
    KT_CHECK_INT_EQ(info.mtime, 0);

    /* A file this machine writes carries the moment of the machine: the
     * seconds since the boot, moved into the window FAT can hold. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/stamp.txt", "x", 1, VFS_O_CREAT), 1);
    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT "/stamp.txt", &info), 0);
    KT_CHECK(info.mtime >= 315532800ull);
    KT_CHECK_INT_EQ(info.ctime, info.mtime);
    stamp = info.mtime;

    /* The moment is on the medium: a mount of the same bytes gives it back. */
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0"), 0);
    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT "/stamp.txt", &info), 0);
    KT_CHECK_INT_EQ(info.mtime, stamp);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/stamp.txt"), 0);

    teardown();
}

static void test_oracle_forward(void)
{
    char image[256];
    char out[320];
    char in[320];
    char buffer[2048];
    struct vfs_file file;
    struct vfs_stat stat;
    int i;
    int rc;

    ktest_begin("fat: the volume we write passes fsck.fat and mtools read it");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/written.txt", "hello from micro os\n", 20, VFS_O_CREAT), 20);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/oracle dir"), 0);
    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/oracle dir/nested.txt", "nested\n", 7, VFS_O_CREAT), 7);
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/written.txt", TEST_ROOT_POINT "/renamed.txt"), 0);

    /* The install that renames over a file: the old name's chain has to go
     * back to the map, or fsck.fat finds a chain no name reaches. */
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/target.txt", "old target\n", 11, VFS_O_CREAT), 11);
    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/replacement.txt", "replacement\n", 12, VFS_O_CREAT), 12);
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/replacement.txt", TEST_ROOT_POINT "/target.txt"),
                    0);

    /* A directory grows past its first cluster; the long names inside it
     * make the runs span the new clusters, and "." and ".." stay the two
     * entries a checker expects at the head. */
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/grow"), 0);
    for (i = 0; i < 24; i++) {
        char path[128];

        snprintf(path, sizeof(path), TEST_ROOT_POINT "/grow/a fairly long name %02d.txt", i);
        KT_CHECK_INT_EQ(write_file(path, "dir\n", 4, VFS_O_CREAT), 4);
    }

    /* An empty directory goes, and its cluster comes back to the map. */
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/doomed"), 0);
    KT_CHECK_INT_EQ(vfs_rmdir(TEST_ROOT_POINT "/doomed"), 0);

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/gone.txt", "123456", 6, VFS_O_CREAT), 6);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/gone.txt", VFS_O_RDWR | VFS_O_TRUNC, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/gone.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 0);

    /* A write that runs out of space leaves the file as it was, and the
     * clusters of the attempt are not lost: the checker below proves it. */
    {
        struct vfs_statfs before_full;
        struct vfs_statfs after_full;
        int fill_rc;

        KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before_full), 0);
        KT_CHECK_INT_EQ(
            vfs_open(TEST_ROOT_POINT "/fill.bin", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data), 0);
        memset(g_io_buffer, 0x5A, sizeof(g_io_buffer));
        for (;;) {
            fill_rc = vfs_write(&file, g_io_buffer, sizeof(g_io_buffer));
            if (fill_rc < 0) {
                break;
            }
        }
        KT_CHECK_INT_EQ(fill_rc, KERR_NOSPC);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
        KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/fill.bin"), 0);
        KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after_full), 0);
        KT_CHECK_INT_EQ(after_full.free_blocks, before_full.free_blocks);
    }

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/HELLO.TXT"), 0);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    if (temp_path(image, sizeof(image)) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    if (!tool_present("fsck.fat") || !tool_present("mdir") || !tool_present("mcopy")) {
        printf("    SKIP: fsck.fat, mdir and mcopy of mtools are not installed\n");
        unlink(image);
        teardown();
        return;
    }

    snprintf(out, sizeof(out), "%s.out", image);
    snprintf(in, sizeof(in), "%s.in", image);

    KT_CHECK_INT_EQ(image_save(image), 0);

    rc = run_shell("fsck.fat -n '%s' >'%s' 2>&1", image, out);
    if (rc != 0) {
        host_file_read(out, buffer, sizeof(buffer));
        printf("    fsck.fat: %s\n", buffer);
    }
    KT_CHECK_INT_EQ(rc, 0);

    rc = run_shell("mdir -i '%s' :: >'%s' 2>&1", image, out);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        host_file_read(out, buffer, sizeof(buffer));
        KT_CHECK(strstr(buffer, "renamed.txt") != NULL);
        KT_CHECK(strstr(buffer, "target.txt") != NULL);
        KT_CHECK(strstr(buffer, "replacement.txt") == NULL);
        KT_CHECK(strstr(buffer, "oracle dir") != NULL);
        KT_CHECK(strstr(buffer, "HELLO.TXT") == NULL);
    }

    rc = run_shell("mcopy -i '%s' ::/renamed.txt '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 20);
        KT_CHECK_STR_EQ(buffer, "hello from micro os\n");
    }

    rc = run_shell("mcopy -i '%s' ::/target.txt '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 12);
        KT_CHECK_STR_EQ(buffer, "replacement\n");
    }

    rc = run_shell("mcopy -i '%s' '::/grow/a fairly long name 23.txt' '%s' >/dev/null 2>&1", image,
                   in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 4);
        KT_CHECK_STR_EQ(buffer, "dir\n");
    }

    rc = run_shell("mcopy -i '%s' '::/oracle dir/nested.txt' '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 7);
        KT_CHECK_STR_EQ(buffer, "nested\n");
    }

    /* A file truncated to zero is an empty file to the rest of the world. */
    rc = run_shell("mcopy -i '%s' ::/gone.txt '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 0);
    }

    unlink(in);
    unlink(out);
    unlink(image);
    teardown();
}

static void test_oracle_reverse(void)
{
    char image[256];
    char out[320];
    char in[320];
    char buffer[2048];
    int rc;

    ktest_begin("fat: an image of mkfs.fat is mounted, changed and still clean");

    setup();

    if (temp_path(image, sizeof(image)) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    if (!tool_present("fsck.fat") || !tool_present("mkfs.fat") || !tool_present("mcopy")) {
        printf("    SKIP: fsck.fat, mkfs.fat and mcopy of mtools are not installed\n");
        unlink(image);
        teardown();
        return;
    }

    snprintf(out, sizeof(out), "%s.out", image);
    snprintf(in, sizeof(in), "%s.in", image);

    /* A volume of the world: 36 MiB is enough clusters for mkfs.fat to
     * accept FAT32 without the warning of a volume that is too small. */
    KT_CHECK_INT_EQ(run_shell("dd if=/dev/zero of='%s' bs=1024 count=36864 status=none", image), 0);

    rc = run_shell("mkfs.fat -F 32 -s 1 -n MICROOS '%s' >'%s' 2>&1", image, out);
    if (rc != 0) {
        host_file_read(out, buffer, sizeof(buffer));
        printf("    mkfs.fat: %s\n", buffer);
    }
    KT_CHECK_INT_EQ(rc, 0);

    KT_CHECK_INT_EQ(run_shell("printf 'from the host\\n' > '%s'", in), 0);
    KT_CHECK_INT_EQ(run_shell("mcopy -i '%s' '%s' ::/FROMHOST.TXT", image, in), 0);

    if (image_load(image) != 0) {
        KT_CHECK(0);
        unlink(in);
        unlink(out);
        unlink(image);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(mount_built(), 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/FROMHOST.TXT", buffer, sizeof(buffer)), 14);
    KT_CHECK_STR_EQ(buffer, "from the host\n");

    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/TOHOST.TXT", "written by micro os\n", 20, VFS_O_CREAT), 20);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/hostdir"), 0);
    KT_CHECK_INT_EQ(
        write_file(TEST_ROOT_POINT "/hostdir/INNER.TXT", "inner from us\n", 14, VFS_O_CREAT), 14);
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/TOHOST.TXT", TEST_ROOT_POINT "/FINAL.TXT"), 0);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(image_save(image), 0);

    rc = run_shell("fsck.fat -n '%s' >'%s' 2>&1", image, out);
    if (rc != 0) {
        host_file_read(out, buffer, sizeof(buffer));
        printf("    fsck.fat: %s\n", buffer);
    }
    KT_CHECK_INT_EQ(rc, 0);

    rc = run_shell("mcopy -i '%s' ::/FINAL.TXT '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 20);
        KT_CHECK_STR_EQ(buffer, "written by micro os\n");
    }

    rc = run_shell("mcopy -i '%s' '::/hostdir/INNER.TXT' '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 14);
        KT_CHECK_STR_EQ(buffer, "inner from us\n");
    }

    unlink(in);
    unlink(out);
    unlink(image);
    teardown();
}

static void test_oracle_fixed_root(enum fat_kind kind, const char *title)
{
    char image[256];
    char out[320];
    char in[320];
    char buffer[2048];
    int rc;

    ktest_begin(title);

    setup();
    if (mount_image(kind) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/WORK.TXT", "fixed root\n", 11, VFS_O_CREAT), 11);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/DIR1"), 0);
    KT_CHECK_INT_EQ(write_file(TEST_ROOT_POINT "/DIR1/IN.TXT", "inner\n", 6, VFS_O_CREAT), 6);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/WORK.TXT"), 0);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    if (temp_path(image, sizeof(image)) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    if (!tool_present("fsck.fat") || !tool_present("mcopy")) {
        printf("    SKIP: fsck.fat and mcopy of mtools are not installed\n");
        unlink(image);
        teardown();
        return;
    }

    snprintf(out, sizeof(out), "%s.out", image);
    snprintf(in, sizeof(in), "%s.in", image);

    KT_CHECK_INT_EQ(image_save(image), 0);

    rc = run_shell("fsck.fat -n '%s' >'%s' 2>&1", image, out);
    if (rc != 0) {
        host_file_read(out, buffer, sizeof(buffer));
        printf("    fsck.fat: %s\n", buffer);
    }
    KT_CHECK_INT_EQ(rc, 0);

    rc = run_shell("mcopy -i '%s' '::/DIR1/IN.TXT' '%s' >/dev/null 2>&1", image, in);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(host_file_read(in, buffer, sizeof(buffer)), 6);
        KT_CHECK_STR_EQ(buffer, "inner\n");
    }

    unlink(in);
    unlink(out);
    unlink(image);
    teardown();
}

/*
 * The volume as the root of the machine (ADR 0027): the mount is at "/" and
 * the path the VFS hands the adapter is the whole path of the tree. The
 * volume of the root is the same FAT32 image, read from its root.
 */
static void test_root_mount(void)
{
    struct vfs_stat stat;
    char buffer[32];

    ktest_begin("fat: the volume is the root of the machine");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    (void)vfs_umount(TEST_ROOT_POINT);
    (void)vfs_umount("/");
    mfs_unmount();

    KT_CHECK_INT_EQ(vfs_mount_type("/", "fat32", "fat0"), 0);
    KT_CHECK_STR_EQ(vfs_mount_name(0), "/");
    KT_CHECK_INT_EQ(vfs_stat("/", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    KT_CHECK_INT_EQ(vfs_stat("/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1);
    KT_CHECK_INT_EQ(stat.size, 10);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file("/HELLO.TXT", buffer, sizeof(buffer)), 10);
    KT_CHECK_STR_EQ(buffer, "hello fat\n");

    KT_CHECK_INT_EQ(vfs_umount("/"), 0);

    /* The tree of the test comes back for the cases after this one. */
    mfs_mount(&g_mfs_device);
    mfs_vfs_register("/");

    teardown();
}

void test_fs_fat(void)
{
    test_mount_and_stat();
    test_read_and_case();
    test_long_names();
    test_readdir();
    test_errors();
    test_no_mode_no_owner();
    test_write_create_read();
    test_truncate_zero();
    test_mtime();
    test_oracle_forward();
    test_oracle_reverse();
    test_oracle_fixed_root(FAT_KIND_12, "fat: a FAT12 volume we write passes fsck.fat");
    test_oracle_fixed_root(FAT_KIND_16, "fat: a FAT16 volume we write passes fsck.fat");
    test_rename();
    test_write_rewrite_append();
    test_mkdir_and_long_names();
    test_free_space();
    test_remount_persists();
    test_fixed_root_write(FAT_KIND_12, "fat: a FAT12 volume takes a file and a directory");
    test_fixed_root_write(FAT_KIND_16, "fat: a FAT16 volume takes a file and a directory");
    test_full_volume();
    test_full_fixed_root();
    test_write_errors();
    test_read_only_medium();
    test_fat12_and_fat16();
    test_null_operations();
    test_types_and_bad_image();
    test_remount();
    test_check_clean(FAT_KIND_12, "fat: the check of a clean FAT12 volume");
    test_check_clean(FAT_KIND_16, "fat: the check of a clean FAT16 volume");
    test_check_clean(FAT_KIND_32, "fat: the check of a clean FAT32 volume");
    test_check_wide();
    test_check_unmounted();
    test_check_broken();
    test_repair_lost();
    test_repair_read_only();
    test_repair_chain();
    test_repair_loop_tail();
    test_repair_size();
    test_repair_names();
    test_repair_medium();
    test_repair_refused();
    test_repair_fixed_root(FAT_KIND_12, "fat: a repair of a FAT12 volume with a fixed root");
    test_repair_fixed_root(FAT_KIND_16, "fat: a repair of a FAT16 volume with a fixed root");
#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
    test_check_command();
#endif
    test_root_mount();
}
