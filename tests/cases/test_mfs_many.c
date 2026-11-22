/*
 * Micro OS — a directory with more entries than one block of the chain.
 *
 * The cleanroom of 1.1.0 found that the two hundredth file of one directory
 * made every path of the volume unreadable: the directory grew to a new
 * block, and the first four bytes of that block (the header of its chain)
 * were the stale number of the file that had held the block before. The
 * chain then went in a circle and the filesystem gave an I/O error for /bin
 * and /etc as well. This test fills a directory past several blocks and
 * asks the check of the volume whether any chain is broken.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

#define MANY_BLOCKS 2048u
#define MANY_FILES 300

void test_mfs_many(void)
{
    struct mfs_ram_device volume;
    struct k_blockdev device;
    struct mfs_file file;
    struct mfs_check_report report;
    char path[32];
    char marker[8];
    int i;

    KT_CHECK_INT_EQ(mfs_ram_init(&volume, MANY_BLOCKS), 0);
    mfs_ram_device(&volume, &device);

    KT_CHECK_INT_EQ(mfs_format(&device, 256), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/many"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/keep"), MFS_OK);

    /* A file that must survive the growth of the directory beside it. */
    KT_CHECK_INT_EQ(mfs_create("/keep/marker"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/keep/marker", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "alive", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    for (i = 0; i < MANY_FILES; i++) {
        snprintf(path, sizeof(path), "/many/f%03d", i);
        KT_CHECK_INT_EQ(mfs_create(path), MFS_OK);
        KT_CHECK_INT_EQ(mfs_open(path, MFS_O_RDWR, &file), MFS_OK);
        KT_CHECK_INT_EQ(mfs_write(&file, "x", 1), 1);
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    KT_CHECK_INT_EQ(mfs_open("/keep/marker", MFS_O_RDONLY, &file), MFS_OK);
    memset(marker, 0, sizeof(marker));
    KT_CHECK_INT_EQ(mfs_read(&file, marker, sizeof(marker)), 5);
    KT_CHECK_STR_EQ(marker, "alive");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.files, MANY_FILES + 1u);

    mfs_unmount();
    mfs_ram_free(&volume);
}
