/*
 * Micro OS — the pipes and the redirections of the shell.
 *
 * The kernel side of pipe() and dup2() has its own test (ADR 0014); this one
 * is the shell on top of it: the words of the grammar, the task per stage,
 * the ends put on 0 and 1, the files of a redirect and the sentences of a
 * line that cannot run. The shell is a task of the kernel, so the test runs
 * it in the test task and reads the answers back from the sink of the port
 * and from the volume.
 *
 * A pipeline here is made of the commands of the shell (`cat`, `ls`, `echo`),
 * because the host cannot run a program at all (k_port_exec_enter answers
 * -1, docs/debts.md). The same runner starts a program of the machine with
 * execve() on x86-64, which is what `make x86-smoke` reads.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

#define PIPE_TEST_BLOCKS 128

static struct mfs_ram_device g_pipe_volume;
static struct k_blockdev g_pipe_device;
static k_u8_t g_pipe_file_data[VFS_FILE_DATA_SIZE];

static int volume_write(const char *path, const char *text)
{
    struct vfs_file file;
    k_size_t len = strlen(text);
    int rc;

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, g_pipe_file_data);
    if (rc != 0) {
        return rc;
    }

    if (vfs_write(&file, text, len) != (int)len) {
        vfs_close(&file);
        return -1;
    }

    return vfs_close(&file);
}

static int volume_read(const char *path, char *out, k_size_t size)
{
    struct vfs_file file;
    int got;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, g_pipe_file_data);
    if (rc != 0) {
        return rc;
    }

    got = vfs_read(&file, out, size - 1);
    vfs_close(&file);

    if (got < 0) {
        return got;
    }

    out[got] = '\0';

    return got;
}

static void pipe_setup(void)
{
    if (mfs_ram_init(&g_pipe_volume, PIPE_TEST_BLOCKS) != 0) {
        return;
    }

    mfs_ram_device(&g_pipe_volume, &g_pipe_device);
    mfs_format(&g_pipe_device, 32);
    mfs_mount(&g_pipe_device);
    mfs_vfs_register("/");
}

static void pipe_teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_pipe_volume);
}

/*
 * No descriptor of the shell may be left by a line, and no slot of the task
 * table either: a stage that was never reaped is a zombie, and the count of
 * the live slots would stay above the one taken before the lines.
 */
static void check_no_leaks(k_size_t tasks_before)
{
    struct task *self = task_self();
    k_size_t i;

    for (i = 0; i < TASK_MAX_FDS; i++) {
        KT_CHECK(self->files[i] == NULL);
    }

    KT_CHECK(task_count() <= tasks_before);
}

static FILE *sink_open(void)
{
    FILE *sink = tmpfile();

    KT_CHECK(sink != NULL);

    if (sink != NULL) {
        port_host_set_sink(sink);
    }

    return sink;
}

static void sink_close(FILE *sink, char *buf, k_size_t size)
{
    size_t used;

    rewind(sink);
    used = fread(buf, 1, size - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);
}

static void test_parse_and_run(void)
{
    char buf[8192];
    FILE *sink;
    k_size_t tasks_before;

    ktest_begin("kshell: a pipe runs its stages and the last one writes the answer");

    pipe_setup();
    tasks_before = task_count();
    KT_CHECK_INT_EQ(volume_write("/in", "one\ntwo\n"), 0);

    sink = sink_open();
    if (sink == NULL) {
        pipe_teardown();
        return;
    }

    /* Two builtins: the bytes of the first reach the second through a pipe. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in | cat"), KSHELL_OK);
    /* Three stages: two pipes of the system and three tasks. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in | cat | cat"), KSHELL_OK);
    /* Ten stages: the pipes are made one boundary at a time as the stages
     * are forked, so the shell's table never holds more than the two ends of
     * one boundary - the shape the line of the smoke test uses. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in | cat | cat | cat | cat | cat | cat | cat | cat | cat"),
                    KSHELL_OK);
    /* A producer of the shell and a consumer of the shell. */
    KT_CHECK_INT_EQ(kshell_execute("ls / | cat"), KSHELL_OK);

    sink_close(sink, buf, sizeof(buf));

    KT_CHECK(strstr(buf, "one\ntwo\n") != NULL);
    /* The listing of the volume came through the pipe, not from the shell. */
    KT_CHECK(strstr(buf, "in") != NULL);

    check_no_leaks(tasks_before);

    pipe_teardown();
}

