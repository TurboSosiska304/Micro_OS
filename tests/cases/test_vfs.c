#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "devfs/devfs.h"
#include "fs_fat32/fat.h"
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "procfs/procfs.h"

#define TEST_BLOCKS 128

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static void setup(void)
{
    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 32);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void test_mount(void)
{
    ktest_begin("vfs: a filesystem is mounted and the paths are resolved");

    setup();

    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_STR_EQ(vfs_mount_name(0), "/");

    teardown();
}

static void test_file_operations(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    char buffer[64];
    int rc;

    ktest_begin("vfs: a file is created, written and read");

    setup();

    /* A path below the mount point has to be found. */
    rc = vfs_open("/hello.txt", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, file_data);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "hello vfs", 9), 9);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    rc = vfs_open("/hello.txt", VFS_O_RDONLY, &file, file_data);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        memset(buffer, 0, sizeof(buffer));
        KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 9);
        KT_CHECK_STR_EQ(buffer, "hello vfs");
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    teardown();
}

static void test_directories(void)
{
    struct vfs_dirent entries[16];
    struct vfs_stat stat;
    k_size_t count = 0;
    int rc;

    ktest_begin("vfs: the directories are listed");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/etc", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2); /* a directory */

    rc = vfs_list("/", entries, 16, &count);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(count, 1);
    if (count == 1) {
        KT_CHECK_STR_EQ(entries[0].name, "etc");
        KT_CHECK_INT_EQ(entries[0].type, 2);
    }

    KT_CHECK_INT_EQ(vfs_rmdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_list("/", entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 0);

    /*
     * The codes of the errors of the filesystem are translated into the
     * codes of the contract of the system calls at the adapter. The two
     * sets are close but not the same: "the directory is not empty" is
     * -39 of the kernel, and -9 of the kernel is a "bad file descriptor".
     */
    KT_CHECK_INT_EQ(vfs_mkdir("/full"), 0);
    KT_CHECK_INT_EQ(mfs_create("/full/file.txt"), MFS_OK);
    KT_CHECK_INT_EQ(vfs_unlink("/full"), KERR_ISDIR);
    KT_CHECK_INT_EQ(vfs_rmdir("/full"), KERR_NOTEMPTY);
    KT_CHECK_INT_EQ(vfs_rmdir("/nothing-here"), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_unlink("/nothing-here"), KERR_NOENT);

    teardown();
}

static void test_mkdir_p(void)
{
    struct vfs_stat stat;

    ktest_begin("vfs: a directory and its parents are created at once");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc/init.d"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/etc", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2); /* a directory */
    KT_CHECK_INT_EQ(vfs_stat("/etc/init.d", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    /* A directory that is already there is not an error. */
    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc/init.d"), 0);

    teardown();
}

static void test_mount_points(void)
{
    struct vfs_dirent entries[16];
    k_size_t count = 0;
    k_size_t i;
    int saw_proc = 0;
    int saw_dev = 0;

    ktest_begin("vfs: the filesystems mounted below a directory are listed in it");

    setup();
    KT_CHECK_INT_EQ(procfs_register("/proc"), 0);
    KT_CHECK_INT_EQ(devfs_register("/dev"), 0);

    /* The root knows nothing of the mounts: the VFS shows them to it. */
    KT_CHECK_INT_EQ(vfs_list("/", entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 2);
    for (i = 0; i < count; i++) {
        KT_CHECK_INT_EQ(entries[i].type, 2); /* a directory */
        if (strcmp(entries[i].name, "proc") == 0) {
            saw_proc = 1;
        }
        if (strcmp(entries[i].name, "dev") == 0) {
            saw_dev = 1;
        }
    }
    KT_CHECK(saw_proc);
    KT_CHECK(saw_dev);

    /* A mount below a subdirectory is an entry of that directory too. */
    KT_CHECK_INT_EQ(vfs_mkdir("/mnt"), 0);
    KT_CHECK_INT_EQ(devfs_register("/mnt/data"), 0);
    KT_CHECK_INT_EQ(vfs_list("/mnt", entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 1);
    if (count == 1) {
        KT_CHECK_STR_EQ(entries[0].name, "data");
        KT_CHECK_INT_EQ(entries[0].type, 2);
    }

    KT_CHECK_INT_EQ(vfs_umount("/mnt/data"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/proc"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/dev"), 0);
    teardown();
}

/*
 * The property: the mode and the owner of an object are the numbers the
 * filesystem keeps, they travel through the VFS, and chmod and chown are the
 * two calls that change them - one without the other.
 */
static void test_attributes(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    struct vfs_stat stat;

    ktest_begin("vfs: the mode and the owner travel through the VFS");

    setup();

    KT_CHECK_INT_EQ(vfs_open("/tool", VFS_O_RDWR | VFS_O_CREAT, &file, file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* A new file has the mode of the design and the uid of the one user. */
    KT_CHECK_INT_EQ(vfs_stat("/tool", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(stat.owner, 0);
    KT_CHECK_INT_EQ(stat.type, 1); /* a file */

    KT_CHECK_INT_EQ(vfs_chmod("/tool", 0755), 0);
    KT_CHECK_INT_EQ(vfs_stat("/tool", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0755);
    KT_CHECK_INT_EQ(stat.type, 1); /* the type did not move with the mode */

    KT_CHECK_INT_EQ(vfs_chown("/tool", 7), 0);
    KT_CHECK_INT_EQ(vfs_stat("/tool", &stat), 0);
    KT_CHECK_INT_EQ(stat.owner, 7);
    KT_CHECK_INT_EQ(stat.mode, 0755); /* and the mode did not move with it */

    /* A directory is born with the mode of a directory. */
    KT_CHECK_INT_EQ(vfs_mkdir("/bin"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/bin", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, MFS_MODE_DIR_DEFAULT);

    /* The numbers are checked where the two contracts meet. */
    KT_CHECK_INT_EQ(vfs_chmod("/tool", MFS_MODE_MASK + 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_chown("/tool", MFS_OWNER_MASK + 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_chmod("/not-here", 0644), KERR_NOENT);

    teardown();
}

static void test_statfs(void)
{
    struct vfs_statfs stat;

    ktest_begin("vfs: the volume reports its size");

    setup();

    KT_CHECK_INT_EQ(vfs_statfs("/", &stat), 0);
    KT_CHECK(stat.total_blocks > 0);
    KT_CHECK(stat.free_blocks > 0);
    KT_CHECK(stat.free_blocks < stat.total_blocks);
    KT_CHECK_INT_EQ(stat.block_size, MFS_BLOCK_SIZE);

    teardown();
}

/*
 * A filesystem that keeps no modes, for the walk through a directory of the
 * mode zero. procfs and devfs are the real ones, but neither of them has the
 * four calls that create, remove or change an object, so the test keeps its
 * own small filesystem: every object is a directory without a mode, and the
 * calls succeed, which is what makes the walk observable.
 */
static int no_modes_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    K_UNUSED(ctx);
    K_UNUSED(path);

    memset(out, 0, sizeof(*out));
    out->type = 2; /* a directory */
    out->mode = 0; /* the filesystem keeps no modes */
    out->owner = 0;

    return 0;
}

static int no_modes_mkdir(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    K_UNUSED(path);

    return 0;
}

static int no_modes_unlink(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    K_UNUSED(path);

    return 0;
}

static int no_modes_chmod(void *ctx, const char *path, k_u32_t mode)
{
    K_UNUSED(ctx);
    K_UNUSED(path);
    K_UNUSED(mode);

    return 0;
}

static int no_modes_chown(void *ctx, const char *path, k_u32_t owner)
{
    K_UNUSED(ctx);
    K_UNUSED(path);
    K_UNUSED(owner);

    return 0;
}

static const struct vfs_operations g_no_modes_ops = {
    .stat = no_modes_stat,
    .mkdir = no_modes_mkdir,
    .unlink = no_modes_unlink,
    .chmod = no_modes_chmod,
    .chown = no_modes_chown,
};

/*
 * The property: every directory of a path has to be searchable by the
 * caller. The VFS checks the `x` bit of the directories it passes through
 * before it asks the filesystem for the object at the end, and a call that
 * creates, removes or changes an object passes through the parent like any
 * other call.
 */
static void test_directory_search(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    struct vfs_stat stat;

    ktest_begin("vfs: a path is searched through the x bit of its directories");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/locked"), 0);
    KT_CHECK_INT_EQ(vfs_open("/locked/inside.txt", VFS_O_RDWR | VFS_O_CREAT, &file, file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /*
     * Read and write for the owner but no search: the child is hidden. Root
     * passes the search, so the directory is given to uid 7 and the runner
     * walks it as that user.
     */
    KT_CHECK_INT_EQ(vfs_chown("/locked", 7), 0);
    KT_CHECK_INT_EQ(vfs_chown("/locked/inside.txt", 7), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/locked", 0600), 0);
    task_set_uid(task_self(), 7);

    KT_CHECK_INT_EQ(vfs_open("/locked/inside.txt", VFS_O_RDONLY, &file, file_data), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_stat("/locked/inside.txt", &stat), KERR_PERM);

    /* The parent is walked for the calls that make, remove or change an
     * object of it, so a directory without the search bit keeps them out. */
    KT_CHECK_INT_EQ(vfs_mkdir("/locked/newdir"), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_unlink("/locked/inside.txt"), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_rmdir("/locked/newdir"), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_chmod("/locked/inside.txt", 0644), KERR_PERM);
    KT_CHECK_INT_EQ(vfs_chown("/locked/inside.txt", 7), KERR_PERM);

    /* The directory itself is not passed through: it is still described. */
    KT_CHECK_INT_EQ(vfs_stat("/locked", &stat), 0);

    /* Search for the owner: the child is reachable again, and the parent of
     * a new object is reached as well. The owner changes the mode of what it
     * owns; giving the object away stays the act of root. */
    KT_CHECK_INT_EQ(vfs_chmod("/locked", 0700), 0);
    KT_CHECK_INT_EQ(vfs_open("/locked/inside.txt", VFS_O_RDONLY, &file, file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_stat("/locked/inside.txt", &stat), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/locked/newdir"), 0);
    KT_CHECK_INT_EQ(vfs_rmdir("/locked/newdir"), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/locked/inside.txt", 0644), 0);
    KT_CHECK_INT_EQ(vfs_chown("/locked/inside.txt", 7), KERR_PERM);

    /* Root gives it away, and the record is the one that was written. */
    task_set_uid(task_self(), 0);
    KT_CHECK_INT_EQ(vfs_chown("/locked/inside.txt", 7), 0);
    KT_CHECK_INT_EQ(vfs_stat("/locked/inside.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0644);
    KT_CHECK_INT_EQ(stat.owner, 7);

    /* The object at the end keeps the check it always had: the walk lets the
     * path through, and the name, the existence and the record are still the
     * answers of the filesystem. */
    KT_CHECK_INT_EQ(vfs_mkdir("/locked"), KERR_EXIST);
    KT_CHECK_INT_EQ(vfs_unlink("/locked/nothing"), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_chmod("/locked/nothing", 0644), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_chown("/locked/nothing", 7), KERR_NOENT);

    /* The root is passed through by every path and works as before. */
    KT_CHECK_INT_EQ(vfs_mkdir("/top"), 0);
    KT_CHECK_INT_EQ(vfs_rmdir("/top"), 0);

    /*
     * A filesystem that keeps no modes is walked the way it always was: a
     * zero is not "nobody may search it", and the four calls reach the
     * objects below such a directory.
     */
    KT_CHECK_INT_EQ(vfs_mkdir("/virt"), 0);
    KT_CHECK_INT_EQ(vfs_mount("/virt", &g_no_modes_ops, NULL), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/virt/dir"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/virt/dir/deep"), 0);
    KT_CHECK_INT_EQ(vfs_unlink("/virt/dir/deep"), 0);
    KT_CHECK_INT_EQ(vfs_chmod("/virt/dir", 0644), 0);
    KT_CHECK_INT_EQ(vfs_chown("/virt/dir", 7), 0);
    KT_CHECK_INT_EQ(vfs_umount("/virt"), 0);

    teardown();
}

/*
 * The property: a directory is opened with VFS_O_DIRECTORY and read through
 * the descriptor, whose position goes from one call to the next; the end of
 * the directory is an empty answer, and a plain open of a directory is still
 * EISDIR.
 */
static void test_directory_descriptor(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    struct vfs_dirent entries[8];
    k_size_t count = 0;
    k_size_t i;
    int saw_etc = 0;
    char byte = 'x';
    int rc;

    ktest_begin("vfs: a directory is read through its descriptor");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);

    /* Without the flag a directory is not an open file. */
    KT_CHECK_INT_EQ(vfs_open("/", VFS_O_RDONLY, &file, file_data), KERR_ISDIR);

    rc = vfs_open("/", VFS_O_RDONLY | VFS_O_DIRECTORY, &file, file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 8, &count), 0);
        KT_CHECK_INT_EQ(count, 1);
        for (i = 0; i < count; i++) {
            if (strcmp(entries[i].name, "etc") == 0 && entries[i].type == 2) {
                saw_etc = 1;
            }
        }
        KT_CHECK(saw_etc);

        /* A directory is read through readdir, not as a stream of bytes. */
        KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), KERR_ISDIR);
        KT_CHECK_INT_EQ(vfs_write(&file, &byte, 1), KERR_ISDIR);

        /* The position is in the descriptor: the end is empty. */
        KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 8, &count), 0);
        KT_CHECK_INT_EQ(count, 0);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    /* A descriptor of a file has no entries to give. */
    KT_CHECK_INT_EQ(vfs_open("/plain.txt", VFS_O_RDWR | VFS_O_CREAT, &file, file_data), 0);
    KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 8, &count), KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

/* --- the types, the runtime mounts and the table of the boot --- */

/* A type of the test that mounts a filesystem with no write: devfs, which
 * has enough objects to be read through the command of the shell. */
static int test_devfs_mount(const char *point, const char *device)
{
    K_UNUSED(device);

    return devfs_register(point);
}

static int write_text(const char *path, const char *text)
{
    struct vfs_file file;
    k_u8_t data[VFS_FILE_DATA_SIZE];
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
 * A FAT12 volume of the memory, built byte by byte: one boot sector, two
 * tables of clusters, a fixed root and one file in the first cluster of the
 * data. It is 256 KB, which is the smallest kind of a medium the reader can
 * be pointed at without a disk of the machine.
 */
#define FAT_VFS_SECTOR 512u
#define FAT_VFS_SECTORS 512u
#define FAT_VFS_RESERVED 1u
#define FAT_VFS_FATS 2u
#define FAT_VFS_FAT_SECTORS 2u
#define FAT_VFS_ROOT_ENTRIES 32u
#define FAT_VFS_ROOT_START (FAT_VFS_RESERVED + FAT_VFS_FATS * FAT_VFS_FAT_SECTORS)
#define FAT_VFS_DATA_START (FAT_VFS_ROOT_START + 2u)

static k_u8_t g_fat_image[FAT_VFS_SECTORS * FAT_VFS_SECTOR];

static void fat_put16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

static void fat_put32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
    p[2] = (k_u8_t)((value >> 16) & 0xFFu);
    p[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* One entry of a table of FAT12: the twelve bits of a cluster are shared by
 * one and a half bytes, and the entry takes the half it does not own. */
static void fat_set12(k_u8_t *fat, k_u32_t index, k_u16_t value)
{
    k_u32_t offset = index + index / 2u;

    if ((index & 1u) != 0u) {
        fat[offset] = (k_u8_t)((fat[offset] & 0x0Fu) | ((value << 4) & 0xF0u));
        fat[offset + 1] = (k_u8_t)(value >> 4);
    } else {
        fat[offset] = (k_u8_t)(value & 0xFFu);
        fat[offset + 1] = (k_u8_t)((fat[offset + 1] & 0xF0u) | ((value >> 8) & 0x0Fu));
    }
}

static void fat_image_build(void)
{
    k_u8_t *boot = g_fat_image;
    k_u8_t *fat = g_fat_image + FAT_VFS_RESERVED * FAT_VFS_SECTOR;
    k_u8_t *root = g_fat_image + FAT_VFS_ROOT_START * FAT_VFS_SECTOR;
    k_u8_t *data = g_fat_image + FAT_VFS_DATA_START * FAT_VFS_SECTOR;

    memset(g_fat_image, 0, sizeof(g_fat_image));

    boot[0] = 0xEB;
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "MICROOS ", 8);
    fat_put16(boot + 11, FAT_VFS_SECTOR);
    boot[13] = 1; /* one sector per cluster */
    fat_put16(boot + 14, FAT_VFS_RESERVED);
    boot[16] = FAT_VFS_FATS;
    fat_put16(boot + 17, FAT_VFS_ROOT_ENTRIES);
    fat_put16(boot + 19, FAT_VFS_SECTORS);
    boot[21] = 0xF8; /* the media of a fixed disk */
    fat_put16(boot + 22, FAT_VFS_FAT_SECTORS);
    fat_put16(boot + 24, 32); /* sectors per track */
    fat_put16(boot + 26, 64); /* heads */
    boot[36] = 0x80;          /* drive */
    boot[38] = 0x29;          /* the extended signature */
    fat_put32(boot + 39, 0x12345678u);
    memcpy(boot + 43, "MICROOS    ", 11);
    memcpy(boot + 54, "FAT12   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    /* The first two entries are the media and the end of the chain; the
     * file lives in the cluster 2, which is the last of its chain. */
    fat_set12(fat, 0, 0x0FF8u);
    fat_set12(fat, 1, 0x0FFFu);
    fat_set12(fat, 2, 0x0FFFu);

    memcpy(root, "HELLO   TXT", 11);
    root[11] = 0x20; /* a file */
    fat_put16(root + 26, 2);
    fat_put32(root + 28, 4);

    memcpy(data, "fat\n", 4);
}

static int fat_test_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= FAT_VFS_SECTORS || count > FAT_VFS_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, g_fat_image + (size_t)sector * FAT_VFS_SECTOR, (size_t)count * FAT_VFS_SECTOR);

    return 0;
}

static int fat_test_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(sector);
    K_UNUSED(buf);
    K_UNUSED(count);

    /* The reader never writes: a test that comes here is a bug. */
    return -1;
}

static const struct k_blockdev_ops g_fat_test_ops = {
    .read = fat_test_read,
    .write = fat_test_write,
    .erase = NULL,
    .sync = NULL,
};

static struct k_blockdev g_fat_test_device = {
    .name = "vfsfat0",
    .sector_size = FAT_VFS_SECTOR,
    .sectors = FAT_VFS_SECTORS,
    .ops = &g_fat_test_ops,
    .ctx = NULL,
};

/*
 * The property: a type is a name in a table of the VFS, and the table is
 * searched by the name a person types.
 */
static void test_type_registry(void)
{
    k_size_t before = vfs_type_count();

    ktest_begin("vfs: a type is registered, found by its name and forgotten");

    KT_CHECK_INT_EQ(vfs_register_type("mock", test_devfs_mount), 0);
    KT_CHECK_INT_EQ(vfs_type_count(), before + 1);
    KT_CHECK_INT_EQ(vfs_type_find("mock"), (int)before);
    KT_CHECK_INT_EQ(vfs_type_find("nothing"), -1);
    KT_CHECK_INT_EQ(vfs_type_find(NULL), -1);
    KT_CHECK_STR_EQ(vfs_type_name(before), "mock");

    /* The same name keeps its slot and takes the call again. */
    KT_CHECK_INT_EQ(vfs_register_type("mock", test_devfs_mount), 0);
    KT_CHECK_INT_EQ(vfs_type_count(), before + 1);

    KT_CHECK_INT_EQ(vfs_unregister_type("mock"), 0);
    KT_CHECK_INT_EQ(vfs_type_find("mock"), -1);
    KT_CHECK_INT_EQ(vfs_type_count(), before);
}

/*
 * The property: MicroFS and FAT are two types of one tree. The root is
 * mounted by the name of its type, a FAT volume is mounted below it, and
 * both answer through the same VFS; the unmount of the FAT volume leaves
 * the point it was mounted on empty again.
 */
static void test_mount_by_type(void)
{
    struct mfs_ram_device volume;
    struct k_blockdev device;
    struct vfs_file file;
    k_u8_t data[VFS_FILE_DATA_SIZE];
    struct vfs_stat stat;
    char buffer[8];

    ktest_begin("vfs: MicroFS and FAT are mounted together by their types");

    KT_CHECK_INT_EQ(mfs_ram_init(&volume, TEST_BLOCKS), 0);
    mfs_ram_device(&volume, &device);
    KT_CHECK_INT_EQ(mfs_format(&device, 32), MFS_OK);
    KT_CHECK_INT_EQ(k_blockdev_register(&device), 0);
    KT_CHECK_INT_EQ(mfs_vfs_register_type(), 0);

    KT_CHECK_INT_EQ(vfs_mount_type("/", "mfs", "ram0"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_STR_EQ(vfs_mount_name(0), "/");

    KT_CHECK_INT_EQ(vfs_mkdir("/mnt"), 0);
    KT_CHECK_INT_EQ(write_text("/mnt/hello.txt", "root"), 0);

    /* The second volume of the tree, a FAT image of the memory. */
    fat_image_build();
    KT_CHECK_INT_EQ(fat_vfs_register_type(), 0);
    KT_CHECK_INT_EQ(k_blockdev_register(&g_fat_test_device), 0);
    KT_CHECK_INT_EQ(vfs_mount_type("/mnt/fat", "fat32", "vfsfat0"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);
    KT_CHECK_STR_EQ(vfs_mount_name(1), "/mnt/fat");

    KT_CHECK_INT_EQ(vfs_stat("/mnt/fat", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2); /* a directory */
    KT_CHECK_INT_EQ(vfs_stat("/mnt/fat/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 1); /* a file */
    KT_CHECK_INT_EQ(stat.size, 4);

    /* The file of the root is still there: the mounts are two at a time. */
    KT_CHECK_INT_EQ(vfs_open("/mnt/hello.txt", VFS_O_RDONLY, &file, data), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 4);
    KT_CHECK_STR_EQ(buffer, "root");
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* After the unmount the point is what the root says: no such entry. */
    KT_CHECK_INT_EQ(vfs_umount("/mnt/fat"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/fat", &stat), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_umount("/"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 0);
    KT_CHECK_INT_EQ(vfs_unregister_type("fat32"), 0);
    KT_CHECK_INT_EQ(vfs_unregister_type("mfs"), 0);
    KT_CHECK_INT_EQ(k_blockdev_unregister(&g_fat_test_device), 0);
    KT_CHECK_INT_EQ(k_blockdev_unregister(&device), 0);
    mfs_unmount();
    mfs_ram_free(&volume);
}

/* A second set of operations, for the point that two filesystems want. */
static const struct vfs_operations g_other_ops = {
    .stat = no_modes_stat,
};

/* A filesystem that refuses to leave until the test lets it. */
static bool g_refusing_leave;

static int refusing_umount(void *ctx)
{
    K_UNUSED(ctx);

    return g_refusing_leave ? 0 : KERR_NOTSUP;
}

static const struct vfs_operations g_refusing_ops = {
    .stat = no_modes_stat,
    .umount = refusing_umount,
};

/*
 * The property: the VFS refuses a point that is not a path of the tree, a
 * point that hides a file, a second filesystem at one point, and the
 * unmount of a directory with a mount below it; the root may leave while
 * the mounts that do not live on its volume stay.
 */
static void test_mount_errors(void)
{
    struct vfs_stat stat;

    ktest_begin("vfs: a mount is refused with the reason of the point");

    setup();
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    /* The point is an absolute path with no empty part. */
    KT_CHECK_INT_EQ(vfs_mount("relative", &g_no_modes_ops, NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount("", &g_no_modes_ops, NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount("/trailing/", &g_no_modes_ops, NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount("//double", &g_no_modes_ops, NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_umount("relative"), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_umount(NULL), KERR_INVAL);

    /* A mount hides a directory and never a file, and its parent is a
     * directory too. */
    KT_CHECK_INT_EQ(write_text("/plain", "x"), 0);
    KT_CHECK_INT_EQ(vfs_mount("/plain", &g_no_modes_ops, NULL), KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_mount("/plain/point", &g_no_modes_ops, NULL), KERR_NOTDIR);

    /* Two filesystems at one point: the second is busy; the same pair is a
     * mount that replaces itself. */
    KT_CHECK_INT_EQ(vfs_mkdir("/dir"), 0);
    KT_CHECK_INT_EQ(vfs_mount("/dir", &g_no_modes_ops, NULL), 0);
    KT_CHECK_INT_EQ(vfs_mount("/dir", &g_other_ops, NULL), KERR_BUSY);
    KT_CHECK_INT_EQ(vfs_mount("/dir", &g_no_modes_ops, NULL), 0);

    /* A point with a mount below it stays until the smaller one leaves. */
    KT_CHECK_INT_EQ(vfs_mount("/dir/deep", &g_no_modes_ops, NULL), 0);
    KT_CHECK_INT_EQ(vfs_umount("/dir"), KERR_BUSY);
    KT_CHECK_INT_EQ(vfs_umount("/dir/deep"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/dir"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/dir"), KERR_NOENT);

    /* The root leaves while the mounts below it stay: /proc and /dev are
     * not on the volume of `/`. */
    KT_CHECK_INT_EQ(vfs_mount("/dir", &g_no_modes_ops, NULL), 0);
    KT_CHECK_INT_EQ(vfs_umount("/"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_STR_EQ(vfs_mount_name(0), "/dir");
    KT_CHECK_INT_EQ(vfs_umount("/dir"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/", &stat), KERR_NOENT);

    /* A filesystem may refuse to leave: its own reason is the answer and
     * the mount is still in the table until the refusal is gone. */
    KT_CHECK_INT_EQ(vfs_mount("/keep", &g_refusing_ops, NULL), 0);
    KT_CHECK_INT_EQ(vfs_umount("/keep"), KERR_NOTSUP);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    g_refusing_leave = true;
    KT_CHECK_INT_EQ(vfs_umount("/keep"), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 0);
    g_refusing_leave = false;

    teardown();
}

/*
 * The table of the boot: two good lines mount two volumes, and the broken
 * ones - an unknown type, an unknown option, a line of two words - are
 * skipped without taking the others with them or stopping anything.
 */
static void test_fstab(void)
{
    static const char fstab[] = "# the volumes of the test\n"
                                "\n"
                                "vfsfat0  /mnt/fat  fat32  defaults\n"
                                "fake0    /mnt/dev  dev\n"
                                "\n"
                                "# the lines below mount nothing\n"
                                "nowhere  /mnt/none  fat32\n"
                                "vfsfat0  /mnt/fat2  nosuchfs\n"
                                "fake0    /mnt/bad   dev  ro\n"
                                "one-word\n";
    struct vfs_stat stat;

    ktest_begin("vfs: /etc/fstab mounts the volumes of the table and skips a bad line");

    setup();
    KT_CHECK_INT_EQ(vfs_mkdir("/etc"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/mnt"), 0);

    fat_image_build();
    KT_CHECK_INT_EQ(fat_vfs_register_type(), 0);
    KT_CHECK_INT_EQ(k_blockdev_register(&g_fat_test_device), 0);
    KT_CHECK_INT_EQ(vfs_register_type("dev", test_devfs_mount), 0);

    KT_CHECK_INT_EQ(write_text(VFS_FSTAB_PATH, fstab), 0);

    /* Two lines mount, five do not, and the boot is not the judge of a
     * line: the count says what happened. */
    KT_CHECK_INT_EQ(vfs_mount_fstab(VFS_FSTAB_PATH), 2);
    KT_CHECK_INT_EQ(vfs_mount_count(), 3);

    KT_CHECK_INT_EQ(vfs_stat("/mnt/fat/HELLO.TXT", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 4);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/dev/tty0", &stat), 0);

    /* The broken lines touched no point of the tree. */
    KT_CHECK_INT_EQ(vfs_stat("/mnt/none", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/fat2", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_stat("/mnt/bad", &stat), KERR_NOENT);

    /* A table that is not there is not a boot that stops: the code says
     * why and the image keeps its root. */
    KT_CHECK_INT_EQ(vfs_unlink(VFS_FSTAB_PATH), 0);
    KT_CHECK_INT_EQ(vfs_mount_fstab(VFS_FSTAB_PATH), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_mount_count(), 3);

    KT_CHECK_INT_EQ(vfs_umount("/mnt/dev"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/mnt/fat"), 0);
    KT_CHECK_INT_EQ(vfs_umount("/"), 0);
    KT_CHECK_INT_EQ(vfs_unregister_type("dev"), 0);
    KT_CHECK_INT_EQ(vfs_unregister_type("fat32"), 0);
    KT_CHECK_INT_EQ(k_blockdev_unregister(&g_fat_test_device), 0);
    teardown();
}

/*
 * The rename (ADR 0024 §5): the atomic step of an install. The VFS asks the
 * filesystem of the two paths, refuses a rename between two volumes, and the
 * volume makes the object visible under the new name and no other.
 */
static void test_rename(void)
{
    static k_u8_t first_data[VFS_FILE_DATA_SIZE];
    static k_u8_t second_data[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    char text[16];
    int rc;

    ktest_begin("vfs: rename moves a name inside one filesystem");

    setup();

    KT_CHECK_INT_EQ(vfs_mkdir("/bin"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir("/tmp"), 0);

    rc = vfs_open("/bin/prog.new", VFS_O_RDWR | VFS_O_CREAT, &file, first_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "program", 7), 7);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    /* The name is not there until the rename makes it. */
    KT_CHECK_INT_EQ(vfs_stat("/bin/prog", NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_rename("/bin/prog.new", "/bin/prog"), 0);
    KT_CHECK_INT_EQ(vfs_stat("/bin/prog.new", &(struct vfs_stat){0}), KERR_NOENT);

    memset(text, 0, sizeof(text));
    rc = vfs_open("/bin/prog", VFS_O_RDONLY, &file, first_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_read(&file, text, sizeof(text)), 7);
        KT_CHECK_STR_EQ(text, "program");
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    /* A file that is already there is replaced; a file may move to another
     * directory of the volume. */
    rc = vfs_open("/bin/prog", VFS_O_RDWR | VFS_O_CREAT, &file, first_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "program", 7), 7);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    rc = vfs_open("/tmp/other", VFS_O_RDWR | VFS_O_CREAT, &file, second_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "second", 6), 6);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_rename("/bin/prog", "/bin/other"), 0);

    memset(text, 0, sizeof(text));
    rc = vfs_open("/bin/other", VFS_O_RDONLY, &file, first_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_read(&file, text, sizeof(text)), 7);
        KT_CHECK_STR_EQ(text, "program");
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_rename("/bin/other", "/tmp/other"), 0);

    memset(text, 0, sizeof(text));
    rc = vfs_open("/tmp/other", VFS_O_RDONLY, &file, second_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_read(&file, text, sizeof(text)), 7);
        KT_CHECK_STR_EQ(text, "program");
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    /* The source has to be there, and a directory moves only in its own
     * parent. */
    KT_CHECK_INT_EQ(vfs_rename("/bin/nothing", "/bin/x"), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_mkdir("/bin/dir"), 0);
    KT_CHECK_INT_EQ(vfs_rename("/bin/dir", "/tmp/dir"), KERR_NOTSUP);

    /* A rename between two filesystems is not a copy. */
    {
        struct vfs_operations other;
        static struct vfs_file no_file;

        memset(&other, 0, sizeof(other));
        (void)no_file;

        KT_CHECK_INT_EQ(vfs_mount("/mnt", &other, NULL), 0);
        KT_CHECK_INT_EQ(vfs_rename("/bin/x", "/mnt/x"), KERR_XDEV);
        KT_CHECK_INT_EQ(vfs_umount("/mnt"), 0);
    }

    teardown();
}

void test_vfs(void)
{
    test_mount();
    test_file_operations();
    test_directories();
    test_mkdir_p();
    test_mount_points();
    test_attributes();
    test_directory_search();
    test_directory_descriptor();
    test_statfs();
    test_type_registry();
    test_mount_by_type();
    test_mount_errors();
    test_rename();
    test_fstab();
}
