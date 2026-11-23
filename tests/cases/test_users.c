/*
 * Micro OS — the tests of the credentials of a task.
 *
 * The properties: a task has a uid and a gid, they come from the task that
 * made it, and the uid decides the triads of the mode of every object. Root
 * passes them; a task of another uid reads the triad of the owner when it is
 * the owner and the triad of the others otherwise. The privileged acts -
 * chown, mount, umount, a signal to a task of another user - belong to root.
 *
 * The runner of the tests is a kernel task and starts as root. A test that
 * needs another identity moves the runner there with task_set_uid() and puts
 * it back at the end: the call of the ABI (setuid) is the subject of a test
 * and not the fixture, because a task that gave up root cannot take it back
 * with that call.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

/* What a child records about itself, for the test of the inheritance. */
static volatile k_u32_t g_child_uid;
static volatile k_u32_t g_child_gid;
static volatile int g_victim_running;
static volatile k_u32_t g_victim_uid;

static void setup(void)
{
    if (mfs_ram_init(&g_volume, 256) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 32);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
    ksys_init();

    g_child_uid = 0xFFFFFFFFu;
    g_child_gid = 0xFFFFFFFFu;
    g_victim_running = 0;
    g_victim_uid = 0xFFFFFFFFu;
}

static void teardown(void)
{
    /* A test that failed in the middle still leaves a root runner. */
    task_set_uid(task_self(), 0);
    task_set_gid(task_self(), 0);

    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* A child of any kind: it records the credentials it was made with. */
static void creds_child(void *arg)
{
    struct task *self = task_self();

    (void)arg;

    if (self != NULL) {
        g_child_uid = task_get_uid(self);
        g_child_gid = task_get_gid(self);
    }
}

/* A task that waits to be signalled. ksys_sleep() is the point where a
 * signal of the kernel is answered. */
static void victim_task(void *arg)
{
    struct task *self = task_self();

    (void)arg;

    if (self != NULL) {
        g_victim_uid = task_get_uid(self);
    }

    g_victim_running = 1;

    while (g_victim_running) {
        ksys_sleep(5);
    }
}

/*
 * The property: a task starts as root, a task created by another takes its
 * uid and gid, a fork does the same, and the calls of the ABI read and move
 * the numbers with the rule of one credential.
 */
static void test_creds_inherit(void)
{
    struct task *self = task_self();
    struct task *child;
    int waited;

    ktest_begin("users: the credentials are inherited by a task and a fork");

    setup();
    KT_CHECK(self != NULL);

    KT_CHECK_INT_EQ(task_get_uid(self), 0);
    KT_CHECK_INT_EQ(task_get_gid(self), 0);
    KT_CHECK_INT_EQ(ksys_getuid(), 0);
    KT_CHECK_INT_EQ(ksys_geteuid(), 0);
    KT_CHECK_INT_EQ(ksys_getgid(), 0);

    /* Root may take any number of the fifteen bits of a record. The gid
     * moves first: after the setuid the task is no longer root. */
    KT_CHECK_INT_EQ(ksys_setgid(3), 0);
    KT_CHECK_INT_EQ(ksys_setuid(7), 0);
    KT_CHECK_INT_EQ(ksys_getuid(), 7);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(ksys_getgid(), 3);

    /* task_create() passes the credentials of the caller. */
    child = task_create("cred-child", creds_child, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(child != NULL);
    if (child != NULL) {
        waited = task_wait(child->pid, NULL, 2000);
        KT_CHECK_INT_EQ(waited, 0);
        KT_CHECK_INT_EQ(g_child_uid, 7);
        KT_CHECK_INT_EQ(g_child_gid, 3);
    }

    /* task_fork_shared() is the child of a fork and of a program. */
    g_child_uid = 0xFFFFFFFFu;
    g_child_gid = 0xFFFFFFFFu;
    child = task_fork_shared(self, "cred-fork", creds_child, NULL);
    KT_CHECK(child != NULL);
    if (child != NULL) {
        waited = task_wait(child->pid, NULL, 2000);
        KT_CHECK_INT_EQ(waited, 0);
        KT_CHECK_INT_EQ(g_child_uid, 7);
        KT_CHECK_INT_EQ(g_child_gid, 3);
    }

    /* A task of another uid may only name itself; the numbers outside the
     * fifteen bits are not a uid at all. */
    KT_CHECK_INT_EQ(ksys_setuid(7), 0);
    KT_CHECK_INT_EQ(ksys_setuid(0), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_setuid(8), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_getuid(), 7);
    KT_CHECK_INT_EQ(ksys_setgid(0), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_getgid(), 3);
    KT_CHECK_INT_EQ(ksys_setuid(KSYS_UID_MAX + 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_setgid(KSYS_UID_MAX + 1u), KERR_INVAL);

    /*
     * Root takes any number, the highest of a record included. The way back
     * is not a call of this ABI: with one credential a task that gave up
     * root cannot name zero again, which is the price recorded in
     * docs/debts.md. The fixture restores the runner.
     */
    task_set_uid(self, 0);
    task_set_gid(self, 0);
    KT_CHECK_INT_EQ(ksys_setuid(KSYS_UID_MAX), 0);
    KT_CHECK_INT_EQ(ksys_getuid(), KSYS_UID_MAX);
    KT_CHECK_INT_EQ(ksys_setuid(0), KERR_PERM);

    task_set_uid(self, 0);
    task_set_gid(self, 0);
    KT_CHECK_INT_EQ(ksys_setuid(0), 0);
    KT_CHECK_INT_EQ(ksys_setgid(0), 0);

    teardown();
}

/*
 * The property: a file of 0640 whose owner is uid 7 is read and written by
 * root, read by 7 and not touched by 8; the triad of the others is what a
 * task that is not the owner reads, for a read and for a write alike.
 */
static void test_uid_permissions(void)
{
    struct task *self = task_self();
    char byte = 'x';
    int fd;

    ktest_begin("users: the uid of the caller decides the triad of the mode");

    setup();

    fd = ksys_open("/u.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    KT_CHECK_INT_EQ(ksys_chown("/u.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/u.txt", 0640), 0);

    /* Root reads and writes the file of another owner. */
    fd = ksys_open("/u.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    fd = ksys_open("/u.txt", KSYS_O_WRONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The owner of a 0640 file reads and writes it. */
    task_set_uid(self, 7);
    fd = ksys_open("/u.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    fd = ksys_open("/u.txt", KSYS_O_WRONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A task of uid 8 reads the triad of the others, which is zero in a
     * 0640 file: neither the read nor the write is allowed, and neither is
     * the mode or the owner. */
    task_set_uid(self, 8);
    KT_CHECK_INT_EQ(ksys_open("/u.txt", KSYS_O_RDONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_open("/u.txt", KSYS_O_WRONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_chmod("/u.txt", 0640), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_chown("/u.txt", 8), KERR_PERM);

    /* The owner changes the mode of what it owns; giving the object away is
     * still the act of root. */
    task_set_uid(self, 7);
    KT_CHECK_INT_EQ(ksys_chmod("/u.txt", 0440), 0);
    KT_CHECK_INT_EQ(ksys_open("/u.txt", KSYS_O_WRONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_open("/u.txt", KSYS_O_RDWR), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_chown("/u.txt", 7), KERR_PERM);

    /* The others bits are the bits of that task: 0004 reads, 0006 writes,
     * even though the owner of the file is somebody else. */
    task_set_uid(self, 0);
    KT_CHECK_INT_EQ(ksys_chmod("/u.txt", 0604), 0);
    task_set_uid(self, 8);
    fd = ksys_open("/u.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_open("/u.txt", KSYS_O_WRONLY), KERR_PERM);

    task_set_uid(self, 0);
    KT_CHECK_INT_EQ(ksys_chmod("/u.txt", 0606), 0);
    task_set_uid(self, 8);
    fd = ksys_open("/u.txt", KSYS_O_WRONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The record is still the one root wrote. */
    task_set_uid(self, 0);
    {
        struct vfs_stat info;

        KT_CHECK_INT_EQ(ksys_stat("/u.txt", &info), 0);
        KT_CHECK_INT_EQ(info.owner, 7);
        KT_CHECK_INT_EQ(info.mode, 0606);
    }

    teardown();
}

/* The file a test of a missing object asks for is not there. */
static void test_privileged_acts(void)
{
    struct task *self = task_self();
    struct task *victim;
    int waited;

    ktest_begin("users: chown, mount, umount and a foreign signal need root");

    setup();

    /* chown of an object that the caller owns is still root's act. */
    {
        char byte = 'x';
        int fd = ksys_open("/owned.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

        KT_CHECK(fd >= 0);
        if (fd >= 0) {
            KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
            KT_CHECK_INT_EQ(ksys_close(fd), 0);
        }
    }

    KT_CHECK_INT_EQ(ksys_chown("/owned.txt", 7), 0);
    task_set_uid(self, 7);
    KT_CHECK_INT_EQ(ksys_chown("/owned.txt", 7), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_chown("/owned.txt", 8), KERR_PERM);

    /* The table of the volumes belongs to root: the privilege is named
     * before the type of a volume that is not there. */
    KT_CHECK_INT_EQ(vfs_mount_type("/mnt", "no-such-type", "no-such-device"), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_umount("/"), KERR_PERM);

    /* A task signals itself, and not a task of another uid. */
    victim = task_create("victim", victim_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(victim != NULL);
    if (victim != NULL) {
        int spins = 0;

        while (g_victim_uid == 0xFFFFFFFFu && spins < 2000) {
            k_sched_sleep_ms(1);
            spins++;
        }
        KT_CHECK_INT_EQ(g_victim_uid, 7);

        task_set_uid(self, 8);
        KT_CHECK_INT_EQ(ksys_kill((int)victim->pid, KSYS_SIGTERM), KERR_PERM);

        /*
         * The kernel itself may still send to it - the policy is the one of
         * the call of a task, not of the helper - and SIGCHLD does not end
         * the victim, which proves the refusal above was the uid and not a
         * dead task.
         */
        KT_CHECK_INT_EQ(task_signal_send(victim->pid, KSYS_SIGCHLD), 0);
        KT_CHECK(task_by_pid(victim->pid) != NULL);

        /* Root ends it, and the child leaves with the signal. */
        task_set_uid(self, 0);
        KT_CHECK_INT_EQ(ksys_kill((int)victim->pid, KSYS_SIGTERM), 0);
        waited = task_wait(victim->pid, NULL, 2000);
        KT_CHECK_INT_EQ(waited, 0);
    }

    /* Root is answered by the type of a volume, not by the privilege. */
    KT_CHECK_INT_EQ(vfs_mount_type("/mnt", "no-such-type", "no-such-device"), KERR_INVAL);

    teardown();
}

void test_users(void)
{
    test_creds_inherit();
    test_uid_permissions();
    test_privileged_acts();
}
