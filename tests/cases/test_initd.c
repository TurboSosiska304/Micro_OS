#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "init/initd.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

#define TEST_BLOCKS 128

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;
static int g_calls;
static char g_last[32];

/* A command that remembers how often and with what it was called. */
static int counting_command(int argc, char **argv)
{
    g_calls++;

    if (argc > 1) {
        strncpy(g_last, argv[1], sizeof(g_last) - 1);
        g_last[sizeof(g_last) - 1] = '\0';
    }

    return KSHELL_OK;
}

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

static void write_text(const char *path, const char *text)
{
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    int size = (int)strlen(text);

    KT_CHECK_INT_EQ(vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, text, (k_size_t)size), size);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
}

static void test_commands(void)
{
    ktest_begin("initd: the lines of a script are commands of the shell");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc/init.d"), 0);
    write_text("/etc/init.d/boot", "# the boot\n\ninitdtest first\r\ninitdtest second\n");

    g_calls = 0;
    g_last[0] = '\0';

    KT_CHECK_INT_EQ(initd_run_script("/etc/init.d/boot"), 0);
    KT_CHECK_INT_EQ(g_calls, 2);
    KT_CHECK_STR_EQ(g_last, "second");

    teardown();
}

static void test_comments(void)
{
    ktest_begin("initd: the comments and the empty lines are not commands");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    write_text("/etc/empty", "# a comment\n\n   \n\t\n");

    g_calls = 0;

    KT_CHECK_INT_EQ(initd_run_script("/etc/empty"), 0);
    KT_CHECK_INT_EQ(g_calls, 0);

    teardown();
}

static void test_missing(void)
{
    ktest_begin("initd: a script that is not there is reported");

    setup();

    KT_CHECK(initd_run_script("/etc/none") < 0);

    teardown();
}

static void test_directory(void)
{
    struct vfs_stat stat;

    ktest_begin("initd: the directory of the scripts is created");

    setup();

    /* A fresh volume has neither /etc nor /etc/init.d. */
    KT_CHECK_INT_EQ(initd_run("/etc/init.d"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/etc", &stat), 0);
    KT_CHECK_INT_EQ(vfs_stat("/etc/init.d", &stat), 0);

    teardown();
}

static void test_failure_does_not_stop(void)
{
    ktest_begin("initd: a command that fails does not stop the script");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc/init.d"), 0);
    write_text("/etc/init.d/mixed", "nosuchcommand\ninitdtest after\n");

    g_calls = 0;

    KT_CHECK_INT_EQ(initd_run_script("/etc/init.d/mixed"), 0);
    KT_CHECK_INT_EQ(g_calls, 1);
    KT_CHECK_STR_EQ(g_last, "after");

    teardown();
}

void test_initd(void)
{
    KT_CHECK_INT_EQ(kshell_register("initdtest", "a command for the unit tests", counting_command),
                    0);

    /* The commands print to the console: the test keeps the console quiet. */
    port_host_set_sink(tmpfile());

    test_commands();
    test_comments();
    test_missing();
    test_directory();
    test_failure_does_not_stop();

    port_host_set_sink(NULL);
}
