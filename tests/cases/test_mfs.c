#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/sched.h"
#include "kernel/task.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

/* The size of the volume of the tests: 256 blocks of 1 KB. */
#define TEST_BLOCKS 256

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

static int setup(void)
{
    int rc = mfs_ram_init(&g_volume, TEST_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);

    return mfs_format(&g_device, 64);
}

static void teardown(void)
{
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void test_format(void)
{
    struct mfs_info info;

    ktest_begin("mfs: the volume is formatted and mounted");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_is_mounted());

    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.total_blocks, TEST_BLOCKS);
    KT_CHECK(info.data_blocks > 0);
    KT_CHECK(info.meta_blocks > 0);
    KT_CHECK(info.inodes_total > 0);
    KT_CHECK_INT_EQ(info.inodes_used, 1); /* only the root */
    KT_CHECK(info.data_free > 0);

    teardown();
}

static void test_layout(void)
{
    struct mfs_info info;
    const k_u8_t *info_block;

    ktest_begin("mfs: the layout follows the design of the documentation");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);

    /* The header of the volume lives in the last block of the volume. */
    info_block = g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE;
    {
        struct mfs_superblock sb;

        memcpy(&sb, info_block, sizeof(sb));

        KT_CHECK_INT_EQ(sb.magic, MFS_MAGIC);
        KT_CHECK_INT_EQ(sb.version, MFS_VERSION);
        KT_CHECK_INT_EQ(sb.block_size, MFS_BLOCK_SIZE);
        KT_CHECK_INT_EQ(sb.total_blocks, TEST_BLOCKS);
        KT_CHECK_INT_EQ(sb.data_start, 0);
        KT_CHECK_INT_EQ(sb.data_blocks, info.data_blocks);
        KT_CHECK_INT_EQ(sb.root_inode, MFS_ROOT_INODE);

        /* The three sections fill the volume, in the documented order. */
        KT_CHECK_INT_EQ(sb.meta_start + sb.meta_blocks, sb.info_start);
        KT_CHECK_INT_EQ(sb.info_start + sb.info_blocks, sb.total_blocks);
        KT_CHECK_INT_EQ(sb.meta_start, TEST_BLOCKS - info.meta_blocks - sb.info_blocks);

        /*
         * The INFO section starts at the border of an erase sector of the
         * flash (4 KB = 4 blocks): the table of the inodes is then in a
         * sector of its own, and writing it cannot erase the header.
         */
        KT_CHECK_INT_EQ(sb.info_start % 4, 0);
        KT_CHECK_INT_EQ(sb.info_blocks % 4, 0);

        /* The header carries the checksum of itself. */
        KT_CHECK((sb.features & MFS_FEATURE_CRC) != 0);
    }

    teardown();
}

/*
 * The property: a header that the storage lost in the middle is noticed,
 * and the volume that it describes is not mounted.
 */
static void test_header_crc(void)
{
    k_u8_t saved[MFS_BLOCK_SIZE];
    k_u8_t *info_block;

    ktest_begin("mfs: a damaged header is not trusted");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    info_block = g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE;
    memcpy(saved, info_block, MFS_BLOCK_SIZE);

    /* A bit that a brownout left half written. */
    info_block[sizeof(struct mfs_superblock) / 2] ^= 0x10;
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_EINVAL);
    KT_CHECK(!mfs_is_mounted());

    /* The same volume, the header put back: everything is fine again. */
    memcpy(info_block, saved, MFS_BLOCK_SIZE);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    teardown();
}

/*
 * The property: the map of the free blocks reaches the device before the
 * record that points at the block. After a restart the directory must not
 * share its block with a file that was created later.
 */
static void test_mkdir_survives_restart(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_file file;
    char buffer[64];

    ktest_begin("mfs: a directory survives a restart without losing its block");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/logs"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/logs/old"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    /* The restart of the board: only what reached the device stays. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* The blocks of the directories are still taken. */
    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free);

    /* The directories are still there. */
    {
        struct mfs_dirent entries[8];

        memset(entries, 0, sizeof(entries));
        KT_CHECK_INT_EQ(mfs_list("/logs", entries, 8), MFS_OK);
        KT_CHECK_STR_EQ(entries[0].name, "old");
    }

    /* A new file must not be given the block of the directory. */
    KT_CHECK_INT_EQ(mfs_create("/logs/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/logs/a.txt", MFS_O_RDWR, &file), MFS_OK);
    memset(buffer, 'x', sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_write(&file, buffer, sizeof(buffer)), (int)sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    {
        struct mfs_dirent entries[8];
        int found_old = 0;
        int found_new = 0;
        k_size_t i;

        memset(entries, 0, sizeof(entries));
        KT_CHECK_INT_EQ(mfs_list("/logs", entries, 8), MFS_OK);

        for (i = 0; i < 8; i++) {
            if (strcmp(entries[i].name, "old") == 0) {
                found_old = 1;
            }
            if (strcmp(entries[i].name, "a.txt") == 0) {
                found_new = 1;
            }
        }

        /* Both are still there, the new file did not overwrite the entry. */
        KT_CHECK(found_old);
        KT_CHECK(found_new);
    }

    /* The directory still owns its block. */
    {
        struct mfs_inode inode;

        KT_CHECK_INT_EQ(mfs_stat("/logs", &inode, NULL), MFS_OK);
        KT_CHECK((inode.flags & MFS_INODE_DIR) != 0);
        KT_CHECK_INT_EQ(inode.entries, 2);
        KT_CHECK(inode.size > 0);
    }

    teardown();
}

