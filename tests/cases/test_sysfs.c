#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "devfs/devfs.h"
#include "kernel/task.h"
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

/* The place for the data of the filesystem for an open file of the test. */
static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];

/* Read a file of the tree of the directories. */
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
    rc = vfs_open("/proc/uptime", VFS_O_RDONLY, &file, g_file_data);
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

static void test_procfs_tasks(void)
{
    struct vfs_dirent entries[16];
    struct vfs_stat stat;
    k_size_t count = 0;
    char buffer[512];
    int rc;

    ktest_begin("procfs: every task has a directory");

    setup();

    /* The root holds the files and the directories of the tasks. */
    rc = vfs_list("/proc", entries, 16, &count);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(count >= 2);

    /* The directory of the current task. */
    rc = vfs_list("/proc/self", entries, 16, &count);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(count, 1);
    if (count == 1) {
        KT_CHECK_STR_EQ(entries[0].name, "status");
    }

    /* The file of the task. */
    rc = read_file("/proc/self/status", buffer, sizeof(buffer));
    KT_CHECK(rc > 0);
    KT_CHECK(strstr(buffer, "Name:") != NULL);
    KT_CHECK(strstr(buffer, "Pid:") != NULL);

    KT_CHECK_INT_EQ(vfs_stat("/proc/self/status", &stat), 0);

    teardown();
}

/* The directory of a task: /proc/<pid> is a directory, its status a file. */
static void test_procfs_pid_dir(void)
{
    struct task *self = task_self();
    struct vfs_dirent entries[8];
    struct vfs_stat stat;
    char path[64];
    char buffer[512];
    k_size_t count = 0;
    int rc;

    ktest_begin("procfs: the directory of a task");

    setup();

    /* The point of the mount is a directory. */
    KT_CHECK_INT_EQ(vfs_stat("/proc", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    /* The directory of the task that reads it, and its status as a file. */
    KT_CHECK_INT_EQ(vfs_stat("/proc/self", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    KT_CHECK_INT_EQ(vfs_stat("/proc/self/status", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1);

    if (self != NULL) {
        /* The same by the number: /proc/<pid> and /proc/<pid>/status. */
        snprintf(path, sizeof(path), "/proc/%u", (unsigned)self->pid);
        KT_CHECK_INT_EQ(vfs_stat(path, &stat), 0);
        KT_CHECK_INT_EQ(stat.type, 2);

        snprintf(path, sizeof(path), "/proc/%u/status", (unsigned)self->pid);
        rc = read_file(path, buffer, sizeof(buffer));
        KT_CHECK(rc > 0);
        KT_CHECK(strstr(buffer, "Pid:") != NULL);
        KT_CHECK_INT_EQ(vfs_stat(path, &stat), 0);
        KT_CHECK_INT_EQ(stat.type, 1);

        /* The directory of the task holds its status. */
        snprintf(path, sizeof(path), "/proc/%u", (unsigned)self->pid);
        count = 0;
        rc = vfs_list(path, entries, 8, &count);
        KT_CHECK_INT_EQ(rc, 0);
        KT_CHECK_INT_EQ(count, 1);
        if (count == 1) {
            KT_CHECK_STR_EQ(entries[0].name, "status");
        }
    }

    /* A task that is not there has no directory and no status file. */
    KT_CHECK(vfs_stat("/proc/9999", &stat) < 0);
    KT_CHECK(read_file("/proc/9999/status", buffer, sizeof(buffer)) < 0);

    teardown();
}

void test_sysfs(void)
{
    test_procfs();
    test_procfs_end();
    test_procfs_tasks();
    test_procfs_pid_dir();
    test_devfs();
}
