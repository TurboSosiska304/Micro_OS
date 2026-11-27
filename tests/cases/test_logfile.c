/*
 * Micro OS — the boot capture and the console log.
 *
 * The file log has two generations of at most 64 KiB. The console path only
 * fills a memory ring; the test calls the batch flush itself, which also proves
 * that no filesystem work happens between k_console_write() and its return.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/logfile.h"
#include "kernel/port.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];
static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;
static char g_read[K_LOGFILE_LIMIT + 1u];
static char g_payload[1024];

static int write_text(const char *path, const char *text)
{
    struct vfs_file file;
    k_size_t len = strlen(text);
    int rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);

    if (rc != 0) {
        return rc;
    }

    rc = vfs_write(&file, text, len);
    if (rc >= 0 && (k_size_t)rc != len) {
        rc = KERR_IO;
    } else if (rc == (int)len) {
        rc = 0;
    }
    if (vfs_close(&file) != 0 && rc == 0) {
        rc = KERR_IO;
    }
    return rc;
}

static int read_text(const char *path, char *out, k_size_t size)
{
    struct vfs_file file;
    k_size_t used = 0;
    int rc = vfs_open(path, VFS_O_RDONLY, &file, g_file_data);

    if (rc != 0) {
        return rc;
    }

    while (used + 1u < size) {
        rc = vfs_read(&file, out + used, size - used - 1u);
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            out[used] = '\0';
            (void)vfs_close(&file);
            return (int)used;
        }
        used += (k_size_t)rc;
    }

    out[used] = '\0';
    (void)vfs_close(&file);
    return rc < 0 ? rc : (int)used;
}

static int file_size(const char *path)
{
    struct vfs_stat info;
    int rc = vfs_stat(path, &info);

    return rc == 0 ? (int)info.size : rc;
}

static void quiet_port(void)
{
    port_host_set_sink(tmpfile());
}

static void setup_storage(void)
{
    (void)vfs_umount("/");
    KT_CHECK_INT_EQ(mfs_ram_init(&g_volume, 512u), 0);
    mfs_ram_device(&g_volume, &g_device);
    KT_CHECK_INT_EQ(mfs_format(&g_device, 32u), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_vfs_register("/"), 0);
    quiet_port();
}

static void teardown_storage(void)
{
    k_logfile_stop();
    (void)vfs_umount("/");
    (void)mfs_unmount();
    mfs_ram_free(&g_volume);
    port_host_set_sink(NULL);
}

static void fill_run(k_size_t bytes)
{
    memset(g_payload, 'r', sizeof(g_payload));

    while (bytes > 0) {
        k_size_t chunk = K_MIN(bytes, sizeof(g_payload));

        k_console_write(g_payload, chunk);
        KT_CHECK_INT_EQ(k_logfile_flush(), 0);
        bytes -= chunk;
    }
}

static void test_boot_generations(void)
{
    ktest_begin("logfile: a new boot keeps the previous attempt as bootlog.1.log");

    setup_storage();
    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc"), 0);
    KT_CHECK_INT_EQ(write_text(K_LOGFILE_BOOT_PATH, "previous boot\n"), 0);
    KT_CHECK_INT_EQ(write_text(K_LOGFILE_BOOT_PREVIOUS_PATH, "older boot\n"), 0);
    KT_CHECK_INT_EQ(write_text(K_LOGFILE_RUN_PATH, "old run\n"), 0);

    KT_CHECK_INT_EQ(k_logfile_start("first line\nsecond line\n",
                                    sizeof("first line\nsecond line\n") - 1u, true),
                    0);
    KT_CHECK(!k_logfile_in_ram());
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);

    KT_CHECK(read_text(K_LOGFILE_BOOT_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK_STR_EQ(g_read, "first line\nsecond line\n");
    KT_CHECK(read_text(K_LOGFILE_BOOT_PREVIOUS_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK_STR_EQ(g_read, "previous boot\n");
    KT_CHECK(read_text(K_LOGFILE_RUN_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK(strncmp(g_read, "old run\n", 8u) == 0);

    KT_CHECK_INT_EQ(k_logfile_start("next boot\n", 10u, true), 0);
    KT_CHECK(read_text(K_LOGFILE_BOOT_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK_STR_EQ(g_read, "next boot\n");
    KT_CHECK(read_text(K_LOGFILE_BOOT_PREVIOUS_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK_STR_EQ(g_read, "first line\nsecond line\n");
    KT_CHECK_INT_EQ(vfs_stat("/etc/bootlog.2.log", &(struct vfs_stat){0}), KERR_NOENT);

    teardown_storage();
}

static void test_run_rotation(void)
{
    k_size_t base;
    k_size_t current;

    ktest_begin("logfile: run.log rotates at 64 KiB and keeps only one previous file");

    setup_storage();
    KT_CHECK_INT_EQ(k_logfile_start("boot\n", 5u, true), 0);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);

    base = (k_size_t)file_size(K_LOGFILE_RUN_PATH);
    KT_CHECK(base > 0 && base < K_LOGFILE_LIMIT);
    fill_run(K_LOGFILE_LIMIT - base);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PREVIOUS_PATH), (int)K_LOGFILE_LIMIT);
    current = (k_size_t)file_size(K_LOGFILE_RUN_PATH);
    KT_CHECK_INT_EQ((int)current, (int)sizeof("log: rotated at 65536 bytes\n") - 1);
    KT_CHECK(read_text(K_LOGFILE_RUN_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK_STR_EQ(g_read, "log: rotated at 65536 bytes\n");

    k_console_write("after\n", 6);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PREVIOUS_PATH), (int)K_LOGFILE_LIMIT);
    current = (k_size_t)file_size(K_LOGFILE_RUN_PATH);
    KT_CHECK(current > 0 && current < K_LOGFILE_LIMIT);
    KT_CHECK(read_text(K_LOGFILE_RUN_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK(strstr(g_read, "log: rotated at 65536 bytes\n") == g_read);
    KT_CHECK(strstr(g_read, "after\n") != NULL);

    /* A second rotation replaces the previous generation, never creates a
     * third file, and the new current is again bounded by the same limit. */
    fill_run(K_LOGFILE_LIMIT - current);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PREVIOUS_PATH), (int)K_LOGFILE_LIMIT);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PATH),
                    (int)sizeof("log: rotated at 65536 bytes\n") - 1);
    k_console_write("newest\n", 8);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PREVIOUS_PATH), (int)K_LOGFILE_LIMIT);
    KT_CHECK(file_size(K_LOGFILE_RUN_PATH) < (int)K_LOGFILE_LIMIT);
    KT_CHECK_INT_EQ(vfs_stat("/etc/run.2.log", &(struct vfs_stat){0}), KERR_NOENT);

    teardown_storage();
}

