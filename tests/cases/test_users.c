/*
 * Micro OS — the tests of the credentials of a task and of the accounts.
 *
 * The properties: a task has a uid and a gid, they come from the task that
 * made it, and the uid decides the triads of the mode of every object. Root
 * passes them; a task of another uid reads the triad of the owner when it is
 * the owner and the triad of the others otherwise. The privileged acts -
 * chown, mount, umount, a signal to a task of another user - belong to root.
 *
 * The second half is the file of accounts: a line of /etc/passwd is a record
 * of five fields, the hash of /etc/shadow is the salted SHA-256 of this tree,
 * a name and a uid are found in the file and nowhere else, and the act of
 * `login` and `su` moves the two credentials of the task together and enters
 * the home directory of the account.
 *
 * The runner of the tests is a kernel task and starts as root. A test that
 * needs another identity moves the runner there with task_set_uid() and puts
 * it back at the end: the call of the ABI (setuid) is the subject of a test
 * and not the fixture, because a task that gave up root cannot take it back
 * with that call.
 */
#include <stdatomic.h>

#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "config/users.h"
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
static _Atomic(k_u32_t) g_child_uid;
static _Atomic(k_u32_t) g_child_gid;
static atomic_int g_victim_running;
static _Atomic(k_u32_t) g_victim_uid;

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
static void test_directory_bits(void)
{
    struct task *self = task_self();
    char byte = 'x';
    int fd;

    ktest_begin("users: a directory of another uid is not a place to write");

    setup();

    /* Root makes a directory of its own with a file in it. */
    KT_CHECK_INT_EQ(ksys_mkdir("/private"), 0);
    fd = ksys_open("/private/secret", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_chmod("/private", 0755), 0);

    /*
     * The bit of an entry is not the permission to change the directory it
     * lives in: a task of uid 7 cannot remove the file, cannot make another
     * one beside it, and cannot take the file away under another name. The
     * cleanroom of 1.6 deleted /etc/passwd this way.
     */
    task_set_uid(self, 7);

    KT_CHECK_INT_EQ(ksys_unlink("/private/secret"), KERR_PERM);
    fd = ksys_open("/private/other", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK_INT_EQ(fd, KERR_PERM);
    if (fd >= 0) {
        (void)ksys_close(fd);
    }
    KT_CHECK_INT_EQ(ksys_mkdir("/private/sub"), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_rename("/private/secret", "/private/moved"), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_rmdir("/private"), KERR_PERM);

    /* The file is still there, with the byte root wrote. */
    task_set_uid(self, 0);
    fd = ksys_open("/private/secret", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        char got = 0;

        KT_CHECK_INT_EQ(ksys_read(fd, &got, 1), 1);
        KT_CHECK(got == 'x');
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A directory that gives the write bit to the others is theirs. */
    KT_CHECK_INT_EQ(ksys_chmod("/private", 0777), 0);
    task_set_uid(self, 7);
    fd = ksys_open("/private/other", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_unlink("/private/other"), 0);

    task_set_uid(self, 0);

    teardown();
}

/* A task of a uid and a gid, for a row of the matrix. The runner is the only
 * task of a test and it starts as root, so this is the fixture that is not
 * the call under test (setuid does not move the gid and cannot come back). */
static void as_user(struct task *self, k_u32_t uid, k_u32_t gid)
{
    task_set_uid(self, uid);
    task_set_gid(self, gid);
}

/*
 * The matrix of the access, row by row: the owner triad for the owner, the
 * triad of the group for a task whose gid is the group of the object, the
 * triad of the others for everybody else, and root above the three. A row
 * that is not in this test is a row nobody has thought about (ADR 0044 §3).
 *
 * The file is 0640 of uid 7 and gid 7, so the three rows answer: read and
 * write (owner), read alone (group), nothing (others).
 */
static void test_group_permissions(void)
{
    struct task *self = task_self();
    struct vfs_stat info;
    struct vfs_statx info_x;
    char byte = 'g';
    int fd;

    ktest_begin("users: the gid of the caller decides the triad of the group");

    setup();

    fd = ksys_open("/g.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    KT_CHECK_INT_EQ(ksys_chown("/g.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/g.txt", 0640), 0);

    /* The record keeps the three numbers, and stat and statx agree. */
    KT_CHECK_INT_EQ(ksys_stat("/g.txt", &info), 0);
    KT_CHECK_INT_EQ(info.owner, 7);
    KT_CHECK_INT_EQ(info.group, 7);
    KT_CHECK_INT_EQ(info.mode, 0640);
    KT_CHECK_INT_EQ(ksys_statx("/g.txt", &info_x), 0);
    KT_CHECK_INT_EQ(info_x.owner, 7);
    KT_CHECK_INT_EQ(info_x.group, 7);

    /* Root: the two triads are passed. */
    as_user(self, 0, 0);
    fd = ksys_open("/g.txt", KSYS_O_RDWR);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The owner reads its own triad even when another group would be wider:
     * uid 7 with gid 8 is still the owner of the file of gid 7. */
    as_user(self, 7, 8);
    fd = ksys_open("/g.txt", KSYS_O_RDWR);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The row of the group: uid 8 is not the owner, gid 7 is the group of
     * the object, so the r of 0640 reads and the w of it is not there. */
    as_user(self, 8, 7);
    fd = ksys_open("/g.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_open("/g.txt", KSYS_O_WRONLY), KERR_PERM);

    /* The row of the others: uid 8 with gid 8 is neither the owner nor the
     * group, and the others of 0640 are zero. */
    as_user(self, 8, 8);
    KT_CHECK_INT_EQ(ksys_open("/g.txt", KSYS_O_RDONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_open("/g.txt", KSYS_O_WRONLY), KERR_PERM);

    /* The group of the object is what decides, not the gid of the task that
     * made it: the same uid 8 reads the file after a chown, and a file of
     * another group is not read by the gid it used to have. */
    as_user(self, 0, 0);
    KT_CHECK_INT_EQ(ksys_chmod("/g.txt", 0604), 0);
    as_user(self, 8, 8);
    fd = ksys_open("/g.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /*
     * The act of chgrp: root gives the object to any group; the owner gives
     * it to the group of its own task and to no other; a third task does not
     * touch it at all. A group wider than the fifteen bits of a record is
     * refused at the door.
     */
    as_user(self, 0, 0);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_stat("/g.txt", &info), 0);
    KT_CHECK_INT_EQ(info.group, 7);

    as_user(self, 7, 7);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 9), KERR_PERM);

    as_user(self, 7, 9);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 7), KERR_PERM); /* not the group of the task */
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 9), 0);         /* the group of the task */
    KT_CHECK_INT_EQ(ksys_stat("/g.txt", &info), 0);
    KT_CHECK_INT_EQ(info.group, 9);

    as_user(self, 8, 9);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", 9), KERR_PERM);

    as_user(self, 0, 0);
    KT_CHECK_INT_EQ(ksys_chgrp("/g.txt", KSYS_GID_MAX + 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_chgrp("/no/such/file", 7), KERR_NOENT);

    /*
     * The group of a new object is the gid of its maker, with the owner in
     * the same act, and a directory with the two triads of the group is a
     * place the group may write in.
     */
    as_user(self, 0, 0);
    KT_CHECK_INT_EQ(ksys_mkdir("/people"), 0);
    KT_CHECK_INT_EQ(ksys_chown("/people", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/people", 0755), 0);

    as_user(self, 7, 7);
    fd = ksys_open("/people/made.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_mkdir("/people/made-dir"), 0);

    KT_CHECK_INT_EQ(ksys_stat("/people/made.txt", &info), 0);
    KT_CHECK_INT_EQ(info.owner, 7);
    KT_CHECK_INT_EQ(info.group, 7);
    KT_CHECK_INT_EQ(ksys_stat("/people/made-dir", &info), 0);
    KT_CHECK_INT_EQ(info.owner, 7);
    KT_CHECK_INT_EQ(info.group, 7);

    as_user(self, 0, 0);
    KT_CHECK_INT_EQ(ksys_mkdir("/gd"), 0);
    KT_CHECK_INT_EQ(ksys_chgrp("/gd", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/gd", 0770), 0);

    /* A member of the group writes in the directory of the group. */
    as_user(self, 8, 7);
    fd = ksys_open("/gd/a", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A task of another group has no such right even with the same uid. */
    as_user(self, 8, 8);
    KT_CHECK_INT_EQ(ksys_open("/gd/b", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_chgrp("/gd", 8), KERR_PERM);

    /* The walk of a path reads the triad of the group of the directory: the
     * x of 0770 is there for gid 7 and not for gid 8. */
    as_user(self, 8, 7);
    KT_CHECK_INT_EQ(ksys_stat("/gd/a", &info), 0);
    as_user(self, 8, 8);
    KT_CHECK_INT_EQ(ksys_stat("/gd/a", &info), KERR_PERM);

    as_user(self, 0, 0);

    teardown();
}

/*
 * The property: a task has the three uids of POSIX (ADR 0053). The effective
 * uid is the one every check reads, the real one is the person and the saved
 * one is what `setuid` may return to; a setuid image moves the effective and
 * the saved uid and leaves the real one, and `setuid` of a task that is not
 * root names its real or its saved uid and nothing else.
 */
static void test_three_uids(void)
{
    struct task *self = task_self();
    char byte = 'u';
    int fd;

    ktest_begin("users: the real, the effective and the saved uid of a task");

    setup();

    /* A task of the shell has the three the same, and the two calls agree. */
    KT_CHECK_INT_EQ(task_get_ruid(self), 0);
    KT_CHECK_INT_EQ(task_get_uid(self), 0);
    KT_CHECK_INT_EQ(task_get_suid(self), 0);
    KT_CHECK_INT_EQ(ksys_getuid(), 0);
    KT_CHECK_INT_EQ(ksys_geteuid(), 0);

    fd = ksys_open("/secret", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_chmod("/secret", 0600), 0);
    KT_CHECK_INT_EQ(ksys_chown("/secret", 0), 0);

    /*
     * The act of a setuid image: the effective and the saved uid become the
     * owner of the file and the real uid is the person. The file of root is
     * then out of reach although the real uid of the task is zero - the
     * checks read the effective uid and nothing else.
     */
    task_set_euid(self, 7);
    KT_CHECK_INT_EQ(ksys_getuid(), 0);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(task_get_suid(self), 7);
    KT_CHECK_INT_EQ(ksys_open("/secret", KSYS_O_RDONLY), KERR_PERM);

    /* The real uid is a way back, as POSIX has it, and it is the only one
     * that is: 9 is neither the real nor the saved uid. */
    KT_CHECK_INT_EQ(ksys_setuid(9), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(ksys_setuid(0), 0);
    KT_CHECK_INT_EQ(ksys_geteuid(), 0);

    fd = ksys_open("/secret", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The saved uid is the other way: the task of the image returns to the
     * identity it was given, and that identity is the one the checks read. */
    task_set_euid(self, 7);
    KT_CHECK_INT_EQ(ksys_setuid(7), 0);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(ksys_open("/secret", KSYS_O_RDONLY), KERR_PERM);

    /* A task whose three uids are 7 has no way back to root with a call:
     * `logout` on the console and the init that makes the next session are
     * the door (ADR 0044). */
    task_set_uid(self, 7);
    KT_CHECK_INT_EQ(ksys_getuid(), 7);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(ksys_setuid(0), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_setuid(7), 0);

    /* Root moves the three together, which is what a login does. */
    task_set_uid(self, 0);
    KT_CHECK_INT_EQ(ksys_setuid(7), 0);
    KT_CHECK_INT_EQ(ksys_getuid(), 7);
    KT_CHECK_INT_EQ(ksys_geteuid(), 7);
    KT_CHECK_INT_EQ(task_get_suid(self), 7);
    KT_CHECK_INT_EQ(ksys_setuid(0), KERR_PERM);

    task_set_uid(self, 0);

    teardown();
}

static void test_cwd_fit(void)
{
    struct task *self = task_self();

    ktest_begin("users: a directory that does not fit is refused, not cut");

    /*
     * A cwd cut at 31 characters is another directory, and a shell that
     * walks it removes the files of somebody else: the second defect of the
     * cleanroom of 1.6. The shell keeps the directory it had.
     */
    KT_CHECK(task_set_cwd(self, "/short"));
    KT_CHECK_STR_EQ(task_get_cwd(self), "/short");

    KT_CHECK(!task_set_cwd(
        self, "/a/very/long/directory/name/that/does/not/fit/in/the/cwd/of/a/task/at/all/"
              "and/more/segments/beyond/the/length/of/the/field/that/keeps/it"));
    KT_CHECK_STR_EQ(task_get_cwd(self), "/short");

    KT_CHECK(task_set_cwd(self, "/"));
}

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

/*
 * The property: a line of /etc/passwd is a record of five fields or it is
 * nothing, the fields are the name, the two numbers, the home and the shell,
 * and a line of another format is refused instead of guessed at.
 */
static void test_accounts_records(void)
{
    struct user_account account;
    char name[USERS_NAME_MAX];
    char hash[USERS_LINE_MAX];

    ktest_begin("users: a line of /etc/passwd is five fields");

    KT_CHECK(users_parse_passwd("root:0:0:/:/bin/sh", &account));
    KT_CHECK_STR_EQ(account.name, "root");
    KT_CHECK_INT_EQ(account.uid, 0);
    KT_CHECK_INT_EQ(account.gid, 0);
    KT_CHECK_STR_EQ(account.home, "/");
    KT_CHECK_STR_EQ(account.shell, "/bin/sh");

    KT_CHECK(users_parse_passwd("guest:7:7:/home/guest:/bin/sh\r\n", &account));
    KT_CHECK_STR_EQ(account.name, "guest");
    KT_CHECK_INT_EQ(account.uid, 7);
    KT_CHECK_INT_EQ(account.gid, 7);
    KT_CHECK_STR_EQ(account.home, "/home/guest");

    /* The highest uid of a record of fifteen bits is a uid. */
    KT_CHECK(users_parse_passwd("nobody:32767:32767:/:/bin/sh", &account));
    KT_CHECK_INT_EQ(account.uid, 32767);

    /* An empty line and a comment are not records; neither is a line of
     * four fields, of six, of a uid that is not a number, of a uid past the
     * fifteen bits, or of a field that does not fit. */
    KT_CHECK(!users_parse_passwd("", &account));
    KT_CHECK(!users_parse_passwd("# a comment", &account));
    KT_CHECK(!users_parse_passwd("root:0:0:/", &account));
    KT_CHECK(!users_parse_passwd("root:0:0:/:/bin/sh:extra", &account));
    KT_CHECK(!users_parse_passwd("root:x:0:/:/bin/sh", &account));
    KT_CHECK(!users_parse_passwd("root:0:0:/:/bin/sh", NULL));
    KT_CHECK(!users_parse_passwd("root:32768:0:/:/bin/sh", &account));
    KT_CHECK(!users_parse_passwd(":0:0:/:/bin/sh", &account));
    KT_CHECK(!users_parse_passwd("a/b:0:0:/:/bin/sh", &account));
    KT_CHECK(!users_parse_passwd("averyveryverylong:0:0:/:/bin/sh", &account));
    KT_CHECK(!users_parse_passwd(
        "root:0:0:/a-home-directory-that-is-much-too-long-for-the-record:/bin/sh", &account));

    /* /etc/shadow is `name:hash`, and the hash may be the `*` of an account
     * without a password. */
    KT_CHECK(users_parse_shadow("root:$sha256$mox1$00", name, sizeof(name), hash, sizeof(hash)));
    KT_CHECK_STR_EQ(name, "root");
    KT_CHECK_STR_EQ(hash, "$sha256$mox1$00");
    KT_CHECK(users_parse_shadow("guest:*\r\n", name, sizeof(name), hash, sizeof(hash)));
    KT_CHECK_STR_EQ(hash, "*");
    KT_CHECK(!users_parse_shadow("root", name, sizeof(name), hash, sizeof(hash)));
    KT_CHECK(!users_parse_shadow("root:a:b", name, sizeof(name), hash, sizeof(hash)));
    KT_CHECK(!users_parse_shadow("# a comment", name, sizeof(name), hash, sizeof(hash)));

    /* The hash of this tree: SHA-256 of the salt and the password, in the
     * sixty-four digits of the file. The numbers are the ones of sha256sum. */
    KT_CHECK(users_hash_make("microos", "mox1", hash, sizeof(hash)));
    KT_CHECK_STR_EQ(
        hash, "$sha256$mox1$da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c");
    KT_CHECK(users_hash_check(hash, "microos"));
    KT_CHECK(!users_hash_check(hash, "microo"));
    KT_CHECK(!users_hash_check(hash, "microoss"));
    KT_CHECK(!users_hash_check(hash, ""));

    /* A salt of its own gives a digest of its own, and the same salt and
     * password give the same digest every time. */
    {
        char again[USERS_LINE_MAX];
        char other[USERS_LINE_MAX];

        KT_CHECK(users_hash_make("microos", "mox1", again, sizeof(again)));
        KT_CHECK_STR_EQ(again, hash);
        KT_CHECK(users_hash_make("hunter2", "s1", other, sizeof(other)));
        KT_CHECK_STR_EQ(
            other, "$sha256$s1$ebd45ded486507e4a30383e322e28c6cfc1be36610b1be98d98f50ce6ae2f823");
        KT_CHECK(users_hash_check(other, "hunter2"));
        KT_CHECK(!users_hash_check(other, "microos"));
    }

    /* A salt that is empty, too long or carries the separator is not a salt;
     * an answer that does not fit is refused, not cut. */
    KT_CHECK(!users_hash_make("microos", "", hash, sizeof(hash)));
    KT_CHECK(!users_hash_make("microos", "0123456789abcdefg", hash, sizeof(hash)));
    KT_CHECK(!users_hash_make("microos", "a$b", hash, sizeof(hash)));
    KT_CHECK(!users_hash_make("microos", "mox1", hash, 20));

    /* A locked account and a hash of another format open with nothing. */
    KT_CHECK(!users_hash_check("*", ""));
    KT_CHECK(!users_hash_check("*", "microos"));
    KT_CHECK(!users_hash_check("", "microos"));
    KT_CHECK(!users_hash_check("!$sha256$mox1$x", "x"));
    KT_CHECK(!users_hash_check("microos", "microos"));
    KT_CHECK(!users_hash_check("$sha256$mox1", "microos"));
    KT_CHECK(!users_hash_check("$sha256$mox1$00", "microos"));
    KT_CHECK(!users_hash_check("$md5$mox1$da6e", "microos"));
}

/* A text written into a file of the volume. */
static void write_text(const char *path, const char *text)
{
    int fd = ksys_open(path, KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, text, strlen(text)), (int)strlen(text));
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
}

/*
 * The property: a name and a uid are found in /etc/passwd and nowhere else,
 * the hash of a name is in /etc/shadow, and a file that is not a file of
 * records is refused instead of read in part.
 */
static void test_accounts_file(void)
{
    struct user_account account;
    char hash[USERS_LINE_MAX];

    ktest_begin("users: the accounts are the files of /etc");

    setup();
    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);

    /* No file at all is no account at all. */
    KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_uid(0, &account), USERS_ENOENT);

    write_text(USERS_PASSWD_PATH, "# the accounts of the test\n"
                                  "root:0:0:/:/bin/sh\n"
                                  "guest:7:7:/home/guest:/bin/sh\n");
    write_text(USERS_SHADOW_PATH,
               "root:$sha256$mox1$"
               "da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c\n"
               "guest:*\n");

    KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_OK);
    KT_CHECK_STR_EQ(account.name, "guest");
    KT_CHECK_INT_EQ(account.uid, 7);
    KT_CHECK_INT_EQ(account.gid, 7);
    KT_CHECK_STR_EQ(account.home, "/home/guest");
    KT_CHECK_STR_EQ(account.shell, "/bin/sh");

    KT_CHECK_INT_EQ(users_find_uid(0, &account), USERS_OK);
    KT_CHECK_STR_EQ(account.name, "root");
    KT_CHECK_INT_EQ(users_find_uid(7, &account), USERS_OK);
    KT_CHECK_STR_EQ(account.name, "guest");

    KT_CHECK_INT_EQ(users_find_name("nobody", &account), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_uid(8, &account), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_name(NULL, &account), USERS_ENOENT);

    KT_CHECK_INT_EQ(users_find_hash("root", hash, sizeof(hash)), USERS_OK);
    KT_CHECK_STR_EQ(hash, "$sha256$mox1$"
                          "da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c");
    KT_CHECK_INT_EQ(users_find_hash("guest", hash, sizeof(hash)), USERS_OK);
    KT_CHECK_STR_EQ(hash, "*");
    KT_CHECK_INT_EQ(users_find_hash("nobody", hash, sizeof(hash)), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_hash("root", hash, 8), USERS_EFORMAT);

    /* A line that is not a record stops the read before the name: a file of
     * accounts is read whole or not at all. A name that is found before the
     * broken line is still found, because the read stops at the match. */
    write_text(USERS_PASSWD_PATH, "garbage\nroot:0:0:/:/bin/sh\n");
    KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_EFORMAT);
    KT_CHECK_INT_EQ(users_find_uid(0, &account), USERS_EFORMAT);

    write_text(USERS_PASSWD_PATH, "root:0:0:/:/bin/sh\ngarbage\n");
    KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_OK);
    KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_EFORMAT);

    /* A file that does not fit is refused as well. */
    {
        char big[USERS_FILE_MAX + 64];
        size_t i;

        for (i = 0; i + 1 < sizeof(big); i++) {
            big[i] = (i % 60 == 59) ? '\n' : 'x';
        }
        big[sizeof(big) - 1] = '\0';

        write_text(USERS_PASSWD_PATH, big);
        KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_EFORMAT);
    }

    teardown();
}

/*
 * The property: /etc/group is a file of records `name:gid:member,member`, a
 * line that is not one stops the read, and a group is found by its name and
 * by its gid and nowhere else (ADR 0044 §4).
 */
static void test_accounts_group(void)
{
    struct user_group group;
    struct user_group parsed;

    ktest_begin("users: the groups are the file of /etc/group");

    setup();
    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);

    /* No file at all is no group at all. */
    KT_CHECK_INT_EQ(users_find_group("root", &group), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_gid(0, &group), USERS_ENOENT);

    /* The record of the format, with and without a member list. */
    KT_CHECK(users_parse_group("root:0:\n", &parsed));
    KT_CHECK_STR_EQ(parsed.name, "root");
    KT_CHECK_INT_EQ(parsed.gid, 0);
    KT_CHECK_STR_EQ(parsed.members, "");

    KT_CHECK(users_parse_group("staff:12:alice,bob", &parsed));
    KT_CHECK_STR_EQ(parsed.name, "staff");
    KT_CHECK_INT_EQ(parsed.gid, 12);
    KT_CHECK_STR_EQ(parsed.members, "alice,bob");

    /* A line that is not a record of this file: a comment, an empty line, a
     * missing field, a fourth field, a gid that is not a number of fifteen
     * bits, a name with a slash in it, an empty member, a name too long. */
    KT_CHECK(!users_parse_group("# a comment", &parsed));
    KT_CHECK(!users_parse_group("", &parsed));
    KT_CHECK(!users_parse_group("staff:12", &parsed));
    KT_CHECK(!users_parse_group("staff:12:a:b", &parsed));
    KT_CHECK(!users_parse_group("staff:x:", &parsed));
    KT_CHECK(!users_parse_group("staff:40000:", &parsed));
    KT_CHECK(!users_parse_group("st/aff:12:", &parsed));
    KT_CHECK(!users_parse_group("staff:12:alice,,bob", &parsed));
    KT_CHECK(!users_parse_group("staff:12:,alice", &parsed));
    KT_CHECK(!users_parse_group("a-name-of-sixteen:12:", &parsed));

    write_text(USERS_GROUP_PATH, "# the groups of the test\n"
                                 "root:0:\n"
                                 "guest:7:guest\n"
                                 "staff:12:alice,bob\n");

    KT_CHECK_INT_EQ(users_find_group("guest", &group), USERS_OK);
    KT_CHECK_STR_EQ(group.name, "guest");
    KT_CHECK_INT_EQ(group.gid, 7);
    KT_CHECK_STR_EQ(group.members, "guest");

    KT_CHECK_INT_EQ(users_find_gid(12, &group), USERS_OK);
    KT_CHECK_STR_EQ(group.name, "staff");
    KT_CHECK_INT_EQ(users_find_gid(0, &group), USERS_OK);
    KT_CHECK_STR_EQ(group.name, "root");

    KT_CHECK_INT_EQ(users_find_group("nobody", &group), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_gid(8, &group), USERS_ENOENT);
    KT_CHECK_INT_EQ(users_find_group(NULL, &group), USERS_ENOENT);

    /* Half a file of groups is a different set of groups: a broken line stops
     * the read with the error of the file and not with the next record. */
    write_text(USERS_GROUP_PATH, "garbage\nroot:0:\n");
    KT_CHECK_INT_EQ(users_find_group("root", &group), USERS_EFORMAT);
    KT_CHECK_INT_EQ(users_find_gid(0, &group), USERS_EFORMAT);

    write_text(USERS_GROUP_PATH, "root:0:\ngarbage\n");
    KT_CHECK_INT_EQ(users_find_group("root", &group), USERS_OK);
    KT_CHECK_INT_EQ(users_find_gid(7, &group), USERS_EFORMAT);

    /* The primary group of an account is the gid of its line of /etc/passwd,
     * and the default file of the groups names the two accounts of the tree. */
    write_text(USERS_PASSWD_PATH, USERS_DEFAULT_PASSWD);
    write_text(USERS_GROUP_PATH, USERS_DEFAULT_GROUP);
    {
        struct user_account account;

        KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_OK);
        KT_CHECK_INT_EQ(users_find_gid(account.gid, &group), USERS_OK);
        KT_CHECK_STR_EQ(group.name, "guest");
    }

    teardown();
}

