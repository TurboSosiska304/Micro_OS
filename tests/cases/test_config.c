#include "ktest.h"
#include "tests/tests.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config.h"
#include "kernel/blockdev.h"
#include "kernel/port/kv.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#define TEST_BLOCKS 128

/* A store of its own for the run: the simulator keeps /tmp/microos-kv. */
#define KV_DIR_TEMPLATE "/tmp/microos-kv-test-XXXXXX"

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static int store_put(const char *key, const char *value)
{
    int rc = k_port_kv_set(key, value);

    if (rc != 0) {
        return rc;
    }

    return k_port_kv_commit();
}

/*
 * Empty the store between the cases. The walk always starts at the index 0,
 * and a key that is gone shifts the rest down, so the same index is used
 * again.
 */
static void store_reset(void)
{
    char key[K_PORT_KV_KEY_MAX];
    k_size_t guard;

    for (guard = 0; guard < CONFIG_MAX_KEYS; guard++) {
        if (k_port_kv_next(0, key, sizeof(key)) != 0) {
            break;
        }
        if (k_port_kv_erase(key) != 0) {
            break;
        }
    }
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

static void read_text(const char *path, char *text, k_size_t size)
{
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    int n;

    text[0] = '\0';

    KT_CHECK_INT_EQ(vfs_open(path, VFS_O_RDONLY, &file, data), 0);
    n = vfs_read(&file, text, size - 1);
    KT_CHECK(n >= 0);
    if (n < 0) {
        n = 0;
    }
    text[n] = '\0';
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
}

static void test_restore(void)
{
    char value[CONFIG_VALUE_MAX];

    ktest_begin("config: the keys of the store come back as files of /etc");

    setup();
    store_reset();
    KT_CHECK_INT_EQ(store_put("ssid", "home"), 0);

    /* /etc does not exist on a fresh volume: it is made on the way. */
    KT_CHECK_INT_EQ(config_restore("/etc"), 1);
    read_text("/etc/ssid", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "home");

    teardown();
}

static void test_persist(void)
{
    char value[CONFIG_VALUE_MAX];

    ktest_begin("config: the files of /etc are saved into the store");

    setup();
    store_reset();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    write_text("/etc/clock", "12:00\n");

    KT_CHECK_INT_EQ(config_persist("/etc"), 1);
    KT_CHECK_INT_EQ(k_port_kv_get("clock", value, sizeof(value)), 5);

    /* A line ending of a person is not a part of the value. */
    KT_CHECK_STR_EQ(value, "12:00");

    teardown();
}

static void test_the_file_wins(void)
{
    char value[CONFIG_VALUE_MAX];

    ktest_begin("config: the file wins over the store, and the store fills the gaps");

    setup();
    store_reset();
    KT_CHECK_INT_EQ(store_put("mode", "old"), 0);
    KT_CHECK_INT_EQ(store_put("port", "23"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    write_text("/etc/mode", "new");

    /* Only the missing key is restored: the edited file stays as it is. */
    KT_CHECK_INT_EQ(config_restore("/etc"), 1);
    read_text("/etc/mode", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "new");
    read_text("/etc/port", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "23");

    KT_CHECK_INT_EQ(config_persist("/etc"), 2);
    KT_CHECK_INT_EQ(k_port_kv_get("mode", value, sizeof(value)), 3);
    KT_CHECK_STR_EQ(value, "new");

    teardown();
}

static void test_empty(void)
{
    ktest_begin("config: an empty store and an empty /etc are not an error");

    setup();
    store_reset();

    KT_CHECK_INT_EQ(config_sync("/etc"), 0);

    teardown();
}

static void test_bad_values(void)
{
    char too_long[CONFIG_VALUE_MAX + 32];

    ktest_begin("config: a name or a value that does not fit the store is reported");

    setup();
    store_reset();
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);

    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    write_text("/etc/too-long", too_long);

    /* The store gives a key 15 characters, this name is longer. */
    write_text("/etc/waytoolongname123", "1");

    /*
     * Neither one is saved: the store of the board takes a key of 15
     * characters and a string of 127, and a silently cut value would be
     * worse than a key that is not there.
     */
    KT_CHECK_INT_EQ(config_persist("/etc"), 0);

    teardown();
}

/*
 * The point of the store is that it outlives the volume: after a restart the
 * files of /etc are gone, and the keys of the store bring them back.
 */
static void test_restart(void)
{
    char value[CONFIG_VALUE_MAX];
    char key[K_PORT_KV_KEY_MAX];

    ktest_begin("config: the keys of the store survive a restart");

    setup();
    store_reset();
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    write_text("/etc/clock", "12:00\n");
    KT_CHECK_INT_EQ(config_persist("/etc"), 1);

    /* The key is really in the port's store, and it is the only one there. */
    KT_CHECK_INT_EQ(k_port_kv_next(0, key, sizeof(key)), 0);
    KT_CHECK_STR_EQ(key, "clock");
    KT_CHECK_INT_EQ(k_port_kv_next(1, key, sizeof(key)), -2);

    /* The restart: the volume with /etc is gone, the store is asked again. */
    teardown();
    config_use_store(NULL);
    config_store_use_platform();
    setup();

    KT_CHECK_INT_EQ(config_restore("/etc"), 1);
    read_text("/etc/clock", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "12:00");

    teardown();
}

void test_config(void)
{
    char dir[] = KV_DIR_TEMPLATE;

    ktest_begin("config: the store of the host is ready for the tests");

    /*
     * The tests use the real store of the host: a table in the memory would
     * not show a store that loses its keys between the boots. The directory
     * is unique, so the run does not touch the store of the simulator and a
     * leftover of another run cannot pass a test.
     */
    KT_CHECK(mkdtemp(dir) != NULL);
    KT_CHECK_INT_EQ(setenv("MICROOS_KV_DIR", dir, 1), 0);

    config_store_use_platform();

    test_restore();
    test_persist();
    test_the_file_wins();
    test_empty();
    test_bad_values();
    test_restart();

    config_use_store(NULL);
    store_reset();
    KT_CHECK_INT_EQ(unsetenv("MICROOS_KV_DIR"), 0);
    KT_CHECK_INT_EQ(rmdir(dir), 0);
}