/* A real read-only mount: the fixture opens the write gate to make /etc, then
 * the root policy closes it because /etc/volumes does not name the medium. */
static struct k_blockdev_ops g_gate_ops;
static int (*g_gate_read)(void *ctx, k_u32_t sector, void *buf, k_size_t count);
static int (*g_gate_write)(void *ctx, k_u32_t sector, const void *buf, k_size_t count);
static bool g_gate_open;

static int gated_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    return g_gate_read(ctx, sector, buf, count);
}

static int gated_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    return g_gate_open ? g_gate_write(ctx, sector, buf, count) : -1;
}

static int gated_writes(void *ctx, bool open)
{
    K_UNUSED(ctx);
    g_gate_open = open;
    return 0;
}

static void test_read_only_ram(void)
{
    struct mfs_ram_device memory;
    struct k_blockdev root;
    struct k_logfile_snapshot snapshot;

    ktest_begin("logfile: a read-only root keeps the log in RAM and says so");

    (void)vfs_umount("/");
    KT_CHECK_INT_EQ(mfs_ram_init(&memory, 128u), 0);
    mfs_ram_device(&memory, &root);
    root.name = "logro";
    g_gate_read = root.ops->read;
    g_gate_write = root.ops->write;
    g_gate_ops = *root.ops;
    g_gate_ops.read = gated_read;
    g_gate_ops.write = gated_write;
    g_gate_ops.writes = gated_writes;
    root.ops = &g_gate_ops;
    g_gate_open = true;
    KT_CHECK_INT_EQ(mfs_format(&root, 32u), MFS_OK);
    KT_CHECK_INT_EQ(k_blockdev_register(&root), 0);
    KT_CHECK_INT_EQ(mfs_vfs_register_type(), 0);
    KT_CHECK_INT_EQ(vfs_mount_type("/", "mfs", "logro"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mount_root_writes("/", "logro"), KERR_ROFS);
    KT_CHECK(vfs_mount_is_read_only(0));
    quiet_port();

    KT_CHECK_INT_EQ(k_logfile_start("read-only boot\n", 15u, true), 0);
    KT_CHECK(k_logfile_in_ram());
    k_console_write("ram console\n", 12);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);

    k_logfile_snapshot(&snapshot);
    KT_CHECK(snapshot.boot != NULL);
    KT_CHECK(strstr(snapshot.boot, "read-only boot\n") != NULL);
    KT_CHECK(strstr(snapshot.boot, "the root is read-only") != NULL);
    KT_CHECK(strstr(snapshot.boot, "are in RAM") != NULL);
    KT_CHECK(snapshot.run != NULL);
    KT_CHECK(strstr(snapshot.run, "ram console\n") != NULL);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_BOOT_PATH), KERR_NOENT);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PATH), KERR_NOENT);

    /* RAM is a store too: it has the same limit and the same two generations. */
    KT_CHECK(snapshot.run_size > 0 && snapshot.run_size < K_LOGFILE_LIMIT);
    fill_run(K_LOGFILE_LIMIT - snapshot.run_size);
    k_logfile_snapshot(&snapshot);
    KT_CHECK_INT_EQ((int)snapshot.run_previous_size, (int)K_LOGFILE_LIMIT);
    KT_CHECK_INT_EQ((int)snapshot.run_size, (int)sizeof("log: rotated at 65536 bytes\n") - 1);
    k_console_write("after ram rotation\n", 19);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    k_logfile_snapshot(&snapshot);
    KT_CHECK(strstr(snapshot.run, "after ram rotation\n") != NULL);
    KT_CHECK(snapshot.run_size < K_LOGFILE_LIMIT);

    k_logfile_stop();
    (void)vfs_umount("/");
    (void)mfs_unmount();
    KT_CHECK_INT_EQ(k_blockdev_unregister(&root), 0);
    mfs_ram_free(&memory);
    port_host_set_sink(NULL);
}

