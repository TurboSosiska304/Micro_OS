#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "devfs/devfs.h"
#include "kernel/config.h"
#include "kernel/errno.h"
#include "kernel/version.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

static int g_argc;
static char g_args[KSHELL_MAX_ARGS][32];
static int g_result;

static int test_command(int argc, char **argv)
{
    int i;

    g_argc = argc;
    for (i = 0; i < argc && i < KSHELL_MAX_ARGS; i++) {
        strncpy(g_args[i], argv[i], sizeof(g_args[i]) - 1);
        g_args[i][sizeof(g_args[i]) - 1] = '\0';
    }

    return g_result;
}

static void test_execute(void)
{
    char buf[2048];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: a command is executed with its arguments");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);
    g_argc = 0;
    g_result = 42;

    KT_CHECK_INT_EQ(kshell_register("testcmd", "a command for the unit tests", test_command), 0);
    KT_CHECK_INT_EQ(kshell_execute("testcmd one two"), 42);
    KT_CHECK_INT_EQ(g_argc, 3);
    KT_CHECK_STR_EQ(g_args[0], "testcmd");
    KT_CHECK_STR_EQ(g_args[1], "one");
    KT_CHECK_STR_EQ(g_args[2], "two");

    /* Extra spaces must not produce empty arguments. */
    KT_CHECK_INT_EQ(kshell_execute("  testcmd   spaced  "), 42);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "spaced");

    /* An empty line does nothing. */
    KT_CHECK_INT_EQ(kshell_execute("   "), KSHELL_OK);

    /*
     * `-h` and `--help` are answered by the shell and not by the handler:
     * nothing is done, and the status is a success, because the help that
     * was asked for was given.
     */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("testcmd --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(g_argc, 0);

    /* The same text for `help <command>`, and a name that is no command. */
    KT_CHECK_INT_EQ(kshell_execute("help testcmd"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("help nosuch"), KSHELL_ERROR);

    /* `help` answers -h like every command, and one name at most. */
    KT_CHECK_INT_EQ(kshell_execute("help -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("help a b"), KSHELL_USAGE);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* A command that was registered with one line promises its name and
     * that line, and says so instead of inventing a usage of its own. */
    KT_CHECK(strstr(buf, "usage: testcmd [arguments]") != NULL);
    KT_CHECK(strstr(buf, "a command for the unit tests") != NULL);
    KT_CHECK(strstr(buf, "help: nosuch: no such command") != NULL);
    KT_CHECK(strstr(buf, "usage: help [command]") != NULL);
}

/*
 * The lexer of the line: a word may hold spaces, a quote may be escaped, and
 * an operator inside the quotes is a character of the argument. An empty
 * argument is a word of its own, and a quote that is not closed is a refusal
 * of the whole line - never a guess at where it ends.
 */
static void test_quotes(void)
{
    char buf[1024];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: the words of a line may be quoted");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);
    KT_CHECK_INT_EQ(kshell_register("testcmd", "a command for the unit tests", test_command), 0);
    g_result = 0;

    /* Double quotes group the spaces and keep the word one argument. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd a \"b c\" d"), 0);
    KT_CHECK_INT_EQ(g_argc, 4);
    KT_CHECK_STR_EQ(g_args[1], "a");
    KT_CHECK_STR_EQ(g_args[2], "b c");
    KT_CHECK_STR_EQ(g_args[3], "d");

    /* Single quotes are literal: nothing inside them is special. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd 'a b'"), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a b");

    /* Inside double quotes a backslash escapes the quote and itself only. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"a\\\"b\""), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a\"b");

    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"a\\\\b\""), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a\\b");

    /* The other backslash of a double quote is a character, as in POSIX. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"a\\nb\""), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a\\nb");

    /* Outside the quotes a backslash takes the character after it. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd a\\ b"), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a b");

    /* Two quotes with nothing between them are an empty argument, which is
     * not the same as no argument at all. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"\""), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "");

    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd ''"), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "");

    /* The operators are ordinary characters inside the quotes. */
    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"|\""), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "|");

    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd 'a | b > c ; d'"), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "a | b > c ; d");

    g_argc = 0;
    KT_CHECK_INT_EQ(kshell_execute("testcmd \\|"), 0);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "|");

    /* A quote that is not closed refuses the whole line and runs nothing. */
    g_argc = -1;
    KT_CHECK_INT_EQ(kshell_execute("testcmd 'abc"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(g_argc, -1);
    KT_CHECK_INT_EQ(kshell_execute("testcmd \"abc"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(g_argc, -1);
    KT_CHECK_INT_EQ(kshell_execute("testcmd abc\\"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(g_argc, -1);

    /* The empty argument reaches a command of the shell: `echo ""` is a
     * line of its own, and a quoted run of spaces keeps both of them. */
    KT_CHECK_INT_EQ(kshell_execute("echo \"\""), 0);
    KT_CHECK_INT_EQ(kshell_execute("echo \"a  b\""), 0);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    KT_CHECK(strstr(buf, "unterminated quote") != NULL);
    KT_CHECK(strstr(buf, "\n\na  b\n") != NULL);
}

static void test_unknown(void)
{
    ktest_begin("kshell: an unknown command is reported");

    port_host_set_sink(tmpfile());

    KT_CHECK(kshell_execute("nosuchcommand") != KSHELL_OK);

    port_host_set_sink(NULL);
}

static void test_registry(void)
{
    ktest_begin("kshell: the commands can be registered and removed");

    port_host_set_sink(tmpfile());

    KT_CHECK(kshell_command_count() > 0);
    KT_CHECK_INT_EQ(kshell_register("testcmd", "a command for the unit tests", test_command), 0);
    KT_CHECK_INT_EQ(kshell_unregister("testcmd"), KSHELL_OK);
    KT_CHECK(kshell_execute("testcmd") != KSHELL_OK);

    /*
     * The table of the commands has a limited number of places, and a
     * command that does not fit is lost: the board lost "mount" and "df"
     * that way, without a word. The one that registers the commands says
     * when one of them was refused, and the test says how many commands
     * the board really has.
     */
    KT_CHECK_INT_EQ(kshell_register_files(), 0);

    /*
     * The board has more commands than the tests can see here: the
     * drivers bring their own ("date", "i2c", "wifi", "display", "mkfs")
     * and those are only on the board. The twenty seven commands of the
     * shell itself have to be here, all of them. A build without the exec
     * path has one command fewer (ADR 0010).
     */
#if KERNEL_EXEC
    KT_CHECK(kshell_command_count() >= 27);
#else
    KT_CHECK(kshell_command_count() >= 26);
#endif

    port_host_set_sink(NULL);
}

/*
 * The report of `free` is a fact about the memory of this build: a machine
 * with one memory has no PSRAM line, and the line is not a zero that reads
 * like a memory that is there (ADR 0010).
 */
static void test_kill(void)
{
    char buf[512];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: kill names a signal and reports a pid that is not there");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);

    /* No pid, a signal outside the table, and a pid no task has. */
    KT_CHECK_INT_EQ(kshell_execute("kill"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("kill -NOPE 1"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("kill 4242"), KSHELL_ERROR);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    /* The sink is closed by the port when it is replaced. */
    port_host_set_sink(NULL);

    KT_CHECK(strstr(buf, "usage: kill [-signal] <pid>") != NULL);
    KT_CHECK(strstr(buf, "no such signal") != NULL);
    KT_CHECK(strstr(buf, "4242: no such process") != NULL);
}

static void test_free(void)
{
    char buf[1024];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: free reports the memory of this machine");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);
    KT_CHECK_INT_EQ(kshell_execute("free"), KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    KT_CHECK(strstr(buf, "heap:") != NULL);
#if KERNEL_PSRAM
    KT_CHECK(strstr(buf, "psram:") != NULL);
#else
    KT_CHECK(strstr(buf, "psram") == NULL);
#endif
}

/*
 * The clock of the shell and the clock of /proc are one style: seconds and
 * ticks, with a word that matches the number. Milliseconds never leak into a
 * line that a person reads.
 */
static void test_uptime(void)
{
    char buf[512];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: uptime prints seconds and ticks in one style");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);
    KT_CHECK_INT_EQ(kshell_execute("uptime"), KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    KT_CHECK(strstr(buf, "up ") != NULL);
    KT_CHECK(strstr(buf, " s, ") != NULL);
    KT_CHECK(strstr(buf, "tick") != NULL);
    KT_CHECK(strstr(buf, "task") != NULL);
    /* The old line spelled the units "(s)", and a person read it as a typo. */
    KT_CHECK(strstr(buf, "(s)") == NULL);
}

static void test_memtest(void)
{
    char buf[4096];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: the memory test measures the memory");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);

    /* The command has to run on every platform, even without a PSRAM. */
    KT_CHECK_INT_EQ(kshell_execute("memtest"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("memtest 32"), KSHELL_OK);

    /* A size of zero is not a valid argument. */
    KT_CHECK(kshell_execute("memtest 0") != KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The answer has a block, the three operations and a verdict; a speed
     * that the clock cannot resolve says so instead of inventing a number. */
    KT_CHECK(strstr(buf, " KB block: fill ") != NULL);
    KT_CHECK(strstr(buf, ", sum ") != NULL);
    KT_CHECK(strstr(buf, ", copy ") != NULL);
    KT_CHECK(strstr(buf, "data ok") != NULL);
    KT_CHECK(strstr(buf, "BROKEN") == NULL);
    KT_CHECK(strstr(buf, "KB/s") != NULL || strstr(buf, "too fast to measure") != NULL);
}

/*
 * The report of `mfetch` is written to the sink of the port: the test
 * reads it back from there. The sink is closed by the port when it is
 * replaced, so the reading is done while it is still the current one.
 */
static void test_mfetch(void)
{
    char buf[4096];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: mfetch prints the logo and the report");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);

    KT_CHECK_INT_EQ(kshell_execute("mfetch"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("mfetch color"), KSHELL_OK);

    /* The report has a shape of its own: an unknown argument is refused. */
    KT_CHECK(kshell_execute("mfetch nosuchmode") != KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The system and the board are named in the plain report ... */
    KT_CHECK(strstr(buf, kernel_name()) != NULL);
    KT_CHECK(strstr(buf, "Board") != NULL);
    KT_CHECK(strstr(buf, "command(s)") != NULL);

    /* ... the rows a person looks for ... */
    KT_CHECK(strstr(buf, "Build") != NULL);
    KT_CHECK(strstr(buf, "Volume") != NULL);
    KT_CHECK(strstr(buf, "Modules") != NULL);
    KT_CHECK(strstr(buf, "Heap") != NULL);

    /* ... and the colored logo brings the escapes of the terminal. */
    KT_CHECK(strstr(buf, "MM MM") != NULL);
    KT_CHECK(strstr(buf, "\033[") != NULL);
}

#define LS_TEST_BLOCKS 128

static struct mfs_ram_device g_ls_volume;
static struct k_blockdev g_ls_device;

static void ls_setup(void)
{
    if (mfs_ram_init(&g_ls_volume, LS_TEST_BLOCKS) != 0) {
        return;
    }

    mfs_ram_device(&g_ls_volume, &g_ls_device);
    mfs_format(&g_ls_device, 32);
    mfs_mount(&g_ls_device);
    mfs_vfs_register("/");
}

static void ls_teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_ls_volume);
}

/* The number of lines in the part of `buf` that one command wrote. */
static int range_lines(const char *buf, long start, long end)
{
    int lines = 0;
    long at;

    for (at = start; at < end && buf[at] != '\0'; at++) {
        if (buf[at] == '\n') {
            lines++;
        }
    }

    return lines;
}

/* How often `needle` is in the part of `buf` that one command wrote. */
static int range_count(const char *buf, long start, long end, const char *needle)
{
    const char *at = buf + start;
    int count = 0;

    while ((at = strstr(at, needle)) != NULL && (long)(at - buf) < end) {
        count++;
        at += strlen(needle);
    }

    return count;
}

/* Is `needle` in the part of `buf` that one command wrote? The sink is one
 * file for the whole test, so the offsets keep the answers apart. */
static bool range_has(const char *buf, long start, long end, const char *needle)
{
    const char *at;

    if (start < 0 || end < start) {
        return false;
    }

    at = strstr(buf + start, needle);
    return at != NULL && (long)(at - buf) < end;
}

/*
 * `ls --color` is the same listing with the SGR sequences of a terminal
 * around a name: blue for a directory and green for a file with an execute
 * bit. `never` and `auto` are the plain names - a shell of this system has no
 * terminal to ask about, so `auto` is off - and `always` is the same as the
 * flag alone.
 */
static void test_ls_color(void)
{
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    char buf[2048];
    size_t used;
    FILE *sink;
    long color_start;
    long color_end;
    long long_start;
    long long_end;
    long never_start;
    long never_end;
    long auto_start;
    long auto_end;

    ktest_begin("kshell: ls --color writes the colours of a terminal");

    ls_setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc/init.d"), 0);
    KT_CHECK_INT_EQ(vfs_open("/etc/hello", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, "hi", 2), 2);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/etc/hello", 0755), 0);

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        ls_teardown();
        return;
    }

    port_host_set_sink(sink);

    color_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls --color /etc"), KSHELL_OK);
    color_end = ftell(sink);

    long_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls -l --color /etc"), KSHELL_OK);
    long_end = ftell(sink);

    never_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls --color=never /etc"), KSHELL_OK);
    never_end = ftell(sink);

    auto_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls --color=auto /etc"), KSHELL_OK);
    auto_end = ftell(sink);

    /* A value the flag does not have is refused before the listing. */
    KT_CHECK_INT_EQ(kshell_execute("ls --color=sometimes /etc"), KSHELL_USAGE);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The executable is green and the directory blue, and every name ends
     * with the reset of the rendition. */
    KT_CHECK(range_has(buf, color_start, color_end, "\033[32mhello\033[0m\n"));
    KT_CHECK(range_has(buf, color_start, color_end, "\033[34minit.d/\033[0m\n"));

    /* The long format colours the name and not the line of the mode. */
    KT_CHECK(range_has(buf, long_start, long_end, "\033[32mhello\033[0m\n"));
    KT_CHECK(range_has(buf, long_start, long_end, "-rwxr-xr-x"));

    /* never and auto are the names themselves. */
    KT_CHECK(range_has(buf, never_start, never_end, "hello\n"));
    KT_CHECK(!range_has(buf, never_start, never_end, "\033["));
    KT_CHECK(range_has(buf, auto_start, auto_end, "hello\n"));
    KT_CHECK(!range_has(buf, auto_start, auto_end, "\033["));

    /* The refusal names the flag and prints the usage line. */
    KT_CHECK(strstr(buf, "ls: --color=sometimes") != NULL);

    ls_teardown();
}

/*
 * `ls` lists the names of a directory and `tree` draws the levels below it.
 * The same volume serves both, so the test reads the two answers of the
 * commands apart: `ls` has no branch and no path line, `tree` has both and
 * ends with the count of what it drew.
 */
static void test_ls(void)
{
    struct vfs_file file;
    struct vfs_stat info;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    char buf[8192];
    size_t used;
    FILE *sink;
    long etc_start;
    long etc_end;
    long long_start;
    long long_end;
    long all_start;
    long all_end;
    long long_all_start;
    long long_all_end;
    long tree_start;
    long tree_end;
    long tree_d_start;
    long tree_d_end;
    long many_start;
    long many_end;
    long wide_start;
    long wide_end;
    long dotdot_start;
    long dotdot_end;
    long pwd1_start;
    long pwd1_end;
    long pwd2_start;
    long pwd2_end;
    long pwd3_start;
    long pwd3_end;
    k_size_t i;

    ktest_begin("kshell: ls lists a directory, tree draws it");

    ls_setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc/init.d"), 0);
    KT_CHECK_INT_EQ(vfs_open("/etc/hello", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, "hi", 2), 2);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(devfs_register("/dev"), 0);

    /* A tree of three levels for `tree` and `rm -r`, and a file of two
     * lines. */
    KT_CHECK_INT_EQ(vfs_mkdir("/tree"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/tree/a"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/tree/a/deep"), 0);
    KT_CHECK_INT_EQ(
        vfs_open("/tree/a/deep/f", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_open("/tree/z", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_open("/lines", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, "one\ntwo\n", 8), 8);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* A directory wider than the old tables of `ls` (64) and `tree` (16):
     * the listing and the walk are streaming now, and neither one may stop
     * at a boundary or lose the names past it. */
    KT_CHECK_INT_EQ(vfs_mkdir("/many"), 0);
    for (i = 0; i < 70; i++) {
        char name[VFS_PATH_MAX];
        char text[8];

        snprintf(name, sizeof(name), "/many/f%02u", (unsigned)i);
        snprintf(text, sizeof(text), "%u\n", (unsigned)i);
        KT_CHECK_INT_EQ(vfs_open(name, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
        KT_CHECK_INT_EQ(vfs_write(&file, text, strlen(text)), (int)strlen(text));
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_mkdir("/wide"), 0);
    for (i = 0; i < 20; i++) {
        char name[VFS_PATH_MAX];

        snprintf(name, sizeof(name), "/wide/w%02u", (unsigned)i);
        KT_CHECK_INT_EQ(vfs_open(name, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        vfs_umount("/dev");
        ls_teardown();
        return;
    }

    port_host_set_sink(sink);

    /* `ls /etc` is the first answer of the sink, so its first line is the
     * first byte of the file: no path line can hide in front of it. */
    etc_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls /etc"), KSHELL_OK);
    etc_end = ftell(sink);

    long_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls -l /etc"), KSHELL_OK);
    long_end = ftell(sink);

    all_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls -a /etc"), KSHELL_OK);
    all_end = ftell(sink);

    long_all_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls -la /etc"), KSHELL_OK);
    long_all_end = ftell(sink);

    /* A file, short and long. */
    KT_CHECK_INT_EQ(kshell_execute("ls /etc/hello"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("ls -l /etc/hello"), KSHELL_OK);
    /* A device of devfs keeps no mode: stat says so instead of a 0000. */
    KT_CHECK_INT_EQ(kshell_execute("stat /dev/tty0"), KSHELL_OK);

    tree_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("tree /tree"), KSHELL_OK);
    tree_end = ftell(sink);

    tree_d_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("tree -d /tree"), KSHELL_OK);
    tree_d_end = ftell(sink);

    /* A directory of 70 names is listed whole, not cut at the old table. */
    many_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls /many"), KSHELL_OK);
    many_end = ftell(sink);

    /* A directory of 20 files is walked whole and counted whole. */
    wide_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("tree /wide"), KSHELL_OK);
    wide_end = ftell(sink);

    /* `.`, `..`, repeated slashes and a trailing slash are one path. */
    KT_CHECK_INT_EQ(kshell_execute("cd /etc"), KSHELL_OK);
    dotdot_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls .."), KSHELL_OK);
    dotdot_end = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("cd ."), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cd //etc//init.d///"), KSHELL_OK);
    pwd1_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("pwd"), KSHELL_OK);
    pwd1_end = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("ls /etc/../etc/./"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cd ../.."), KSHELL_OK);
    pwd2_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("pwd"), KSHELL_OK);
    pwd2_end = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("cd .."), KSHELL_OK);
    pwd3_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("pwd"), KSHELL_OK);
    pwd3_end = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("cd /"), KSHELL_OK);

    /* `-a` is accepted and changes nothing: there are no hidden names. */
    KT_CHECK_INT_EQ(kshell_execute("tree -a /tree"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("tree /nope"), KSHELL_ERROR);

    /* The help of the two commands, and an option they do not have. */
    KT_CHECK_INT_EQ(kshell_execute("ls -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("ls --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("ls -z /"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("tree -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("tree --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("tree -z /"), KSHELL_USAGE);

    /* cat -n numbers the lines; without the flag the bytes are as they are. */
    KT_CHECK_INT_EQ(kshell_execute("cat -n /lines"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cat /lines"), KSHELL_OK);

    /* A name that begins with a dash is a file once `--` says so. */
    KT_CHECK_INT_EQ(kshell_execute("write -dash hi"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cat -dash"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cat -- -dash"), KSHELL_OK);

    /* A directory that is already there is not a file that exists. */
    KT_CHECK_INT_EQ(kshell_execute("mkdir /tree/a"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("mkdir /etc/hello"), KSHELL_ERROR);

    /* The sizes of the volume for a person; here -h is the flag of df and
     * not the help, as in df of Linux. */
    KT_CHECK_INT_EQ(kshell_execute("df -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("df --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("df -z"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("df /nope"), KSHELL_ERROR);

    /* A directory needs -r, and -r takes the whole tree down. */
    KT_CHECK_INT_EQ(kshell_execute("rm /tree"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("rm -r /tree"), KSHELL_OK);
    KT_CHECK(vfs_stat("/tree", &info) != 0);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* ls: the names, one per line in the order of the filesystem; a
     * directory keeps its slash and no branch or path line is there. */
    KT_CHECK(range_has(buf, etc_start, etc_end, "hello\n"));
    KT_CHECK(range_has(buf, etc_start, etc_end, "init.d/\n"));
    KT_CHECK(!range_has(buf, etc_start, etc_end, "`--"));

    /* The long format: the nine bits, the owner, the size, the date and the
     * name. */
    KT_CHECK(range_has(buf, long_start, long_end, "-rw-r--r--"));
    KT_CHECK(range_has(buf, long_start, long_end, "drwxr-xr-x"));
    KT_CHECK(range_has(buf, long_start, long_end, "root"));
    KT_CHECK(range_has(buf, long_start, long_end, "hello"));
    KT_CHECK(range_has(buf, long_start, long_end, "init.d/"));
    /* A record without a moment says so; one with a moment carries the
     * colon of HH:MM. */
    KT_CHECK(range_has(buf, long_start, long_end, " - ") ||
             range_has(buf, long_start, long_end, ":"));

    /* -a puts `.` and `..` first, in both formats. */
    KT_CHECK(range_has(buf, all_start, all_end, ".\n..\n"));
    KT_CHECK(range_has(buf, long_all_start, long_all_end, " .\n"));
    KT_CHECK(range_has(buf, long_all_start, long_all_end, " ..\n"));

    /* A file is its name as it was given, or one line of the long format. */
    KT_CHECK(strstr(buf, "/etc/hello") != NULL);

    /* tree: the branches, the deep name and the counts. The path itself is
     * not a directory of the answer. */
    KT_CHECK(range_has(buf, tree_start, tree_end, "|-- "));
    KT_CHECK(range_has(buf, tree_start, tree_end, "`-- "));
    KT_CHECK(range_has(buf, tree_start, tree_end, "deep"));
    KT_CHECK(range_has(buf, tree_start, tree_end, "2 directories, 2 files"));

    /* tree -d: the directories only, and the files are still counted: the
     * summary is the content of the tree, not the branches that were drawn. */
    KT_CHECK(range_has(buf, tree_d_start, tree_d_end, "deep/"));
    KT_CHECK(!range_has(buf, tree_d_start, tree_d_end, "z"));
    KT_CHECK(!range_has(buf, tree_d_start, tree_d_end, "f\n"));
    KT_CHECK(range_has(buf, tree_d_start, tree_d_end, "2 directories, 2 files"));

    /* The old tables cut the listing at 64 and the tree at 16. */
    KT_CHECK_INT_EQ(range_lines(buf, many_start, many_end), 70);
    KT_CHECK(range_has(buf, many_start, many_end, "f00\n"));
    KT_CHECK(range_has(buf, many_start, many_end, "f69\n"));
    KT_CHECK(range_has(buf, wide_start, wide_end, "0 directories, 20 files"));
    KT_CHECK(range_has(buf, wide_start, wide_end, "w00"));
    KT_CHECK(range_has(buf, wide_start, wide_end, "w19"));
    KT_CHECK_INT_EQ(range_count(buf, wide_start, wide_end, "|-- ") +
                        range_count(buf, wide_start, wide_end, "`-- "),
                    20);

    /* `.` and `..` are the directory and its parent, here as anywhere. */
    KT_CHECK(range_has(buf, dotdot_start, dotdot_end, "etc/"));
    KT_CHECK(range_has(buf, pwd1_start, pwd1_end, "/etc/init.d\n"));
    KT_CHECK(range_has(buf, pwd2_start, pwd2_end, "/\n"));
    KT_CHECK(range_has(buf, pwd3_start, pwd3_end, "/\n"));

    /* The errors and the help of the two commands. */
    KT_CHECK(strstr(buf, "tree: /nope: no such file or directory") != NULL);
    KT_CHECK(strstr(buf, "usage: ls [-la] [--color[=always|never|auto]] [path]") != NULL);
    KT_CHECK(strstr(buf, "usage: tree [-ad] [path]") != NULL);
    KT_CHECK(strstr(buf, "ls: unknown option: -z") != NULL);
    KT_CHECK(strstr(buf, "tree: unknown option: -z") != NULL);

    /* stat of a device: no bits to show, and an honest "not set". */
    KT_CHECK(strstr(buf, "type: device") != NULL);
    KT_CHECK(strstr(buf, "mode: (not set)") != NULL);

    /* cat: the lines are numbered with the flag, and not without it. */
    KT_CHECK(strstr(buf, "     1\tone") != NULL);
    KT_CHECK(strstr(buf, "     2\ttwo") != NULL);
    KT_CHECK(strstr(buf, "one\ntwo\n") != NULL);

    /* A dash is an option until `--` says otherwise. */
    KT_CHECK(strstr(buf, "cat: unknown option: -d") != NULL);
    KT_CHECK(strstr(buf, "hi\n") != NULL);
    /* The count of the bytes has a word that matches it. */
    KT_CHECK(strstr(buf, "3 bytes written to /-dash") != NULL);
    KT_CHECK(strstr(buf, "byte(s)") == NULL);

    /* A directory says that it is one; a file in the way says "file
     * exists", and neither one claims a file. */
    KT_CHECK(strstr(buf, "mkdir: /tree/a: is a directory") != NULL);
    KT_CHECK(strstr(buf, "mkdir: /etc/hello: file exists") != NULL);

    /* df -h: the sizes in KB or MB, then the fields of the volume. */
    KT_CHECK(strstr(buf, "total,") != NULL);
    KT_CHECK(strstr(buf, "df: unknown option: -z") != NULL);
    KT_CHECK(strstr(buf, "df: /nope: no such file or directory") != NULL);

    /* rm: the sentence about a directory, then the tree is gone. */
    KT_CHECK(strstr(buf, "is a directory, use -r") != NULL);

    vfs_umount("/dev");
    ls_teardown();
}

/*
 * The commands of the mode, the owner, the path and the programs speak to
 * the ABI and not to the filesystem: the test checks the words of a refusal,
 * that a mode and an owner that were accepted are the ones the next stat
 * reports, and that a name without a slash is looked up in PATH.
 */
static void test_attr(void)
{
    char buf[8192];
    size_t used;
    FILE *sink;
    struct vfs_stat stat;
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];

    ktest_begin("kshell: chmod, chown, stat, path and exec");

    ls_setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/bin"), 0);
    KT_CHECK_INT_EQ(vfs_open("/bin/hello", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, "hi\n", 3), 3);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        ls_teardown();
        return;
    }

    port_host_set_sink(sink);

    /* A command with the wrong number of arguments says how it is used. */
    KT_CHECK_INT_EQ(kshell_execute("chmod 0644"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("chmod 0644 /bin extra"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("chown 0"), KSHELL_USAGE);
#if KERNEL_EXEC
    KT_CHECK_INT_EQ(kshell_execute("exec"), KSHELL_USAGE);
#endif
    KT_CHECK_INT_EQ(kshell_execute("stat"), KSHELL_USAGE);
    /* The check of the volume takes the words of the type it checks. */
    KT_CHECK_INT_EQ(kshell_execute("fsck fat"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck fat -y"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("fsck -y extra"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("help fsck"), KSHELL_OK);

    /* The volume is whole, so the repair has nothing to do and says so. */
    KT_CHECK_INT_EQ(kshell_execute("fsck -y"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("fsck --repair"), KSHELL_OK);

    /*
     * The help of a command is the text of the table: the usage line, the
     * one line of the list and the notes of the flags. The handler does not
     * run, so a chmod with no arguments is a request for the text and not a
     * failure of the command.
     */
    KT_CHECK_INT_EQ(kshell_execute("chmod -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("chmod --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("stat --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("help stat"), KSHELL_OK);
    /* A name that is no command is an error with the name in it. */
    KT_CHECK_INT_EQ(kshell_execute("help nosuch"), KSHELL_ERROR);

    /* A mode that is not octal is refused before the call ... */
    KT_CHECK_INT_EQ(kshell_execute("chmod 0999 /bin"), KSHELL_ERROR);
    /* ... and so is a uid outside the fifteen bits. */
    KT_CHECK_INT_EQ(kshell_execute("chown 99999 /bin"), KSHELL_ERROR);

    /* The calls pass what was asked, and the next stat sees it. */
    KT_CHECK_INT_EQ(kshell_execute("chmod 0711 /bin"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_stat("/bin", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0711);
    KT_CHECK_INT_EQ(kshell_execute("chown 7 /bin"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_stat("/bin", &stat), 0);
    KT_CHECK_INT_EQ(stat.owner, 7);

    /* A zero is a mode the record cannot hold, and the reason is printed. */
    KT_CHECK_INT_EQ(kshell_execute("chmod 000 /bin"), KSHELL_ERROR);

    /* The program PATH will find: a file with an execute bit and an owner. */
    KT_CHECK_INT_EQ(kshell_execute("chmod 0711 /bin/hello"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("chown 7 /bin/hello"), KSHELL_OK);

    /* PATH begins at the default, and `path` prints what it holds. */
    KT_CHECK_INT_EQ(kshell_execute("path"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("path /bin"), KSHELL_OK);

#if KERNEL_EXEC
    /*
     * A name without a slash is looked up in PATH: the answer names the
     * file that was found, not the name that was typed.
     */
    KT_CHECK_INT_EQ(kshell_execute("exec hello"), KSHELL_ERROR);

    /* A name that no entry holds reports the search and the directories. */
    KT_CHECK_INT_EQ(kshell_execute("path /nosuchdir"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("exec hello"), KSHELL_ERROR);
#else
    KT_CHECK_INT_EQ(kshell_execute("path /nosuchdir"), KSHELL_OK);
#endif

    /* An empty value is not a list of directories: it is refused, and the
     * value that was there stays. */
    KT_CHECK_INT_EQ(kshell_execute("path \"\""), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("path /bin:/usr/bin"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("path"), KSHELL_OK);

    /* stat: the line of ls, then the fields a chmod is checked with. */
    KT_CHECK_INT_EQ(kshell_execute("stat /bin/hello"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("stat /bin"), KSHELL_OK);

    /* The answer of the ABI reaches the person: a path that is not there. */
    KT_CHECK_INT_EQ(kshell_execute("chmod 0600 /nosuch"), KSHELL_ERROR);
#if KERNEL_EXEC
    KT_CHECK_INT_EQ(kshell_execute("exec /nosuch"), KSHELL_ERROR);
#endif
    KT_CHECK_INT_EQ(kshell_execute("stat /nosuch"), KSHELL_ERROR);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    KT_CHECK(strstr(buf, "usage: chmod <mode> <path>") != NULL);
    KT_CHECK(strstr(buf, "usage: chown <uid> <path>") != NULL);
#if KERNEL_EXEC
    KT_CHECK(strstr(buf, "usage: exec <program>") != NULL);
#endif
    KT_CHECK(strstr(buf, "usage: stat <path>") != NULL);
    KT_CHECK(strstr(buf, "usage: fsck") != NULL);
    KT_CHECK(strstr(buf, "fsck [-y]") != NULL);
#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
    KT_CHECK(strstr(buf, "-y") != NULL);
#endif
    KT_CHECK(strstr(buf, "the volume is consistent") != NULL);
    KT_CHECK(strstr(buf, "not an octal mode") != NULL);
    KT_CHECK(strstr(buf, "a uid is 0..32767") != NULL);
    KT_CHECK(strstr(buf, "filesystem keeps") != NULL);
    KT_CHECK(strstr(buf, "no such file") != NULL);
    KT_CHECK(strstr(buf, "an empty value is not a list of directories") != NULL);

    /* The notes of the help come from the table, once per request. */
    KT_CHECK(strstr(buf, "The mode is the nine bits in octal") != NULL);
    KT_CHECK(strstr(buf, "the type, the mode, the owner and the size") != NULL);
    KT_CHECK(strstr(buf, "help: nosuch: no such command") != NULL);
    /* The long notes of fsck pass the 192 bytes of the print buffer: a
     * sentence from the end is there, whole. The sentence is the one of the
     * build with FAT32; the minimal build words the same promise over a line
     * break, so the check is asked only where the sentence exists. */
#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
    KT_CHECK(strstr(buf, "a record no name reaches is freed") != NULL);
#endif

    /* The file: nine bits of 0711, the owner 7 and three bytes of text. */
    KT_CHECK(strstr(buf, "-rwx--x--x 7 3 /bin/hello") != NULL);
    KT_CHECK(strstr(buf, "mode: 0711") != NULL);
    KT_CHECK(strstr(buf, "owner: 7") != NULL);
    KT_CHECK(strstr(buf, "size: 3") != NULL);

    /* The directory is a `d`, and the same mode and owner as the file. */
    KT_CHECK(strstr(buf, "drwx--x--x 7 ") != NULL);
    KT_CHECK(strstr(buf, "type: directory") != NULL);

    /* PATH looked the name up, and the search that failed said where. */
#if KERNEL_EXEC
    KT_CHECK(strstr(buf, "exec: /bin/hello: not a MOX1 image of this system") != NULL);
    KT_CHECK(strstr(buf, "exec: hello: not found in /nosuchdir") != NULL);
#endif
    KT_CHECK(strstr(buf, "/bin:/usr/bin\n") != NULL);

    ls_teardown();
}

/*
 * `help` is the list of what the machine can run: the commands of the shell
 * and the programs of PATH, which the shell never registered and used to hide
 * from the person. A name that is both is answered with both parts, and the
 * listing stops at a screenful with `...` rather than running the console out
 * of lines.
 */
static void test_help_programs(void)
{
#if !KERNEL_EXEC
    /*
     * A build without the exec path has no program to run, and `help` says
     * so by not listing one: a file of PATH that no port can enter would be
     * a promise the machine cannot keep. The name is no command either.
     */
    ktest_begin("kshell: help names no program without the exec path");
    KT_CHECK_INT_EQ(kshell_execute("help hello"), KSHELL_ERROR);
#else
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    char buf[8192];
    size_t used;
    FILE *sink;
    long list_start;
    long list_end;
    long both_start;
    long both_end;
    long one_start;
    long one_end;
    k_size_t i;

    ktest_begin("kshell: help names the programs of PATH");

    ls_setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/bin"), 0);

    /* The two shapes: a name that is a command as well (`ls`), a name that
     * is a program only (`hello`), and a file that no port would run. */
    KT_CHECK_INT_EQ(vfs_open("/bin/ls", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/bin/ls", 0755), 0);
    KT_CHECK_INT_EQ(vfs_open("/bin/hello", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/bin/hello", 0755), 0);
    KT_CHECK_INT_EQ(vfs_open("/bin/notes", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data),
                    0);
    KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* More programs than the listing prints: the names past the limit are
     * the `...` of the answer. */
    for (i = 0; i < 26; i++) {
        char path[VFS_PATH_MAX];

        snprintf(path, sizeof(path), "/bin/p%02u", (unsigned)i);
        KT_CHECK_INT_EQ(vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data), 0);
        KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
        KT_CHECK_INT_EQ(vfs_chmod(path, 0755), 0);
    }

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        ls_teardown();
        return;
    }

    port_host_set_sink(sink);
    KT_CHECK_INT_EQ(kshell_execute("path /bin"), KSHELL_OK);

    list_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("help"), KSHELL_OK);
    list_end = ftell(sink);

    both_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("help ls"), KSHELL_OK);
    both_end = ftell(sink);

    one_start = ftell(sink);
    KT_CHECK_INT_EQ(kshell_execute("help hello"), KSHELL_OK);
    one_end = ftell(sink);

    KT_CHECK_INT_EQ(kshell_execute("help nosuch"), KSHELL_ERROR);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The list of the commands ends with the programs of PATH. */
    KT_CHECK(range_has(buf, list_start, list_end, "programs in PATH:"));
    KT_CHECK(range_has(buf, list_start, list_end, " ls"));
    KT_CHECK(range_has(buf, list_start, list_end, " hello"));
    KT_CHECK(!range_has(buf, list_start, list_end, "notes"));
    KT_CHECK(range_has(buf, list_start, list_end, "..."));

    /* A name of both parts names the file and then the command of the shell. */
    KT_CHECK(range_has(buf, both_start, both_end, "ls is a program: /bin/ls"));
    KT_CHECK(range_has(buf, both_start, both_end, "the builtin:"));
    KT_CHECK(range_has(buf, both_start, both_end, "usage: ls "));

    /* A program with no command of its own is one line and nothing else. */
    KT_CHECK(
        range_has(buf, one_start, one_end, "hello is a program at /bin/hello (type hello --help)"));
    KT_CHECK(!range_has(buf, one_start, one_end, "usage: hello"));

    KT_CHECK(strstr(buf, "help: nosuch: no such command") != NULL);

    KT_CHECK_INT_EQ(kshell_execute("path /bin:/usr/bin"), KSHELL_OK);
    ls_teardown();
#endif
}

/* The type that the mount commands of this test speak: devfs has objects to
 * read and no volume, so two mounts of it cost nothing. */
static int kshell_devfs_mount(const char *point, const char *device)
{
    K_UNUSED(device);

    return devfs_register(point);
}

static int kshell_write_text(const char *path, const char *text)
{
    struct vfs_file file;
    static k_u8_t data[VFS_FILE_DATA_SIZE];
    int rc;

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, data);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_write(&file, text, strlen(text));
    if (rc < 0) {
        vfs_close(&file);
        return rc;
    }

    return vfs_close(&file);
}

/*
 * The commands of the mount table: the list without a word, a volume by its
 * device, point and type, -a over /etc/fstab, and the unmount that gives
 * the point back. A refusal has to say which point and why.
 */
static void test_mount(void)
{
    char buf[8192];
    size_t used;
    FILE *sink;
    struct vfs_stat stat;

    ktest_begin("kshell: mount, umount and the table of the boot");

    ls_setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/mnt"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_register_type("dev", kshell_devfs_mount), 0);

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        vfs_unregister_type("dev");
        ls_teardown();
        return;
    }

    port_host_set_sink(sink);

    /* Without a word the mounted filesystems are listed. */
    KT_CHECK_INT_EQ(kshell_execute("mount"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /mnt/dev dev"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/dev/tty0", &stat), 0);

    /* The same point is busy, and the same line of the table says so. */
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /mnt/dev dev"), KSHELL_ERROR);

    /* A type the image does not have, a wrong number of words, an option
     * that is not there. */
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /mnt/x nosuch"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /mnt/x"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("mount -z"), KSHELL_USAGE);

    /* The help of the table, for -h and for --help. */
    KT_CHECK_INT_EQ(kshell_execute("mount -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("mount --help"), KSHELL_OK);

    /*
     * -a mounts the two volumes of /etc/fstab; the third line is broken and
     * the command is still a success. The second run mounts nothing and the
     * points that are busy do not make it a failure either.
     */
    KT_CHECK_INT_EQ(kshell_write_text(VFS_FSTAB_PATH, "fake0 /mnt/one dev\n"
                                                      "fake0 /mnt/two dev\n"
                                                      "broken-line\n"),
                    0);
    KT_CHECK_INT_EQ(kshell_execute("mount -a"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_mount_count(), 4);
    KT_CHECK_INT_EQ(kshell_execute("mount -a"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_mount_count(), 4);

    /* The unmount, and the point that is empty again. */
    KT_CHECK_INT_EQ(kshell_execute("umount /mnt/dev"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/dev/tty0", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(kshell_execute("umount /mnt/dev"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("umount"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("umount -h"), KSHELL_OK);

    KT_CHECK_INT_EQ(kshell_execute("umount /mnt/one"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("umount /mnt/two"), KSHELL_OK);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    /* The root and the filesystems of the kernel stay. */
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /proc dev"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("mount fake0 /dev dev"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("umount /"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("umount /proc"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("umount /dev"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(vfs_umount("/proc"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/dev"), 0);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The list, the mount and the line it printed. */
    KT_CHECK(strstr(buf, "/\n") != NULL);
    KT_CHECK(strstr(buf, "mount: fake0 is mounted at /mnt/dev") != NULL);

    /* Every refusal names the point and the reason in words. */
    KT_CHECK(strstr(buf, "mount: /mnt/dev: device or resource busy") != NULL);
    KT_CHECK(strstr(buf, "mount: there is no filesystem type 'nosuch'") != NULL);
    KT_CHECK(strstr(buf, "umount: /mnt/dev: no such file or directory") != NULL);
    KT_CHECK(strstr(buf, "umount: /mnt/dev is out of the tree") != NULL);
    KT_CHECK(strstr(buf, "umount: / is the root of the tree; it stays") != NULL);
    KT_CHECK(strstr(buf, "umount: /proc is mounted by the kernel and stays") != NULL);
    KT_CHECK(strstr(buf, "umount: /dev is mounted by the kernel and stays") != NULL);
    KT_CHECK(strstr(buf, "mount: unknown option: -z") != NULL);

    /* The two tables of the command: the words and the flags. */
    KT_CHECK(strstr(buf, "usage: mount [<device> <point> <type>] | mount -a") != NULL);
    KT_CHECK(strstr(buf, "usage: umount <point>") != NULL);
    KT_CHECK(strstr(buf, "mount: 2 filesystem(s) mounted from /etc/fstab") != NULL);
    KT_CHECK(strstr(buf, "mount: 0 filesystem(s) mounted from /etc/fstab") != NULL);
    KT_CHECK(strstr(buf, "mount every volume of /etc/fstab") != NULL);

    KT_CHECK_INT_EQ(vfs_unregister_type("dev"), 0);
    ls_teardown();
}

void test_kshell(void)
{ /* The built in commands are registered by the shell itself. */
    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);

    test_execute();
    test_quotes();
    test_unknown();
    test_registry();
    test_kill();
    test_free();
    test_uptime();
    test_memtest();
    test_mfetch();
    test_ls();
    test_ls_color();
    test_attr();
    test_help_programs();
    test_mount();
}