/*
 * The property: a name and a password are checked against the two files, and
 * the act of `login` moves the uid, the gid and the directory of the task.
 */
static void test_accounts_act(void)
{
    struct task *self = task_self();
    struct user_account account;

    ktest_begin("users: a password opens a session and moves the task");

    setup();
    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(ksys_mkdir("/home"), 0);
    KT_CHECK_INT_EQ(ksys_mkdir("/home/guest"), 0);

    write_text(USERS_PASSWD_PATH, "root:0:0:/:/bin/sh\n"
                                  "guest:7:7:/home/guest:/bin/sh\n");
    write_text(USERS_SHADOW_PATH,
               "root:$sha256$mox1$"
               "da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c\n"
               "guest:*\n");

    /* The password of the file, and not another one. */
    KT_CHECK_INT_EQ(users_authenticate("root", "microos", &account), USERS_OK);
    KT_CHECK_STR_EQ(account.name, "root");
    KT_CHECK_INT_EQ(users_authenticate("root", "wrong", &account), USERS_EAUTH);
    KT_CHECK_INT_EQ(users_authenticate("root", "", &account), USERS_EAUTH);
    KT_CHECK_INT_EQ(users_authenticate("nobody", "microos", &account), USERS_ENOENT);
    /* guest has no password of its own: only root becomes guest. */
    KT_CHECK_INT_EQ(users_authenticate("guest", "", &account), USERS_EAUTH);
    KT_CHECK_INT_EQ(users_authenticate("guest", "*", &account), USERS_EAUTH);

    /* The act: the home directory first, then the two credentials together.
     * The gid moves with the uid, which is what `login` does and what
     * `setgid` after `setuid` could not do any more. */
    KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_OK);
    task_set_cwd(self, "/");
    KT_CHECK_INT_EQ(users_apply(&account, true), USERS_OK);
    KT_CHECK_INT_EQ(task_get_uid(self), 7);
    KT_CHECK_INT_EQ(task_get_gid(self), 7);
    KT_CHECK_STR_EQ(task_get_cwd(self), "/home/guest");

    /* A task of uid 7 is no longer root: the way back is not a call. */
    KT_CHECK_INT_EQ(ksys_setuid(0), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_setgid(0), KERR_PERM);

    /* Without the flag the directory does not move. */
    task_set_creds(self, 0, 0);
    KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_OK);
    KT_CHECK_INT_EQ(users_apply(&account, false), USERS_OK);
    KT_CHECK_INT_EQ(task_get_uid(self), 7);
    KT_CHECK_INT_EQ(task_get_gid(self), 7);
    KT_CHECK_STR_EQ(task_get_cwd(self), "/home/guest");

    /* A home that is not a directory is a warning and not a refusal: the
     * credentials still move, which is what a login with a lost home does. */
    task_set_creds(self, 0, 0);
    task_set_cwd(self, "/");
    KT_CHECK_INT_EQ(ksys_unlink(USERS_PASSWD_PATH), 0);
    write_text(USERS_PASSWD_PATH, "root:0:0:/nowhere/at/all:/bin/sh\n");
    KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_OK);
    KT_CHECK_INT_EQ(users_apply(&account, true), USERS_OK);
    KT_CHECK_STR_EQ(task_get_cwd(self), "/");

    teardown();
}