static void test_board_ram(void)
{
    struct k_logfile_snapshot snapshot;

    ktest_begin("logfile: a port without an append volume uses the same RAM log");

    setup_storage();
    KT_CHECK_INT_EQ(k_logfile_start("board boot\n", 11u, false), 0);
    KT_CHECK(k_logfile_in_ram());
    k_logfile_snapshot(&snapshot);
    KT_CHECK(snapshot.boot != NULL);
    KT_CHECK(strstr(snapshot.boot, "board boot\n") != NULL);
    KT_CHECK(strstr(snapshot.boot, "there is no append volume") != NULL);
    KT_CHECK(strstr(snapshot.boot, "are in RAM") != NULL);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_BOOT_PATH), KERR_NOENT);

    teardown_storage();
}

static void test_console_buffer(void)
{
    int before;
    int after;

    ktest_begin("logfile: the console fills memory and the VFS waits for a batch");

    setup_storage();
    KT_CHECK_INT_EQ(k_logfile_start("boot\n", 5u, true), 0);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    before = file_size(K_LOGFILE_RUN_PATH);

    k_console_write("buffered", 8);
    KT_CHECK_INT_EQ((int)k_logfile_pending(), 8);
    KT_CHECK_INT_EQ(file_size(K_LOGFILE_RUN_PATH), before);

    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    after = file_size(K_LOGFILE_RUN_PATH);
    KT_CHECK_INT_EQ(after, before + 8);
    KT_CHECK_INT_EQ((int)k_logfile_pending(), 0);

    teardown_storage();
}

static void test_poweroff_close(void)
{
    ktest_begin("logfile: poweroff drains the last batch and refuses later bytes");

    setup_storage();
    KT_CHECK_INT_EQ(k_logfile_start("boot\n", 5u, true), 0);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    k_console_write("last power line\n", 16);

    KT_CHECK_INT_EQ(k_logfile_stop_for_poweroff(), 0);
    KT_CHECK_INT_EQ((int)k_logfile_pending(), 0);

    /* The volume is still mounted here, but the log behaves as it must after
     * the close: a late power/sync message cannot reopen it or reach the VFS. */
    k_console_write("too late\n", 9);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);
    KT_CHECK(read_text(K_LOGFILE_RUN_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK(strstr(g_read, "last power line\n") != NULL);
    KT_CHECK(strstr(g_read, "too late\n") == NULL);

    teardown_storage();
}

static void test_password_not_logged(void)
{
    static const char secret[] = "password-7f31c9";

    ktest_begin("logfile: a secret input mode keeps a password out of run.log");

    setup_storage();
    KT_CHECK_INT_EQ(k_logfile_start("boot\n", 5u, true), 0);
    k_console_write("password: ", 10);
    k_console_set_input_not_logged(true);
    k_console_write(secret, sizeof(secret) - 1u);
    k_console_set_input_not_logged(false);
    k_console_write("\nvisible answer\n", sizeof("\nvisible answer\n") - 1u);
    KT_CHECK_INT_EQ(k_logfile_flush(), 0);

    KT_CHECK(read_text(K_LOGFILE_RUN_PATH, g_read, sizeof(g_read)) > 0);
    KT_CHECK(strstr(g_read, "password: ") != NULL);
    KT_CHECK(strstr(g_read, "visible answer\n") != NULL);
    KT_CHECK(strstr(g_read, secret) == NULL);
    KT_CHECK(!k_console_input_not_logged());

    teardown_storage();
}

void test_logfile(void)
{
    test_boot_generations();
    test_run_rotation();
    test_read_only_ram();
    test_board_ram();
    test_console_buffer();
    test_poweroff_close();
    test_password_not_logged();
}
