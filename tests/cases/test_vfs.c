#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#define TEST_BLOCKS 128

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

static void setup(void)
{
    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 32);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void test_mount(void)
{
    ktest_begin("vfs: a filesystem is mounted and the paths are resolved");

    setup();

    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_STR_EQ(vfs_mount_name(0), "/");

    teardown();
}

static void test_file_operations(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    char buffer[64];
    int rc;

    ktest_begin("vfs: a file is created, written and read");

    setup();

    /* A path below the mount point has to be found. */
    rc = vfs_open("/hello.txt", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, file_data);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "hello vfs", 9), 9);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    rc = vfs_open("/hello.txt", VFS_O_RDONLY, &file, file_data);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 9);
        KT_CHECK_STR_EQ(buffer, "hello vfs");
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    teardown();
}

static void test_directories(void)
{
    struct vfs_dirent entries[16];
    struct vfs_stat stat;
    k_size_t count = 0;
    int rc;

    ktest_begin("vfs: the directories are listed");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/etc", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2); /* a directory */

    rc = vfs_list("/", entries, 16, &count);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(count, 1);
    if (count == 1) {
        KT_CHECK_STR_EQ(entries[0].name, "etc");
        KT_CHECK_INT_EQ(entries[0].type, 2);
    }

    KT_CHECK_INT_EQ(vfs_unlink("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_list("/", entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 0);

    /*
     * The codes of the errors of the filesystem are translated into the
     * codes of the contract of the system calls at the adapter. The two
     * sets are close but not the same: "the directory is not empty" is
     * -39 of the kernel, and -9 of the kernel is a "bad file descriptor".
     */
    KT_CHECK_INT_EQ(vfs_mkdir("/full"), 0);
    KT_CHECK_INT_EQ(mfs_create("/full/file.txt"), MFS_OK);
    KT_CHECK_INT_EQ(vfs_unlink("/full"), KERR_NOTEMPTY);
    KT_CHECK_INT_EQ(vfs_unlink("/nothing-here"), KERR_NOENT);

    teardown();
}

static void test_statfs(void)
{
    struct vfs_statfs stat;

    ktest_begin("vfs: the volume reports its size");

    setup();

    KT_CHECK_INT_EQ(vfs_statfs("/", &stat), 0);
    KT_CHECK(stat.total_blocks > 0);
    KT_CHECK(stat.free_blocks > 0);
    KT_CHECK(stat.free_blocks < stat.total_blocks);
    KT_CHECK_INT_EQ(stat.block_size, MFS_BLOCK_SIZE);

    teardown();
}

void test_vfs(void)
{
    test_mount();
    test_file_operations();
    test_directories();
    test_statfs();
}