/*
 * The property: a boot with no file of accounts makes the two of the system,
 * and the hash of the file it writes is the hash of the password of the
 * record — the C half and the check of it agree without a second table.
 */
static void test_accounts_defaults(void)
{
    struct user_account account;
    struct vfs_stat info;

    ktest_begin("users: the default files of /etc are made when they are not there");

    setup();

    KT_CHECK_INT_EQ(users_defaults("/etc"), 3);
    KT_CHECK_INT_EQ(ksys_stat(USERS_PASSWD_PATH, &info), 0);
    KT_CHECK_INT_EQ(info.mode, 0644);
    KT_CHECK_INT_EQ(ksys_stat(USERS_SHADOW_PATH, &info), 0);
    KT_CHECK_INT_EQ(info.mode, 0600);
    KT_CHECK_INT_EQ(ksys_stat(USERS_GROUP_PATH, &info), 0);
    KT_CHECK_INT_EQ(info.mode, 0644);
    KT_CHECK_INT_EQ(ksys_stat("/home/guest", &info), 0);
    KT_CHECK_INT_EQ(info.type, 2);

    /* The file of a person is never replaced. */
    KT_CHECK_INT_EQ(users_defaults("/etc"), 0);

    KT_CHECK_INT_EQ(users_find_name("root", &account), USERS_OK);
    KT_CHECK_INT_EQ(account.uid, 0);
    KT_CHECK_STR_EQ(account.home, "/");
    KT_CHECK_INT_EQ(users_find_name("guest", &account), USERS_OK);
    KT_CHECK_INT_EQ(account.uid, 7);
    KT_CHECK_STR_EQ(account.home, "/home/guest");

    /* The password of the default record is the one the file was made with. */
    KT_CHECK_INT_EQ(users_authenticate("root", USERS_DEFAULT_PASSWORD, &account), USERS_OK);
    KT_CHECK_INT_EQ(users_authenticate("root", "not the password", &account), USERS_EAUTH);
    /* And the second account of the file has none. */
    KT_CHECK_INT_EQ(users_authenticate("guest", "", &account), USERS_EAUTH);

    teardown();
}

void test_users(void)
{
    test_creds_inherit();
    test_uid_permissions();
    test_directory_bits();
    test_group_permissions();
    test_three_uids();
    test_cwd_fit();
    test_privileged_acts();
    test_accounts_records();
    test_accounts_file();
    test_accounts_group();
    test_accounts_act();
    test_accounts_defaults();
}
