#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "devfs/devfs.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "procfs/procfs.h"

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
    procfs_register("/proc");
    devfs_register("/dev");
}

static void teardown(void)
{
    vfs_umount("/dev");
    vfs_umount("/proc");
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* Read a file of the tree of the directories. */
static int read_file(const char *path, char *buf, k_size_t size)
{
    struct vfs_file file;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, 0);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_read(&file, buf, size);
    vfs_close(&file);

    return rc;
}

static void test_procfs(void)
{
    char buffer[1024];
    int rc;

    ktest_begin("procfs: the files of the system are generated");

    setup();

    rc = read_file("/proc/version", buffer, sizeof(buffer));
    KT_CHECK(rc > 0);
    KT_CHECK(strstr(buffer, "MicroOS") != NULL);

    rc = read_file("/proc/uptime", buffer, sizeof(buffer));
    KT_CHECK(rc > 0);
    KT_CHECK(strstr(buffer, "up ") != NULL);

    rc = read_file("/proc/meminfo", buffer, sizeof(buffer));
    KT_CHECK(rc > 0);
    KT_CHECK(strstr(buffer, "psram") != NULL);

    rc = read_file("/proc/tasks", buffer, sizeof(buffer));
    KT_CHECK(rc > 0);
    KT_CHECK(strstr(buffer, "name") != NULL);

    /* The file that does not exist. */
    KT_CHECK(read_file("/proc/nosuchfile", buffer, sizeof(buffer)) < 0);

    teardown();
}

static void test_procfs_end(void)
{
    char buffer[1024];
    struct vfs_file file;
    int rc;

    ktest_begin("procfs: the end of the file is reached");

    setup();

    /* The file has to end, otherwise the reader loops forever. */
    rc = vfs_open("/proc/uptime", VFS_O_RDONLY, &file, 0);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        int total = 0;
        int chunk;

        while ((chunk = vfs_read(&file, buffer, sizeof(buffer))) > 0) {
            total += chunk;
            if (total > 4096) {
                break; /* the file does not end */
            }
        }

        KT_CHECK(total > 0);
        KT_CHECK(chunk <= 0);
        vfs_close(&file);
    }

    teardown();
}

static void test_devfs(void)
{
    struct vfs_dirent entries[8];
    k_size_t count = 0;
    int rc;

    ktest_begin("devfs: the devices of the system are listed");

    setup();

    rc = vfs_list("/dev", entries, 8, &count);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(count >= 2);

    if (count >= 2) {
        bool found_tty = false;
        k_size_t i;

        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "tty0") == 0) {
                found_tty = true;
            }
        }

        KT_CHECK(found_tty);
    }

    /* A device that does not exist. */
    {
        struct vfs_stat stat;

        KT_CHECK(vfs_stat("/dev/nosuchdevice", &stat) < 0);
    }

    teardown();
}

void test_sysfs(void)
{
    test_procfs();
    test_procfs_end();
    test_devfs();
}
