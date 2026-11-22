/*
 * Micro OS — the systems of parameters of /etc (ADR 0021).
 *
 * The parser, the rewrite and the calls of the console are pure functions of
 * a text, and the tests drive them without a screen: the applier below is a
 * machine that reads the keys the way the driver of the panel does. The last
 * case is the one the design is for: a system file survives a format because
 * it is one value of the store of the port.
 */
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
#include "settings/settings.h"

#define TEST_BLOCKS 128

/* A store of its own for the run: the simulator keeps /tmp/microos-kv. */
#define KV_DIR_TEMPLATE "/tmp/microos-settings-kv-XXXXXX"

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

/* What the applier of the tests saw, the way the driver of the panel reads
 * the file: the helpers of the settings and not the text. */
struct applied {
    int calls;
    int rotation;
    int invert;
    int brightness;
    int scale;
    int columns;
    int rows;
    bool dark;
};

static struct applied g_applied;

static void apply_test(const char *system, const struct settings_table *table)
{
    K_UNUSED(system);

    g_applied.calls++;
    g_applied.rotation = settings_number(table, "rotation", 0, 0, 270);
    g_applied.invert = settings_flag(table, "invert", true) ? 1 : 0;
    g_applied.brightness = settings_number(table, "brightness", 100, 0, 100);
    g_applied.scale = settings_number(table, "scale", 1, 1, 2);
    g_applied.columns = settings_number(table, "columns", 0, 0, 99);
    g_applied.rows = settings_number(table, "rows", 0, 0, 99);
    g_applied.dark = strcmp(settings_value(table, "theme", "dark"), "dark") == 0;
}

static const struct settings_system s_display_applier = {
    .name = "display",
    .apply = apply_test,
};

static const struct settings_system s_font_applier = {
    .name = "font",
    .apply = apply_test,
};

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
    memset(&g_applied, 0, sizeof(g_applied));

    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 32);
    mfs_mount(&g_device);
    mfs_vfs_register("/");

    /* The directory of the systems: a fresh volume has only its root. */
    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc"), 0);
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

static void test_parser(void)
{
    static const char *text = "# a comment, and the line below is empty\n"
                              "\n"
                              "rotation = 90\n"
                              "  invert=0  \n"
                              "unknown.key-1 = the value\n"
                              "rotation=180\n"
                              "a line without an equal sign\n"
                              "Bad=1\n"
                              "empty=\n"
                              "toolong=0123456789012345678901234567890123\n";
    struct settings_table table;

    ktest_begin("settings: the parser reads key=value, skips comments and lets the last line win");

    KT_CHECK_INT_EQ(settings_parse(text, strlen(text), &table), SETTINGS_OK);

    /* rotation (the second line wins), invert and the unknown key. */
    KT_CHECK_INT_EQ((int)table.count, 3);
    KT_CHECK_STR_EQ(settings_find(&table, "rotation"), "180");
    KT_CHECK_STR_EQ(settings_find(&table, "invert"), "0");
    KT_CHECK_STR_EQ(settings_find(&table, "unknown.key-1"), "the value");
    KT_CHECK(settings_find(&table, "Bad") == NULL);
    KT_CHECK(settings_find(&table, "empty") == NULL);

    /* The line without '=', the capital key, the empty value and the long
     * one are four lines the parser did not read. */
    KT_CHECK_INT_EQ((int)table.bad_lines, 4);
}

