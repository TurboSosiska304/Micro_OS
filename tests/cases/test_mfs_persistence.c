#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

#define TEST_BLOCKS 256

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static int setup(void)
{
    int rc = mfs_ram_init(&g_volume, TEST_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);

    return mfs_format(&g_device, 64);
}

static void test_bitmap_persists(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_file file;

    ktest_begin("mfs: the map of the free blocks survives a remount");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/data.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/data.bin", MFS_O_RDWR, &file), MFS_OK);
    mfs_write(&file, "0123456789", 10);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);

    /* The blocks of the file must not be given out a second time. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free);
    KT_CHECK(after.data_free < after.data_blocks);

    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void test_free_after_unlink(void)
{
    struct mfs_info before;
    struct mfs_info after;

    ktest_begin("mfs: the blocks of a removed file are free again");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/gone.bin"), MFS_OK);
    {
        struct mfs_file file;

        KT_CHECK_INT_EQ(mfs_open("/gone.bin", MFS_O_RDWR, &file), MFS_OK);
        mfs_write(&file, "0123456789", 10);
        mfs_close(&file);
    }

    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);

    KT_CHECK_INT_EQ(mfs_unlink("/gone.bin"), MFS_OK);

    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free + 1);

    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/*
 * The property: the mode and the owner are part of the record of the
 * object, so they are on the volume before the remount, like the data.
 */
static void test_modes_persist(void)
{
    struct mfs_inode inode;

    ktest_begin("mfs: the mode and the owner survive a remount");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/bin/tool"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chmod("/bin", 0750), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chmod("/bin/tool", 0755), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chown("/bin/tool", 1000), MFS_OK);

    /* The restart of the board: only what reached the device stays. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_stat("/bin/tool", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0755);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 1000);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, 0);

    /* The directory kept its own mode and the owner it was made by. */
    KT_CHECK_INT_EQ(mfs_stat("/bin", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0750);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, MFS_INODE_DIR);

    mfs_unmount();
    mfs_ram_free(&g_volume);
}

void test_mfs_persistence(void)
{
    test_bitmap_persists();
    test_free_after_unlink();
    test_modes_persist();
}
