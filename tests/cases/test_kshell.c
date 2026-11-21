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

static void test_memtest(void)
{
    ktest_begin("kshell: the memory test measures the memory");

    port_host_set_sink(tmpfile());

    /* The command has to run on every platform, even without a PSRAM. */
    KT_CHECK_INT_EQ(kshell_execute("memtest"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("memtest 32"), KSHELL_OK);

    /* A size of zero is not a valid argument. */
    KT_CHECK(kshell_execute("memtest 0") != KSHELL_OK);

    port_host_set_sink(NULL);
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

    /* The sizes of the volume for a person; here -h is the flag of df and
     * not the help, as in df of Linux. */
    KT_CHECK_INT_EQ(kshell_execute("df -h"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("df --help"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("df -z"), KSHELL_USAGE);

    /* A directory needs -r, and -r takes the whole tree down. */
    KT_CHECK_INT_EQ(kshell_execute("rm /tree"), KSHELL_ERROR);
    KT_CHECK_INT_EQ(kshell_execute("rm -r /tree"), KSHELL_OK);
    KT_CHECK(vfs_stat("/tree", &info) != 0);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* ls: the names, sorted, one per line; a directory keeps its slash and
     * no branch or path line is there. */
    KT_CHECK(strncmp(buf, "hello\n", 6) == 0);
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

    /* tree -d: the directories only, and no file is counted. */
    KT_CHECK(range_has(buf, tree_d_start, tree_d_end, "deep/"));
    KT_CHECK(!range_has(buf, tree_d_start, tree_d_end, "z"));
    KT_CHECK(!range_has(buf, tree_d_start, tree_d_end, "f\n"));
    KT_CHECK(range_has(buf, tree_d_start, tree_d_end, "2 directories, 0 files"));

    /* The errors and the help of the two commands. */
    KT_CHECK(strstr(buf, "tree: /nope: no such file or directory") != NULL);
    KT_CHECK(strstr(buf, "usage: ls [-la] [path]") != NULL);
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

    /* df -h: the sizes in KB or MB, then the fields of the volume. */
    KT_CHECK(strstr(buf, "total,") != NULL);
    KT_CHECK(strstr(buf, "df: unknown option: -z") != NULL);

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
    KT_CHECK_INT_EQ(kshell_execute("help fsck"), KSHELL_OK);

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

    /* The empty argument brings the default back. */
    KT_CHECK_INT_EQ(kshell_execute("path \"\""), KSHELL_OK);
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
#if defined(MICRO_OS_FS_FAT32) && MICRO_OS_FS_FAT32
    KT_CHECK(strstr(buf, "-y") != NULL);
#endif
    KT_CHECK(strstr(buf, "not an octal mode") != NULL);
    KT_CHECK(strstr(buf, "a uid is 0..32767") != NULL);
    KT_CHECK(strstr(buf, "filesystem keeps") != NULL);
    KT_CHECK(strstr(buf, "no such file") != NULL);

    /* The notes of the help come from the table, once per request. */
    KT_CHECK(strstr(buf, "The mode is the nine bits in octal") != NULL);
    KT_CHECK(strstr(buf, "the type, the mode, the owner and the size") != NULL);
    KT_CHECK(strstr(buf, "help: nosuch: no such command") != NULL);

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
    test_unknown();
    test_registry();
    test_kill();
    test_free();
    test_memtest();
    test_mfetch();
    test_ls();
    test_attr();
    test_mount();
}
