/*
 * Micro OS — the tests of the process part of the ABI.
 *
 * The properties again: the same data has to give the same result no
 * matter how the program forks, waits and reads.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "libc/unistd.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

#define CORPUS_MAX 8192

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;
static char g_text[CORPUS_MAX];
static k_size_t g_text_len;

/* What the child and the parent share. */
static char g_shared[CORPUS_MAX];
static volatile int g_child_done;
static volatile int g_fork_result;
static volatile int g_wait_result;
static volatile int g_wait_status = -1;
static volatile int g_parent_pid;
static volatile int g_child_pid;
static volatile int g_child_waits;

/* The result of the test, the parent writes it. */
static volatile int g_result;

static int load_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    k_size_t used = 0;

    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        k_size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }
        if (used + len + 2 >= sizeof(g_text)) {
            break;
        }

        memcpy(g_text + used, line, len);
        used += len;
        g_text[used++] = '\n';
        g_text[used] = '\0';
    }

    fclose(file);
    g_text_len = used;

    return (used > 0) ? 0 : -1;
}

static void setup(void)
{
    if (mfs_ram_init(&g_volume, 512) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
    ksys_init();

    memset(g_shared, 0, sizeof(g_shared));
    g_child_done = 0;
    g_fork_result = -1;
    g_wait_result = -1;
    g_wait_status = -1;
    g_parent_pid = -1;
    g_child_pid = -1;
    g_child_waits = 0;
    g_result = -1;
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/*
 * The program of the parent and of the child. The child knows it by the
 * flag of its task.
 */
static void parent_program(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    if (self != NULL && self->is_child) {
        /* The child: it writes the data and finishes with the code 7. */
        memcpy(g_shared, g_text, g_text_len);
        g_child_done = 1;
        task_exit(7);
        return;
    }

    /* The parent: it creates the child and waits for it. */
    g_parent_pid = (int)ksys_getpid();
    g_fork_result = ksys_fork();
    g_child_pid = g_fork_result;

    if (g_fork_result > 0) {
        g_wait_result = ksys_waitpid(g_fork_result, (int *)&g_wait_status, 0);
    }

    g_result = 1;
}

/*
 * The property: the child gets its own number, the parent waits for it and
 * gets the code it finished with, and the data of the child is the corpus.
 */
static void test_fork_wait(void)
{
    struct task *parent;
    int waited;

    ktest_begin("ksys: the child has its own number and the parent waits for it");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    /* The program runs in a task of its own: the fork copies the program. */
    parent = task_create("parent", parent_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(parent != NULL);

    for (waited = 0; waited < 3000 && g_result < 0; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK_INT_EQ(g_result, 1);
    KT_CHECK(g_fork_result > 0);
    KT_CHECK(g_fork_result != g_parent_pid);
    KT_CHECK_INT_EQ(g_wait_result, g_fork_result);
    KT_CHECK_INT_EQ(g_wait_status, 7);
    KT_CHECK_INT_EQ(g_child_done, 1);
    KT_CHECK_INT_EQ(memcmp(g_shared, g_text, g_text_len), 0);

    teardown();
}

/*
 * The property: the number of the child is always bigger than the number of
 * the parent, and the parent of the child is the parent.
 */
static void test_fork_numbers(void)
{
    struct task *parent;
    int waited;

    ktest_begin("ksys: the numbers of the children are bigger than the number of the parent");

    setup();

    /*
     * A machine without an address space per task refuses the copy of the
     * caller, and the kernel keeps the placeholder of a fork. The slot it
     * took for the child before asking the port is given back.
     */
    {
        struct task *copied = (struct task *)0x1;
        k_size_t before = task_count();

        KT_CHECK_INT_EQ(task_fork(task_self(), "nofork", &copied), -1);
        KT_CHECK(copied == NULL);
        KT_CHECK_INT_EQ(task_count(), before);
    }

    parent = task_create("parent", parent_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(parent != NULL);

    for (waited = 0; waited < 3000 && g_result < 0; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK(g_fork_result > g_parent_pid);
    KT_CHECK_INT_EQ(g_wait_result, g_fork_result);

    teardown();
}

/*
 * The program of the child and of the parent, the way a program of POSIX
 * writes it: fork() is 0 in the child and the number of the child in the
 * parent. The child of this fork is a task that re-runs this program from
 * its start, so it reaches the fork again - and gets the 0, which is what
 * tells it that it is the child.
 */
static void posix_program(void *arg)
{
    int pid;

    K_UNUSED(arg);

    pid = microos_fork();

    if (pid == 0) {
        /* The child: it finishes with the code 7, like the other tests. */
        g_child_done = 1;
        task_exit(7);
        return;
    }

    /* The parent: it remembers the number and waits for the child. */
    g_fork_result = pid;

    if (pid > 0) {
        g_wait_result = microos_waitpid(pid, (int *)&g_wait_status, 0);
    }

    g_result = 1;
}

/*
 * The property: the shape of the fork from the library is the usual one of
 * POSIX - 0 in the child, the number of the child in the parent - and the
 * parent waits for the child and gets the code it finished with.
 */
static void test_fork_posix(void)
{
    struct task *parent;
    int waited;

    ktest_begin("ksys: fork() from the library is 0 in the child and its number in the parent");

    setup();

    parent = task_create("posix", posix_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(parent != NULL);

    for (waited = 0; waited < 3000 && g_result < 0; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK_INT_EQ(g_result, 1);
    /* The parent got the number of the child, not 0 and not its own. */
    KT_CHECK(g_fork_result > 0);
    KT_CHECK_INT_EQ(g_wait_result, g_fork_result);
    KT_CHECK_INT_EQ(g_wait_status, 7);
    /* The child took the branch of the 0. */
    KT_CHECK_INT_EQ(g_child_done, 1);

    teardown();
}

/*
 * The property: a pid and a flag that cannot name a child are refused
 * instead of being guessed at, and the two shapes of "any child" stay
 * valid.
 */
static void test_wait_validation(void)
{
    int status = -1;
    int rc;

    ktest_begin("ksys: a pid that makes no sense is refused");

    setup();

    /* Only 0 and -1 mean "any child": -2 and below name no task. */
    KT_CHECK_INT_EQ(ksys_waitpid(-2, &status, KSYS_WNOHANG), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_waitpid(-100, &status, 0), KERR_INVAL);

    /* The flags of the call are one bit. */
    KT_CHECK_INT_EQ(ksys_waitpid(0, &status, 0x0100), KERR_INVAL);

    /* A wide pid must not be cut down to a small child by the cast to int;
     * the width of the arguments belongs to the target. */
    if (sizeof(ksys_arg_t) > sizeof(int)) {
        KT_CHECK_INT_EQ(
            ksys_call(KSYS_WAITPID, (ksys_arg_t)0x100000005ull, 0, KSYS_WNOHANG, 0, 0, 0),
            KERR_INVAL);
    }

    /* "Any child" still means any child. A task without a child is told so,
     * and a child that has not finished yet leaves the call with 0. */
    rc = ksys_waitpid(-1, &status, KSYS_WNOHANG);
    KT_CHECK(rc >= 0 || rc == KERR_CHILD);
    if (rc == 0) {
        KT_CHECK_INT_EQ(status, -1);
    }

    rc = ksys_waitpid(0, &status, KSYS_WNOHANG);
    KT_CHECK(rc >= 0 || rc == KERR_CHILD);

    teardown();
}

/*
 * The property: waiting for a child that does not exist gives an error
 * instead of hanging forever.
 */
static void test_wait_errors(void)
{
    int status = -1;

    ktest_begin("ksys: waiting for a child that does not exist fails");

    setup();

    /*
     * The call that does not wait answers at once: there is no child with
     * that number, so there is nothing that could finish.
     */
    KT_CHECK_INT_EQ(ksys_waitpid(4242, &status, KSYS_WNOHANG), KERR_CHILD);

    teardown();
}

/*
 * The property: the child and the parent write the same data, the order in
 * which they are waited for does not change the result.
 */
static void test_fork_data_is_the_same(void)
{
    struct task *parent;
    int waited;
    int i;

    ktest_begin("ksys: the data of the child is the same every time");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    for (i = 0; i < 4; i++) {
        memset(g_shared, 0, sizeof(g_shared));
        g_result = -1;
        g_child_done = 0;

        parent = task_create("parent", parent_program, NULL, TASK_PRIO_DEFAULT);
        KT_CHECK(parent != NULL);

        for (waited = 0; waited < 3000 && g_result < 0; waited++) {
            k_sched_sleep_ms(1);
        }

        KT_CHECK_INT_EQ(g_result, 1);
        KT_CHECK_INT_EQ(g_wait_status, 7);
        KT_CHECK_INT_EQ(memcmp(g_shared, g_text, g_text_len), 0);
    }

    teardown();
}

/* Write a 32-bit field the way the header of an image keeps it: the bytes of
 * the file, not a struct of the machine that reads them. */
static void put_u32(k_u8_t *out, k_u32_t value)
{
    out[0] = (k_u8_t)(value & 0xFFu);
    out[1] = (k_u8_t)((value >> 8) & 0xFFu);
    out[2] = (k_u8_t)((value >> 16) & 0xFFu);
    out[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/*
 * The smallest program of the format: the header and one word of text. The
 * word is never run by the test - the machine that runs the tests cannot run
 * a program at all - it only has to be an image the parser accepts.
 */
static k_size_t build_image(k_u8_t *image)
{
    put_u32(image + 0, K_EXEC_MAGIC);
    put_u32(image + 4, K_EXEC_VERSION);
    put_u32(image + 8, 0);                   /* the flags: none */
    put_u32(image + 12, K_EXEC_HEADER_SIZE); /* the entry: the first word */
    put_u32(image + 16, 4);                  /* one word of text */
    put_u32(image + 20, 0);                  /* no data */
    put_u32(image + 24, 0);                  /* no bss */
    put_u32(image + 28, 0);                  /* reserved */
    put_u32(image + 32, 0xC3u);              /* a return, were it ever run */

    return K_EXEC_HEADER_SIZE + 4;
}

static int write_file(const char *path, const void *data, k_size_t size)
{
    int fd = ksys_open(path, KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

    if (fd < 0) {
        return fd;
    }

    if (ksys_write(fd, data, size) != (k_ssize_t)size) {
        ksys_close(fd);
        return KERR_IO;
    }

    return ksys_close(fd);
}

/*
 * The property: execve reads the file through the VFS, checks the image and
 * asks the machine; a machine that cannot run a program answers ENOSYS, and
 * every refusal before that answer is the answer of its own reason. No
 * descriptor of the caller is spent by a failed exec.
 */
static void test_execve(void)
{
    static k_u8_t image[K_EXEC_HEADER_SIZE + 4];
    static const char junk[] = "this is not a program of this system\n";
    const k_size_t size = build_image(image);
    char *argv[] = {(char *)"/prog", NULL};
    int fd;

    ktest_begin("ksys: execve reads a program and asks the machine");

    setup();

    /* A file that is not there is not a program. */
    KT_CHECK_INT_EQ(ksys_execve("/no/such/program", NULL), KERR_NOENT);

    /* A directory is not a program. */
    KT_CHECK_INT_EQ(ksys_mkdir("/bin"), 0);
    KT_CHECK_INT_EQ(ksys_execve("/bin", NULL), KERR_ISDIR);

    /* A file that is not an image of this system, and a file too small to
     * hold even the header. The mode is opened to execution first, so that
     * the format is what refuses them. */
    KT_CHECK_INT_EQ(write_file("/junk", junk, sizeof(junk) - 1), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/junk", 0755), 0);
    KT_CHECK_INT_EQ(ksys_execve("/junk", NULL), KERR_INVAL);

    KT_CHECK_INT_EQ(write_file("/tiny", "MOX", 3), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/tiny", 0755), 0);
    KT_CHECK_INT_EQ(ksys_execve("/tiny", NULL), KERR_INVAL);

    /* A real image, but a file that may not be executed: the mode decides,
     * and 0644 has no x bit. */
    KT_CHECK_INT_EQ(write_file("/prog", image, size), 0);
    KT_CHECK_INT_EQ(ksys_execve("/prog", NULL), KERR_PERM);

    KT_CHECK_INT_EQ(ksys_chmod("/prog", 0755), 0);

    /*
     * The image is a program of the format of the system, and this machine
     * cannot run one: the simulator must not jump into code that is not the
     * kernel. The answer is ENOSYS, the same as on the board.
     */
    KT_CHECK_INT_EQ(ksys_execve("/prog", NULL), KERR_NOSYS);
    KT_CHECK_INT_EQ(ksys_execve("/prog", argv), KERR_NOSYS);

    /*
     * A status left by a program of another machine is not a success here:
     * ENOSYS is the answer of this one whatever the last program ended with,
     * and a vector of one word is not a reason for a different answer.
     */
    k_exec_exit(7);
    KT_CHECK_INT_EQ(ksys_execve("/prog", argv), KERR_NOSYS);

    /*
     * The vector of the program is bounded at the edge of the call before
     * anything is looked at: a word that does not fit the frame of the call
     * and a vector of too many words are E2BIG, and an empty vector is a
     * program without arguments - the answer of the machine, not a refusal.
     */
    {
        static char long_word[600];
        char *long_argv[] = {long_word, NULL};
        char *empty_argv[] = {NULL};
        char *many_argv[10];
        int i;

        memset(long_word, 'a', sizeof(long_word) - 1);
        KT_CHECK_INT_EQ(ksys_execve("/prog", long_argv), KERR_TOOBIG);

        for (i = 0; i < 9; i++) {
            many_argv[i] = (char *)"x";
        }
        many_argv[9] = NULL;
        KT_CHECK_INT_EQ(ksys_execve("/prog", many_argv), KERR_TOOBIG);

        KT_CHECK_INT_EQ(ksys_execve("/prog", empty_argv), KERR_NOSYS);
    }

    /* The raw call answers the same way, and a failed exec left the table of
     * the descriptors as it was: the file can still be opened. */
    KT_CHECK_INT_EQ(ksys_call(KSYS_EXECVE, (ksys_arg_t)(uintptr_t)"/prog", 0, 0, 0, 0, 0),
                    KERR_NOSYS);
    fd = ksys_open("/prog", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    teardown();
}

void test_ksys_process(void)
{
    test_fork_wait();
    test_fork_numbers();
    test_fork_posix();
    test_wait_errors();
    test_wait_validation();
    test_fork_data_is_the_same();
    test_execve();
}
