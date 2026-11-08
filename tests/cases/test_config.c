#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "config/config.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#define TEST_BLOCKS 128
#define STORE_KEYS 8

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

/* The store of the tests: the same table the NVS gives the board. */
static char g_keys[STORE_KEYS][CONFIG_KEY_MAX];
static char g_values[STORE_KEYS][CONFIG_VALUE_MAX];
static k_size_t g_count;

static int store_load(const char *key, char *value, k_size_t size)
{
    k_size_t i;

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_keys[i], key) == 0) {
            strncpy(value, g_values[i], size - 1);
            value[size - 1] = '\0';
            return 0;
        }
    }

    return -2;
}

static int store_save(const char *key, const char *value)
{
    k_size_t i;

    if (strlen(key) >= CONFIG_KEY_MAX || strlen(value) >= CONFIG_VALUE_MAX) {
        return -1;
    }

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_keys[i], key) == 0) {
            strncpy(g_values[i], value, sizeof(g_values[i]) - 1);
            g_values[i][sizeof(g_values[i]) - 1] = '\0';
            return 0;
        }
    }

    if (g_count >= STORE_KEYS) {
        return -1;
    }

    strncpy(g_keys[g_count], key, sizeof(g_keys[g_count]) - 1);
    g_keys[g_count][sizeof(g_keys[g_count]) - 1] = '\0';
    strncpy(g_values[g_count], value, sizeof(g_values[g_count]) - 1);
    g_values[g_count][sizeof(g_values[g_count]) - 1] = '\0';
    g_count++;

    return 0;
}

static int store_next(k_size_t index, char *key, k_size_t size)
{
    if (index >= g_count) {
        return -2;
    }

    strncpy(key, g_keys[index], size - 1);
    key[size - 1] = '\0';

    return 0;
}

static const struct config_store g_store = {
    .load = store_load,
    .save = store_save,
    .next = store_next,
};

static void store_reset(void)
{
    memset(g_keys, 0, sizeof(g_keys));
    memset(g_values, 0, sizeof(g_values));
    g_count = 0;
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
    KT_CHECK_INT_EQ(store_save("ssid", "home"), 0);

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
    KT_CHECK_INT_EQ(store_load("clock", value, sizeof(value)), 0);

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
    KT_CHECK_INT_EQ(store_save("mode", "old"), 0);
    KT_CHECK_INT_EQ(store_save("port", "23"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    write_text("/etc/mode", "new");

    /* Only the missing key is restored: the edited file stays as it is. */
    KT_CHECK_INT_EQ(config_restore("/etc"), 1);
    read_text("/etc/mode", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "new");
    read_text("/etc/port", value, sizeof(value));
    KT_CHECK_STR_EQ(value, "23");

    KT_CHECK_INT_EQ(config_persist("/etc"), 2);
    KT_CHECK_INT_EQ(store_load("mode", value, sizeof(value)), 0);
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

    /* The NVS gives a key 15 characters, this name is longer. */
    write_text("/etc/waytoolongname123", "1");

    /*
     * Neither one is saved: the store of the board takes a key of 15
     * characters and a string of 127, and a silently cut value would be
     * worse than a key that is not there.
     */
    KT_CHECK_INT_EQ(config_persist("/etc"), 0);

    teardown();
}

void test_config(void)
{
    config_use_store(&g_store);

    test_restore();
    test_persist();
    test_the_file_wins();
    test_empty();
    test_bad_values();

    config_use_store(NULL);
}