static void test_rewrite(void)
{
    static const char *text = "# the screen of this machine\n"
                              "rotation=0\n"
                              "gamma=2\n"
                              "\n"
                              "invert = 1\n";
    char next[SETTINGS_TEXT_MAX + 2];
    int length;

    ktest_begin("settings: a rewrite keeps the comment and the key it does not name");

    length = settings_rewrite(text, strlen(text), "rotation", "90", next, sizeof(next));
    KT_CHECK(length > 0);
    KT_CHECK_STR_EQ(next, "# the screen of this machine\nrotation=90\ngamma=2\n\ninvert = 1\n");

    /* The line is appended when the key is not in the file, and the rest of
     * the file is still there. */
    length = settings_rewrite(next, (k_size_t)length, "theme", "light", next, sizeof(next));
    KT_CHECK(length > 0);
    KT_CHECK(strstr(next, "# the screen of this machine") != NULL);
    KT_CHECK(strstr(next, "gamma=2") != NULL);
    KT_CHECK(strstr(next, "theme=light") != NULL);

    /* The file is read back through the parser: the rewrite left no line
     * that the parser refuses. */
    {
        struct settings_table table;

        KT_CHECK_INT_EQ(settings_parse(next, (k_size_t)length, &table), SETTINGS_OK);
        KT_CHECK_INT_EQ((int)table.bad_lines, 0);
        KT_CHECK_STR_EQ(settings_find(&table, "rotation"), "90");
        KT_CHECK_STR_EQ(settings_find(&table, "theme"), "light");
        KT_CHECK_STR_EQ(settings_find(&table, "gamma"), "2");
    }

    /* A buffer that cannot hold the file is a refusal and not a cut. */
    KT_CHECK_INT_EQ(settings_rewrite(text, strlen(text), "rotation", "90", next, 8),
                    SETTINGS_ETOOLONG);

    /* The defaults of the tree are one value of the store, so a file that
     * the boot writes always fits the text of a system; and every key of a
     * default file is a key of its system, so a key is applied or named. */
    {
        k_size_t i;

        for (i = 0; i < settings_known_count(); i++) {
            const struct settings_known *known = settings_known_at(i);
            struct settings_table table;
            k_size_t j;

            KT_CHECK((int)strlen(known->defaults) <= SETTINGS_TEXT_MAX);
            KT_CHECK_INT_EQ(settings_parse(known->defaults, strlen(known->defaults), &table),
                            SETTINGS_OK);
            KT_CHECK_INT_EQ((int)table.bad_lines, 0);
            for (j = 0; j < table.count; j++) {
                KT_CHECK(settings_key_known(known->name, table.pairs[j].key));
            }
        }
    }
}

static void test_defaults(void)
{
    char value[SETTINGS_VALUE_MAX];

    ktest_begin("settings: the defaults of the tree are the behaviour before the files");

    setup();

    KT_CHECK_INT_EQ(settings_defaults("/etc"), 3);
    KT_CHECK_INT_EQ(settings_defaults("/etc"), 0);

    KT_CHECK_INT_EQ(settings_get("display", "rotation", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "0");
    KT_CHECK_INT_EQ(settings_get("display", "invert", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "1");
    KT_CHECK_INT_EQ(settings_get("display", "brightness", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "100");
    KT_CHECK_INT_EQ(settings_get("font", "scale", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "1");
    KT_CHECK_INT_EQ(settings_get("console", "theme", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "dark");

    /* A key of no system, a system of no tree and a small buffer. */
    KT_CHECK_INT_EQ(settings_get("display", "gamma", NULL, value, sizeof(value)), SETTINGS_EKEY);
    KT_CHECK_INT_EQ(settings_get("display", "gamma", "2", value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "2");
    KT_CHECK_INT_EQ(settings_get("time", "zone", NULL, value, sizeof(value)), SETTINGS_ENOSYS);
    KT_CHECK_INT_EQ(settings_get("display", "rotation", NULL, value, 1), SETTINGS_ENOTEXT);
    KT_CHECK_INT_EQ(settings_get("display", "Rotation", NULL, value, sizeof(value)),
                    SETTINGS_EBADKEY);

    teardown();
}

static void test_load(void)
{
    ktest_begin("settings: the file is read and the applier of the machine is called");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_display_applier), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_register(&s_font_applier), SETTINGS_OK);

    write_text("/etc/display", "# the screen\nrotation=90\ninvert=0\nbrightness=40\ntheme=light\n");

    KT_CHECK_INT_EQ(settings_load("display"), SETTINGS_OK);
    KT_CHECK_INT_EQ(g_applied.calls, 1);
    KT_CHECK_INT_EQ(g_applied.rotation, 90);
    KT_CHECK_INT_EQ(g_applied.invert, 0);
    KT_CHECK_INT_EQ(g_applied.brightness, 40);
    KT_CHECK(g_applied.dark == false);

    /* A file that is not there is the defaults of the system: the machine
     * still applies one state, and the first frame is not a surprise. */
    g_applied.calls = 0;
    KT_CHECK_INT_EQ(settings_load("font"), SETTINGS_OK);
    KT_CHECK_INT_EQ(g_applied.calls, 1);
    KT_CHECK_INT_EQ(g_applied.scale, 1);
    KT_CHECK_INT_EQ(g_applied.columns, 0);

    /* A machine without an applier keeps the file and says so; the load is
     * not an error and the value is still read. */
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_font_applier), SETTINGS_OK);
    g_applied.calls = 0;
    KT_CHECK_INT_EQ(settings_load("display"), SETTINGS_OK);
    KT_CHECK_INT_EQ(g_applied.calls, 0); /* the applier of display is not here */

    teardown();
}

static void test_bad_value(void)
{
    char value[SETTINGS_VALUE_MAX];

    ktest_begin("settings: a value the system cannot parse is the default and a warning");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_display_applier), SETTINGS_OK);

    write_text("/etc/display", "rotation=abc\ninvert=7\nbrightness=400\n");
    KT_CHECK_INT_EQ(settings_load("display"), SETTINGS_OK);

    /* The applier got the defaults of its keys, not the broken text. */
    KT_CHECK_INT_EQ(g_applied.rotation, 0);
    KT_CHECK_INT_EQ(g_applied.invert, 1);
    KT_CHECK_INT_EQ(g_applied.brightness, 100);

    /* The file is not rewritten by the load: the text of a person stays as
     * they left it, so they can see what they typed. */
    KT_CHECK_INT_EQ(settings_get("display", "rotation", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "abc");

    teardown();
}

static void test_unknown_key(void)
{
    char value[SETTINGS_VALUE_MAX];
    char text[SETTINGS_TEXT_MAX + 1];

    ktest_begin("settings: a key the system does not know is kept and read back");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_display_applier), SETTINGS_OK);

    write_text("/etc/display", "rotation=90\ngamma=2\n");

    KT_CHECK_INT_EQ(settings_load("display"), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_get("display", "gamma", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "2");

    /* The load does not rewrite the file, so the key of a newer release is
     * still there for the release that knows it. */
    read_text("/etc/display", text, sizeof(text));
    KT_CHECK_STR_EQ(text, "rotation=90\ngamma=2\n");

    teardown();
}

