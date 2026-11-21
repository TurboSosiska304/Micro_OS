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

#include "fs_fat32/fat.h"
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
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
};

static struct mfs_ram_device g_mfs_volume;
static struct k_blockdev g_mfs_device;
static struct fat_image g_image;
static struct test_device g_fat_device;
static struct k_blockdev g_fat_blockdev;
static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];

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
    }

    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, 0x12345678u);
    memcpy(b + 71, "MICROOS    ", 11);
    memcpy(b + 82, label, 8);
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
        image_dir_add(img, root, &index, "NESTED", NULL, 0x10, nested, 0);
    }

    image_dir_add(img, sub, &sub_index, ".", NULL, 0x10, sub, 0);
    image_dir_add(img, sub, &sub_index, "..", NULL, 0x10, root, 0);
    image_dir_add(img, sub, &sub_index, "INNER.TXT", NULL, 0x20, inner, 6);

    if (img->type == 32) {
        image_dir_add(img, sub, &sub_index, "NESTED", NULL, 0x10, nested, 0);
        image_dir_add(img, nested, &nested_index, ".", NULL, 0x10, nested, 0);
        image_dir_add(img, nested, &nested_index, "..", NULL, 0x10, sub, 0);
        image_dir_add(img, nested, &nested_index, "DEEP.TXT", NULL, 0x20, deep, 5);
    }
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
    K_UNUSED(ctx);
    K_UNUSED(sector);
    K_UNUSED(buf);
    K_UNUSED(count);

    /* The reader must never come here. A test that writes to the volume is
     * a bug of the reader and not a feature of the medium. */
    return -1;
}

static const struct k_blockdev_ops g_test_ops = {
    .read = test_read,
    .write = test_write,
    .erase = NULL,
    .sync = NULL,
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

static int mount_image(enum fat_kind kind)
{
    int rc;

    build_image(kind);
    if (g_image.data == NULL) {
        return KERR_INVAL;
    }

    rc = vfs_mkdir("/mnt");
    if (rc != 0 && rc != KERR_EXIST) {
        return rc;
    }

    return vfs_mount_type(TEST_ROOT_POINT, "fat32", "fat0");
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

    KT_CHECK_INT_EQ(total, 6); /* the root of the image: no dot entries */
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

static void test_read_only(void)
{
    struct vfs_file file;
    struct vfs_dirent entries[4];
    struct vfs_statfs volume;
    k_size_t count = 0;
    char byte = 'x';

    ktest_begin("fat: every write is ENOTSUP, not a permission of a file");

    setup();
    if (mount_image(FAT_KIND_32) != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_chmod(TEST_ROOT_POINT "/HELLO.TXT", 0644), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_chown(TEST_ROOT_POINT "/HELLO.TXT", 7), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/HELLO.TXT"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/newdir"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/new.txt", VFS_O_RDWR | VFS_O_CREAT, &file, g_file_data),
        KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/HELLO.TXT", entries, 4, &count), KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &volume), KERR_NOTSUP);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/HELLO.TXT", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, &byte, 1), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* The text of the refusal does not come from the first mounted
     * filesystem: MicroFS does not know this code. */
    KT_CHECK_STR_EQ(vfs_strerror(KERR_NOTSUP), "operation not supported");

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

void test_fs_fat(void)
{
    test_mount_and_stat();
    test_read_and_case();
    test_long_names();
    test_readdir();
    test_errors();
    test_read_only();
    test_fat12_and_fat16();
    test_null_operations();
    test_types_and_bad_image();
    test_remount();
}
