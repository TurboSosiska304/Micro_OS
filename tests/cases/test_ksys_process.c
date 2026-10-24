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
static struct mfs_device g_device;
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
 * The property: waiting for a child that does not exist gives an error
 * instead of hanging forever.
 */
static void test_wait_errors(void)
{
    int status = -1;

    ktest_begin("ksys: waiting for a child that does not exist fails");

    setup();

    /*
     * The call that does not wait answers at once: there is no child that
     * could finish.
     */
    KT_CHECK_INT_EQ(ksys_waitpid(4242, &status, KSYS_WNOHANG), 0);

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

void test_ksys_process(void)
{
    test_fork_wait();
    test_fork_numbers();
    test_fork_posix();
    test_wait_errors();
    test_fork_data_is_the_same();
}