static void test_too_long(void)
{
    char text[SETTINGS_TEXT_MAX + 40];
    char value[SETTINGS_VALUE_MAX];
    k_size_t i;

    ktest_begin("settings: a file over the limit is refused and the defaults are used");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_font_applier), SETTINGS_OK);

    for (i = 0; i + 1 < sizeof(text); i++) {
        text[i] = 'x';
    }
    text[0] = '#';
    text[sizeof(text) - 1] = '\0';
    write_text("/etc/font", text);

    KT_CHECK_INT_EQ(settings_load("font"), SETTINGS_ETOOLONG);

    /* The machine applied the defaults, not half of a cut file. */
    KT_CHECK_INT_EQ(g_applied.calls, 1);
    KT_CHECK_INT_EQ(g_applied.scale, 1);

    KT_CHECK_INT_EQ(settings_get("font", "scale", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "1");

    teardown();
}

static void test_set(void)
{
    char value[SETTINGS_VALUE_MAX];
    char text[SETTINGS_TEXT_MAX + 1];
    char too_long[SETTINGS_VALUE_MAX + 8];

    ktest_begin("settings: set writes one line, keeps the rest and applies at once");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_display_applier), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_register(&s_font_applier), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_defaults("/etc"), 3);

    KT_CHECK_INT_EQ(settings_set("display", "rotation", "270"), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_get("display", "rotation", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "270");
    KT_CHECK_INT_EQ(g_applied.calls, 1);
    KT_CHECK_INT_EQ(g_applied.rotation, 270);

    /* The line was replaced in place: the comment and the other keys of the
     * defaults are still in the file. */
    read_text("/etc/display", text, sizeof(text));
    KT_CHECK((int)strlen(text) <= SETTINGS_TEXT_MAX);
    KT_CHECK(strstr(text, "# the screen") != NULL);
    KT_CHECK(strstr(text, "rotation=270") != NULL);
    KT_CHECK(strstr(text, "brightness=100") != NULL);

    /* A typo and a value that is too long do not reach the file. */
    KT_CHECK_INT_EQ(settings_set("display", "rotaton", "90"), SETTINGS_EKEY);
    KT_CHECK_INT_EQ(settings_set("display", "rotation", ""), SETTINGS_EBADVALUE);
    KT_CHECK_INT_EQ(settings_set("display", "rotation", " 90"), SETTINGS_EBADVALUE);
    KT_CHECK_INT_EQ(settings_set("display", "Rotation", "90"), SETTINGS_EBADKEY);
    KT_CHECK_INT_EQ(settings_set("time", "zone", "UTC"), SETTINGS_ENOSYS);
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    KT_CHECK_INT_EQ(settings_set("display", "rotation", too_long), SETTINGS_EBADVALUE);

    read_text("/etc/display", text, sizeof(text));
    KT_CHECK(strstr(text, "rotation=270") != NULL);

    /* A file of a person is the base of the rewrite: the key that is not in
     * the defaults is added and the comment of the person is kept. */
    write_text("/etc/font", "# my font\nscale=1\n# the end\n");
    KT_CHECK_INT_EQ(settings_set("font", "scale", "2"), SETTINGS_OK);
    read_text("/etc/font", text, sizeof(text));
    KT_CHECK_STR_EQ(text, "# my font\nscale=2\n# the end\n");

    /* Twelve keys are the most the parser holds: the thirteenth is refused
     * before the file is written, so the next boot does not refuse it. */
    write_text("/etc/display", "a=1\nb=1\nc=1\nd=1\ne=1\nf=1\ng=1\nh=1\ni=1\nj=1\nk=1\n");
    KT_CHECK_INT_EQ(settings_set("display", "rotation", "90"), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_set("display", "invert", "0"), SETTINGS_ETOOLONG);
    read_text("/etc/display", text, sizeof(text));
    KT_CHECK(strstr(text, "rotation=90") != NULL);
    KT_CHECK(strstr(text, "invert=0") == NULL);

    /* A machine without the applier writes the file and answers KEPT. The
     * file is given back its room first: the twelve keys above are full. */
    write_text("/etc/display", "# the screen\nrotation=0\n");
    settings_reset();
    KT_CHECK_INT_EQ(settings_set("display", "invert", "0"), SETTINGS_KEPT);

    teardown();
}