static void test_redirections(void)
{
    char buf[8192];
    char out[256];
    FILE *sink;
    k_size_t tasks_before;

    ktest_begin("kshell: the redirections write and read the files of the line");

    pipe_setup();
    tasks_before = task_count();
    KT_CHECK_INT_EQ(volume_write("/in", "first\n"), 0);
    KT_CHECK_INT_EQ(volume_write("/-dash", "dash\n"), 0);

    sink = sink_open();
    if (sink == NULL) {
        pipe_teardown();
        return;
    }

    /* `>` makes the file, `>>` keeps what is there. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in > /out"), KSHELL_OK);
    KT_CHECK_INT_EQ(volume_read("/out", out, sizeof(out)), 6);
    KT_CHECK_STR_EQ(out, "first\n");

    KT_CHECK_INT_EQ(kshell_execute("cat /in >> /out"), KSHELL_OK);
    KT_CHECK_INT_EQ(volume_read("/out", out, sizeof(out)), 12);
    KT_CHECK_STR_EQ(out, "first\nfirst\n");

    /* `<` is the input of the stage; a pipe and a redirect may meet on one
     * line, and the redirect of the output wins over the pipe. */
    KT_CHECK_INT_EQ(kshell_execute("cat < /in"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cat /in | cat > /out2"), KSHELL_OK);
    KT_CHECK_INT_EQ(volume_read("/out2", out, sizeof(out)), 6);
    KT_CHECK_STR_EQ(out, "first\n");

    /* `2>` makes the file even when the stage writes nothing there. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in > /out3 2> /err"), KSHELL_OK);
    KT_CHECK_INT_EQ(volume_read("/err", out, sizeof(out)), 0);

    /* The splitter keeps `--`: a name that begins with a dash is an
     * argument of the stage, not an option of the shell. */
    KT_CHECK_INT_EQ(kshell_execute("cat -- -dash | cat"), KSHELL_OK);

    sink_close(sink, buf, sizeof(buf));

    KT_CHECK(strstr(buf, "first\n") != NULL);
    KT_CHECK(strstr(buf, "dash\n") != NULL);

    check_no_leaks(tasks_before);

    pipe_teardown();
}

static void test_errors(void)
{
    char buf[8192];
    FILE *sink;
    k_size_t tasks_before;

    ktest_begin("kshell: a line the parser cannot run is a sentence, not a hang");

    pipe_setup();
    tasks_before = task_count();
    KT_CHECK_INT_EQ(volume_write("/in", "alive\n"), 0);

    sink = sink_open();
    if (sink == NULL) {
        pipe_teardown();
        return;
    }

    /* A pipe needs a command on both sides. */
    KT_CHECK_INT_EQ(kshell_execute("| cat"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cat |"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cat /in | | cat"), KSHELL_USAGE);

    /* The words the shell does not parse yet are refused and named. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in && cat"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cat /in ; cat"), KSHELL_USAGE);

    /* A redirect without a name. */
    KT_CHECK_INT_EQ(kshell_execute("ls / >"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cat < | cat"), KSHELL_USAGE);

    /* `cd` belongs to the shell task, not to a stage of its own. */
    KT_CHECK_INT_EQ(kshell_execute("cd / | cat"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("cd / > /x"), KSHELL_USAGE);

    /* A file that cannot be opened is the failure of the stage, and the
     * shell is alive after it. */
    KT_CHECK(kshell_execute("cat < /no/such/file") != KSHELL_OK);
    KT_CHECK(kshell_execute("ls / > /no/such/dir/out") != KSHELL_OK);

    /* A name that is no command is said, and the last stage still decides
     * the status of the line. */
    KT_CHECK_INT_EQ(kshell_execute("nosuch | cat"), KSHELL_OK);
    KT_CHECK(kshell_execute("cat /in | nosuch") != KSHELL_OK);

    /* The shell still runs a good line after all of them. */
    KT_CHECK_INT_EQ(kshell_execute("cat /in | cat"), KSHELL_OK);

    sink_close(sink, buf, sizeof(buf));

    KT_CHECK(strstr(buf, "a pipe needs a command on both sides") != NULL);
    KT_CHECK(strstr(buf, "'&&' is not supported") != NULL);
    KT_CHECK(strstr(buf, "needs a file name") != NULL);
    KT_CHECK(strstr(buf, "cannot run in a pipeline") != NULL);
    KT_CHECK(strstr(buf, "no such file") != NULL);
    KT_CHECK(strstr(buf, "nosuch: command not found") != NULL);
    KT_CHECK(strstr(buf, "alive\n") != NULL);

    check_no_leaks(tasks_before);

    pipe_teardown();
}

void test_kshell_pipe(void)
{
    test_parse_and_run();
    test_redirections();
    test_errors();
}