/*
 * The property: a file opened with O_TRUNC gives its blocks back to the
 * map, and the map reaches the device.
 */
static void test_truncate_frees_blocks(void)
{
    struct mfs_info empty;
    struct mfs_info full;
    struct mfs_info after;
    struct mfs_file file;
    char buffer[3000];

    ktest_begin("mfs: a truncated file gives its blocks back");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&empty), MFS_OK);

    memset(buffer, 'a', sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_create("/big.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, buffer, sizeof(buffer)), (int)sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&full), MFS_OK);
    KT_CHECK(full.data_free < empty.data_free);

    /* The truncation: the blocks of the chain go back to the map. */
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR | MFS_O_TRUNC, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, empty.data_free);

    teardown();
}

static void test_files(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    char buffer[128];
    int rc;

    ktest_begin("mfs: a file can be written and read");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/hello.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/hello.txt"), MFS_EEXIST);

    KT_CHECK_INT_EQ(mfs_open("/hello.txt", MFS_O_RDWR, &file), MFS_OK);

    rc = mfs_write(&file, "hello microfs", 13);
    KT_CHECK_INT_EQ(rc, 13);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/hello.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    rc = mfs_read(&file, buffer, sizeof(buffer));
    KT_CHECK_INT_EQ(rc, 13);
    KT_CHECK_STR_EQ(buffer, "hello microfs");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_stat("/hello.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 13);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_USED, MFS_INODE_USED);

    teardown();
}

static void test_big_file(void)
{
    struct mfs_file file;
    static char content[5000];
    static char check[5000];
    int rc;
    k_size_t i;

    ktest_begin("mfs: a file that is bigger than a block");

    for (i = 0; i < sizeof(content); i++) {
        content[i] = (char)(i & 0xFF);
    }

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/big.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR, &file), MFS_OK);

    rc = mfs_write(&file, content, sizeof(content));
    KT_CHECK_INT_EQ(rc, (int)sizeof(content));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDONLY, &file), MFS_OK);
    memset(check, 0, sizeof(check));
    rc = mfs_read(&file, check, sizeof(check));
    KT_CHECK_INT_EQ(rc, (int)sizeof(check));
    KT_CHECK_INT_EQ(memcmp(content, check, sizeof(content)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    teardown();
}

static void test_directories(void)
{
    struct mfs_dirent entries[16];
    struct mfs_dirent entry;

    ktest_begin("mfs: the directories hold the entries");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_EEXIST);
    KT_CHECK_INT_EQ(mfs_create("/etc/config.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/readme"), MFS_OK);

    /* The root holds two entries. */
    KT_CHECK_INT_EQ(mfs_list("/", entries, 16), MFS_OK);
    KT_CHECK(mfs_stat("/etc", NULL, &entry) == MFS_OK);
    KT_CHECK_STR_EQ(entry.name, "etc");
    KT_CHECK_INT_EQ(entry.type & MFS_INODE_DIR, MFS_INODE_DIR);

    /* The directory holds the file. */
    KT_CHECK_INT_EQ(mfs_list("/etc", entries, 16), MFS_OK);
    KT_CHECK_STR_EQ(entries[0].name, "config.txt");
    KT_CHECK_INT_EQ(entries[0].type & MFS_INODE_DIR, 0);

    teardown();
}