/*
 * The point of the store: the text of a system is one value, so a format
 * takes /etc away and the keys bring the settings back whole.
 */
static void test_store_roundtrip(void)
{
    char before[SETTINGS_TEXT_MAX + 1];
    char after[SETTINGS_TEXT_MAX + 1];
    char value[SETTINGS_VALUE_MAX];

    ktest_begin("settings: a system file survives a format through the store");

    setup();
    settings_reset();
    KT_CHECK_INT_EQ(settings_register(&s_display_applier), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_register(&s_font_applier), SETTINGS_OK);
    KT_CHECK_INT_EQ(settings_defaults("/etc"), 3);
    KT_CHECK_INT_EQ(settings_set("display", "rotation", "90"), SETTINGS_OK);
    read_text("/etc/display", before, sizeof(before));

    /* Three systems and not one more. */
    KT_CHECK_INT_EQ(config_persist("/etc"), 3);

    /* The format: the volume is gone, the store of the port is not. */
    teardown();
    config_use_store(NULL);
    config_store_use_platform();
    setup();

    KT_CHECK_INT_EQ(config_restore("/etc"), 3);
    read_text("/etc/display", after, sizeof(after));

    /* The store keeps a value without its last line ending, which is the
     * rule of /etc since the first release: the content of the file is
     * whole and the text before the final '\n' is the one that was saved. */
    if (strlen(before) > 0 && before[strlen(before) - 1] == '\n') {
        before[strlen(before) - 1] = '\0';
    }
    KT_CHECK_STR_EQ(after, before);
    KT_CHECK_INT_EQ(settings_get("display", "rotation", NULL, value, sizeof(value)), SETTINGS_OK);
    KT_CHECK_STR_EQ(value, "90");

    teardown();
}

void test_settings(void)
{
    char dir[] = KV_DIR_TEMPLATE;

    ktest_begin("settings: the store of the host is ready for the tests");

    /*
     * The real store of the host, in a directory of its own: a table in the
     * memory would not show a file that comes back after a format.
     */
    KT_CHECK(mkdtemp(dir) != NULL);
    KT_CHECK_INT_EQ(setenv("MICROOS_KV_DIR", dir, 1), 0);

    config_store_use_platform();

    test_parser();
    test_rewrite();
    test_defaults();
    test_load();
    test_bad_value();
    test_unknown_key();
    test_too_long();
    test_set();
    test_store_roundtrip();

    settings_reset();
    config_use_store(NULL);
    store_reset();
    KT_CHECK_INT_EQ(unsetenv("MICROOS_KV_DIR"), 0);
    KT_CHECK_INT_EQ(rmdir(dir), 0);
}
