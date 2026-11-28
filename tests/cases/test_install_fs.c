/*
 * Micro OS — the filesystem of an install, after the format.
 *
 * The L3 lane creates the volume on the partition it just appended. These
 * tests build the same volume in memory, format it with the writer the
 * installer calls, mount it as the root of the tree and ask the VFS for the
 * files: the mount proves the bytes are a filesystem, and the files prove
 * the tree a boot expects is there.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "fs_ext2/ext2.h"
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/install.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"

#define TARGET_SECTOR 1024u
#define TARGET_SECTORS 8192u /* 8 MiB, the size of the QEMU gate */
#define TARGET_NAME "instfs0"

static k_u8_t g_target[TARGET_SECTORS * TARGET_SECTOR];
static struct k_blockdev g_target_dev;
static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];

static int target_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= TARGET_SECTORS || count > TARGET_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, g_target + (k_size_t)sector * TARGET_SECTOR, count * TARGET_SECTOR);
    return 0;
}

static int target_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= TARGET_SECTORS || count > TARGET_SECTORS - sector) {
        return -1;
    }

    memcpy(g_target + (k_size_t)sector * TARGET_SECTOR, buf, count * TARGET_SECTOR);
    return 0;
}

static int target_sync(void *ctx)
{
    K_UNUSED(ctx);
    return 0;
}

static const struct k_blockdev_ops g_target_ops = {
    .read = target_read,
    .write = target_write,
    .sync = target_sync,
};

static int setup(bool ext2)
{
    int rc;

    memset(g_target, 0, sizeof(g_target));
    memset(&g_target_dev, 0, sizeof(g_target_dev));
    g_target_dev.name = TARGET_NAME;
    g_target_dev.sector_size = TARGET_SECTOR;
    g_target_dev.sectors = TARGET_SECTORS;
    g_target_dev.ops = &g_target_ops;
    rc = k_blockdev_register(&g_target_dev);
    if (rc != 0) {
        return rc;
    }

    if (ext2) {
        rc = ext2_vfs_register_type();
        if (rc != 0) {
            return rc;
        }
        rc = ext2_format(&g_target_dev);
    } else {
        rc = mfs_format(&g_target_dev, 256u);
    }

    return rc;
}

static void teardown(void)
{
    (void)vfs_umount("/");
    mfs_unmount();
    (void)k_blockdev_unregister(&g_target_dev);
    (void)vfs_unregister_type("ext2");
    (void)vfs_unregister_type("mfs");
}

static int read_text(const char *path, char *buf, k_size_t size)
{
    struct vfs_file file;
    int got;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, g_file_data);
    if (rc != 0) {
        return rc;
    }
    got = vfs_read(&file, buf, size - 1u);
    rc = vfs_close(&file);
    if (got < 0) {
        return got;
    }
    buf[got] = '\0';

    return rc;
}

static bool is_dir(const char *path)
{
    struct vfs_stat info;

    return vfs_stat(path, &info) == 0 && info.type == 2u;
}

static void check_tree(void)
{
    char text[512];
    struct vfs_stat info;

    /* The five places the boot expects: two are mounted later, two are
     * written, and all five have to exist on the volume itself. */
    KT_CHECK(is_dir("/dev"));
    KT_CHECK(is_dir("/proc"));
    KT_CHECK(is_dir("/tmp"));
    KT_CHECK(is_dir("/mnt"));
    KT_CHECK(is_dir("/etc"));
    KT_CHECK_INT_EQ(vfs_stat("/etc/volumes", &info), 0);

    KT_CHECK_INT_EQ(read_text("/etc/volumes", text, sizeof(text)), 0);
    KT_CHECK_STR_EQ(text, "nvme0p2\n");

    KT_CHECK_INT_EQ(read_text(INSTALL_RECORD_PATH, text, sizeof(text)), 0);
    KT_CHECK(strstr(text, "version: ") != NULL);
    KT_CHECK(strstr(text, "source: fnv1a64:") != NULL);
    KT_CHECK(strstr(text, "uuid: ") != NULL);
    KT_CHECK(strstr(text, "date: ") != NULL);
    KT_CHECK(strstr(text, "boot: uefi") != NULL);
    KT_CHECK(strstr(text, "fs: ") != NULL);
}

static void test_install_ext2(void)
{
    struct install_record rec = {"nvme0p2", "ext2", "uefi", 4096u, 16384u};
    struct ext2_check_report report;
    int rc;

    ktest_begin("install: an ext2 volume mounts with the tree of an install");

    rc = setup(true);
    if (rc != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }
    rc = vfs_mount_type("/", "ext2", TARGET_NAME);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(install_tree_create("/", &rec), 0);
    check_tree();

    rc = ext2_check("/", &report);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(report.errors == 0u);
    KT_CHECK(report.files >= 2u);
    KT_CHECK(report.directories >= 6u);

    teardown();
}

static void test_install_mfs(void)
{
    struct install_record rec = {"nvme0p2", "mfs", "uefi", 4096u, 16384u};
    struct mfs_check_report report;
    int rc;

    ktest_begin("install: a MicroFS volume mounts with the tree of an install");

    rc = setup(false);
    if (rc != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }
    rc = mfs_mount(&g_target_dev);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    rc = mfs_vfs_register("/");
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(install_tree_create("/", &rec), 0);
    check_tree();

    rc = mfs_check(&report);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    KT_CHECK(report.errors == 0u);

    teardown();
}

static void test_dates(void)
{
    char text[32];

    ktest_begin("install: the record prints an honest date");

    install_date(text, sizeof(text), 0u);
    KT_CHECK_STR_EQ(text, "1970-01-01");
    install_date(text, sizeof(text), 1764326400u); /* 2025-11-28 */
    KT_CHECK_STR_EQ(text, "2025-11-28");
}

void test_install_fs(void)
{
    test_install_ext2();
    test_install_mfs();
    test_dates();
}