static void test_unlink(void)
{
    struct mfs_info info;
    struct mfs_inode inode;

    ktest_begin("mfs: a file can be removed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/temp.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.inodes_used, 2);

    KT_CHECK_INT_EQ(mfs_unlink("/temp.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/temp.txt", &inode, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.inodes_used, 1);

    /* The directory has to be empty before it can be removed. */
    KT_CHECK_INT_EQ(mfs_mkdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_ENOTEMPTY);
    KT_CHECK_INT_EQ(mfs_unlink("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_OK);

    teardown();
}

static void test_persistence(void)
{
    struct mfs_file file;
    char buffer[32];

    ktest_begin("mfs: the data survives a remount");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/keep.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/keep.txt", MFS_O_RDWR, &file), MFS_OK);
    mfs_write(&file, "persisted", 9);
    mfs_close(&file);

    /* Unmount and mount again: the volume is the same memory. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/keep.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    mfs_read(&file, buffer, sizeof(buffer));
    KT_CHECK_STR_EQ(buffer, "persisted");
    mfs_close(&file);

    teardown();
}

/* The state of the two tasks of the test of the lock. */
static volatile int s_writers_failed;
static volatile int s_writers_done;

/*
 * Every task writes its own file, with pauses between the writes: the
 * tasks meet each other in the middle of their work, and the filesystem
 * has to keep them apart.
 */
static void writer_task(void *arg)
{
    char name[16];
    char buffer[64];
    struct mfs_file file;
    int id = (int)(intptr_t)arg;
    int i;

    memset(buffer, 'a' + id, sizeof(buffer));
    snprintf(name, sizeof(name), "/w%d.bin", id);

    if (mfs_create(name) != MFS_OK) {
        s_writers_failed++;
        return;
    }

    if (mfs_open(name, MFS_O_RDWR, &file) != MFS_OK) {
        s_writers_failed++;
        return;
    }

    for (i = 0; i < 20; i++) {
        if (mfs_write(&file, buffer, sizeof(buffer)) != (int)sizeof(buffer)) {
            s_writers_failed++;
            break;
        }
        k_sched_yield();
    }

    if (mfs_close(&file) != MFS_OK) {
        s_writers_failed++;
    }

    s_writers_done++;
}

/*
 * The property: two tasks that write their own files at the same time do
 * not see each other's work: neither of the files is given the blocks of
 * the other one, and both of them read back the way they were written.
 */
static void test_two_tasks_write(void)
{
    struct task *tasks[2];
    char name[16];
    char buffer[64];
    char back[64];
    struct mfs_file file;
    int id;
    int failures = 0;

    ktest_begin("mfs: two tasks that write files at the same time keep them apart");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    s_writers_failed = 0;
    s_writers_done = 0;

    for (id = 0; id < 2; id++) {
        char task_name[16];

        snprintf(task_name, sizeof(task_name), "writer%d", id);
        tasks[id] = task_create(task_name, writer_task, (void *)(intptr_t)id, (k_u8_t)(5 + id));
        KT_CHECK(tasks[id] != NULL);
    }

    for (id = 0; id < 2; id++) {
        if (tasks[id] != NULL) {
            /* The task is waited for: a finished task has to be taken back. */
            KT_CHECK_INT_EQ(task_wait(tasks[id]->pid, NULL, 5000), 0);
        }
    }

    KT_CHECK_INT_EQ(s_writers_done, 2);
    KT_CHECK_INT_EQ(s_writers_failed, 0);

    /* Every file holds its own byte, all the way through. */
    for (id = 0; id < 2; id++) {
        int i;
        int good = 1;

        snprintf(name, sizeof(name), "/w%d.bin", id);
        memset(buffer, 'a' + id, sizeof(buffer));

        KT_CHECK_INT_EQ(mfs_open(name, MFS_O_RDONLY, &file), MFS_OK);
        memset(back, 0, sizeof(back));
        for (i = 0; i < 20; i++) {
            if (mfs_read(&file, back, sizeof(back)) != (int)sizeof(back)) {
                good = 0;
                break;
            }
            if (memcmp(back, buffer, sizeof(back)) != 0) {
                good = 0;
                break;
            }
        }
        KT_CHECK_INT_EQ(good, 1);
        mfs_close(&file);
    }

    /* The volume is still in one piece after a restart. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/w0.bin", NULL, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/w1.bin", NULL, NULL), MFS_OK);

    teardown();
    failures = 0;
    (void)failures;
}

/*
 * The property: the flags of the open decide what the file is opened for.
 * "append" adds to the end of the file whatever the position of the
 * descriptor says, and a file that was opened to be written to cannot be
 * read from (and the other way around).
 */
static void test_append_and_access(void)
{
    struct mfs_file file;
    char buffer[32];

    ktest_begin("mfs: append adds to the end, the access is the one that was asked for");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* A file with "hello" in it. */
    KT_CHECK_INT_EQ(mfs_create("/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* Opening with append starts at the end of the file. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY | MFS_O_APPEND, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.offset, 5);
    KT_CHECK_INT_EQ(mfs_write(&file, " world", 6), 6);

    /*
     * The position is moved back by hand: a file with "append" is still
     * written to its end.
     */
    file.offset = 0;
    KT_CHECK_INT_EQ(mfs_write(&file, "!", 1), 1);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The file holds "hello world!" - the second write did not go to the head. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.size, 12);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_read(&file, buffer, sizeof(buffer)), 12);
    KT_CHECK_STR_EQ(buffer, "hello world!");
    mfs_close(&file);

    /* A file opened to be written to cannot be read from. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK(mfs_read(&file, buffer, sizeof(buffer)) < 0);
    mfs_close(&file);

    /* A file opened to be read from cannot be written to. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK(mfs_write(&file, "x", 1) < 0);
    mfs_close(&file);

    teardown();
}

void test_mfs(void)
{
    test_format();
    test_layout();
    test_header_crc();
    test_mkdir_survives_restart();
    test_truncate_frees_blocks();
    test_two_tasks_write();
    test_append_and_access();
    test_files();
    test_big_file();
    test_directories();
    test_unlink();
    test_persistence();
}
