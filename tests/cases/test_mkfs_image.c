/*
 * Micro OS — the image of the x86-64 machine, read back by MicroFS.
 *
 * The tool that makes the image (tools/mkfs_mfs.c) links the filesystem of
 * the board; this test is the other side of that promise: the bytes it
 * writes are a volume that the same mfs_* calls mount and read, with the
 * programs and /etc/setting in the places the port asks for at the boot.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/types.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mkfs_mfs.h"

#define TEST_IMAGE_MB 2u
#define TEST_START_LBA 2048u
#define TEST_SECTOR_BYTES 512u
#define TEST_IMAGE_BYTES (TEST_IMAGE_MB * 1024u * 1024u)
#define TEST_BLOCKS ((TEST_IMAGE_BYTES - TEST_START_LBA * TEST_SECTOR_BYTES) / MFS_BLOCK_SIZE)

#define TEST_PROGRAM "MKFS" /* four bytes, the shape of a format that is not run here */
#define TEST_SETTING "hello\n"
#define TEST_EPOCH 1700000000ull

static k_u8_t g_mbr[TEST_SECTOR_BYTES];

/* The objects of the small image: the same list the port's tool writes, with
 * four bytes in the place of a program. */
static const struct mkfs_mfs_entry g_entries[] = {
    {.path = "/bin", .directory = true, .mode = MKFS_MFS_MODE_DIR},
    {.path = "/bin/hello",
     .data = (const k_u8_t *)TEST_PROGRAM,
     .size = strlen(TEST_PROGRAM),
     .mode = MKFS_MFS_MODE_PROGRAM},
    {.path = "/etc", .directory = true, .mode = MKFS_MFS_MODE_DIR},
    {.path = "/etc/init.d", .directory = true, .mode = MKFS_MFS_MODE_DIR},
    {.path = "/etc/setting",
     .data = (const k_u8_t *)TEST_SETTING,
     .size = strlen(TEST_SETTING),
     .mode = MKFS_MFS_MODE_FILE},
};

static k_u32_t read_le32(const k_u8_t *bytes)
{
    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

static bool read_whole(FILE *file, void *buffer, size_t size)
{
    return size == 0 || fread(buffer, 1, size, file) == size;
}

static void test_image_contents(const char *path)
{
    struct mfs_ram_device volume;
    struct k_blockdev device;
    struct mfs_inode inode;
    struct mfs_dirent entry;
    struct mfs_file file;
    struct mfs_check_report report;
    k_u8_t content[32];
    FILE *image;

    image = fopen(path, "rb");
    KT_CHECK(image != NULL);
    if (image == NULL) {
        return;
    }

    /* --- the table of the partitions, as a person's tool writes it */
    KT_CHECK(read_whole(image, g_mbr, sizeof(g_mbr)));
    KT_CHECK_INT_EQ(g_mbr[510], 0x55);
    KT_CHECK_INT_EQ(g_mbr[511], 0xAA);
    KT_CHECK_INT_EQ(g_mbr[446 + 4], 0x83);
    KT_CHECK_INT_EQ(read_le32(g_mbr + 446 + 8), TEST_START_LBA);
    KT_CHECK_INT_EQ(read_le32(g_mbr + 446 + 12),
                    TEST_IMAGE_BYTES / TEST_SECTOR_BYTES - TEST_START_LBA);

    /* --- the partition is a volume of MicroFS */
    KT_CHECK_INT_EQ(mfs_ram_init(&volume, TEST_BLOCKS), 0);
    KT_CHECK(fseek(image, (long)(TEST_START_LBA * TEST_SECTOR_BYTES), SEEK_SET) == 0);
    KT_CHECK(read_whole(image, volume.data, (size_t)TEST_BLOCKS * MFS_BLOCK_SIZE));
    fclose(image);

    mfs_ram_device(&volume, &device);
    KT_CHECK_INT_EQ(mfs_mount(&device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    /* --- /bin and /etc are directories of the volume, not of the image */
    KT_CHECK_INT_EQ(mfs_stat("/bin", &inode, &entry), MFS_OK);
    KT_CHECK((inode.flags & MFS_INODE_DIR) != 0);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MKFS_MFS_MODE_DIR);

    KT_CHECK_INT_EQ(mfs_stat("/etc", &inode, &entry), MFS_OK);
    KT_CHECK((inode.flags & MFS_INODE_DIR) != 0);

    KT_CHECK_INT_EQ(mfs_stat("/etc/init.d", &inode, &entry), MFS_OK);
    KT_CHECK((inode.flags & MFS_INODE_DIR) != 0);

    /* --- a program is a file with the bit of execution and its bytes */
    KT_CHECK_INT_EQ(mfs_stat("/bin/hello", &inode, &entry), MFS_OK);
    KT_CHECK((inode.flags & MFS_INODE_DIR) == 0);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MKFS_MFS_MODE_PROGRAM);
    KT_CHECK_INT_EQ(inode.size, (k_u32_t)strlen(TEST_PROGRAM));
    /* The tool stamps every record with the one moment it was given, not
     * with the clock of the run: that is what makes the image reproducible. */
    KT_CHECK_INT_EQ(inode.mtime, TEST_EPOCH);
    KT_CHECK_INT_EQ(inode.ctime, TEST_EPOCH);

    KT_CHECK_INT_EQ(mfs_open("/bin/hello", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_read(&file, content, sizeof(content)), (int)strlen(TEST_PROGRAM));
    KT_CHECK_INT_EQ(memcmp(content, TEST_PROGRAM, strlen(TEST_PROGRAM)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* --- a file of data: /etc/setting, the one the smoke test cats */
    KT_CHECK_INT_EQ(mfs_stat("/etc/setting", &inode, &entry), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MKFS_MFS_MODE_FILE);
    KT_CHECK_INT_EQ(inode.size, (k_u32_t)strlen(TEST_SETTING));

    KT_CHECK_INT_EQ(mfs_open("/etc/setting", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_read(&file, content, sizeof(content)), (int)strlen(TEST_SETTING));
    KT_CHECK_INT_EQ(memcmp(content, TEST_SETTING, strlen(TEST_SETTING)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    mfs_unmount();
    mfs_ram_free(&volume);
}

static void test_build_and_mount(void)
{
    char path[] = "/tmp/microos-mkfs-XXXXXX";
    int fd;

    ktest_begin("mkfs: the image of the disk is a volume that MicroFS reads");

    fd = mkstemp(path);
    KT_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    close(fd);

    KT_CHECK_INT_EQ(mkfs_mfs_build_image(path, TEST_IMAGE_MB, TEST_START_LBA, g_entries,
                                         K_ARRAY_SIZE(g_entries), TEST_EPOCH),
                    0);

    test_image_contents(path);

    unlink(path);
}

/* Two files of the host, byte by byte, in blocks: an image is megabytes and
 * the test has no reason to hold two of them. */
static bool files_are_equal(const char *one, const char *two)
{
    k_u8_t first[4096];
    k_u8_t second[4096];
    FILE *left = fopen(one, "rb");
    FILE *right = fopen(two, "rb");
    bool same = left != NULL && right != NULL;

    while (same) {
        size_t a = fread(first, 1, sizeof(first), left);
        size_t b = fread(second, 1, sizeof(second), right);

        if (a != b || memcmp(first, second, a) != 0) {
            same = false;
        } else if (a == 0) {
            break;
        }
    }

    if (left != NULL) {
        fclose(left);
    }
    if (right != NULL) {
        fclose(right);
    }

    return same;
}

/*
 * The property of a reproducible build: the tool is given one moment, not
 * the clock of the run, so two images of the same objects are the same
 * bytes. `make reproducible` checks the image of the port; this is the same
 * promise in the unit suite, where a change of the stamps fails at once.
 */
static void test_same_bytes(void)
{
    char one[] = "/tmp/microos-mkfs-one-XXXXXX";
    char two[] = "/tmp/microos-mkfs-two-XXXXXX";
    int fd_one;
    int fd_two;

    ktest_begin("mkfs: two images of the same objects are the same bytes");

    fd_one = mkstemp(one);
    fd_two = mkstemp(two);
    KT_CHECK(fd_one >= 0);
    KT_CHECK(fd_two >= 0);

    if (fd_one < 0 || fd_two < 0) {
        if (fd_one >= 0) {
            close(fd_one);
            unlink(one);
        }
        if (fd_two >= 0) {
            close(fd_two);
            unlink(two);
        }
        return;
    }

    close(fd_one);
    close(fd_two);

    KT_CHECK_INT_EQ(mkfs_mfs_build_image(one, TEST_IMAGE_MB, TEST_START_LBA, g_entries,
                                         K_ARRAY_SIZE(g_entries), TEST_EPOCH),
                    0);
    KT_CHECK_INT_EQ(mkfs_mfs_build_image(two, TEST_IMAGE_MB, TEST_START_LBA, g_entries,
                                         K_ARRAY_SIZE(g_entries), TEST_EPOCH),
                    0);
    KT_CHECK(files_are_equal(one, two));

    unlink(one);
    unlink(two);
}

void test_mkfs_image(void)
{
    test_build_and_mount();
    test_same_bytes();
}
