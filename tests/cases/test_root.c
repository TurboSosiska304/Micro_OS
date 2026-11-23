/*
 * Micro OS — the type of the volume of the root (ADR 0027).
 *
 * The kernel reads the type of the root out of the medium instead of being
 * built with it: the test makes the bytes of a volume of every filesystem of
 * the tree - and of a medium that is none of them - and asks the kernel what
 * it found. The devices here speak both units a real one does: 1024 bytes
 * (the block of MicroFS, the unit of the flash) and 512 (the sector of a
 * partition of a disk).
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/partition.h"
#include "kernel/root.h"
#include "kernel/vfs.h"

/* --- a volume of the memory ------------------------------------------- */

#define ROOT_TEST_SECTORS 16u
#define ROOT_TEST_SECTOR 1024u
#define ROOT_TEST_BYTES (ROOT_TEST_SECTORS * ROOT_TEST_SECTOR)

static k_u8_t s_volume[ROOT_TEST_BYTES];

static int volume_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_SECTORS || count > ROOT_TEST_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, s_volume + sector * ROOT_TEST_SECTOR, count * ROOT_TEST_SECTOR);

    return 0;
}

static int volume_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_SECTORS || count > ROOT_TEST_SECTORS - sector) {
        return -1;
    }

    memcpy(s_volume + sector * ROOT_TEST_SECTOR, buf, count * ROOT_TEST_SECTOR);

    return 0;
}

static const struct k_blockdev_ops s_volume_ops = {
    .read = volume_read,
    .write = volume_write,
};

static const struct k_blockdev s_volume_device = {
    .name = "root0",
    .sector_size = ROOT_TEST_SECTOR,
    .sectors = ROOT_TEST_SECTORS,
    .ops = &s_volume_ops,
    .ctx = s_volume,
};

/* --- a disk with one partition ---------------------------------------- */

/*
 * The disk of the port speaks blocks of a kilobyte (the unit of the driver),
 * so the partition of the table is a device of that unit as well: it is the
 * pair the boot of ADR 0027 finds in a real machine.
 */
#define ROOT_TEST_DISK_SECTORS 8192u
#define ROOT_TEST_DISK_SECTOR 1024u
#define ROOT_TEST_PART_LBA 2048u
#define ROOT_TEST_PART_BYTES ((k_size_t)ROOT_TEST_PART_LBA * 512u)

static k_u8_t s_disk[ROOT_TEST_DISK_SECTORS * ROOT_TEST_DISK_SECTOR];

static int disk_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_DISK_SECTORS || count > ROOT_TEST_DISK_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, s_disk + sector * ROOT_TEST_DISK_SECTOR, count * ROOT_TEST_DISK_SECTOR);

    return 0;
}

static int disk_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_DISK_SECTORS || count > ROOT_TEST_DISK_SECTORS - sector) {
        return -1;
    }

    memcpy(s_disk + sector * ROOT_TEST_DISK_SECTOR, buf, count * ROOT_TEST_DISK_SECTOR);

    return 0;
}

static const struct k_blockdev_ops s_disk_ops = {
    .read = disk_read,
    .write = disk_write,
};

static const struct k_blockdev s_disk_device = {
    .name = "rt0",
    .sector_size = ROOT_TEST_DISK_SECTOR,
    .sectors = ROOT_TEST_DISK_SECTORS,
    .ops = &s_disk_ops,
    .ctx = s_disk,
};

/* The MBR of the disk: one entry of the given type, from ROOT_TEST_PART_LBA
 * (the units of the table) to the end of the disk. */
static void disk_mbr(k_u8_t type)
{
    k_u8_t *entry = s_disk + 446u;
    k_u32_t length = ROOT_TEST_DISK_SECTORS * ROOT_TEST_DISK_SECTOR / 512u - ROOT_TEST_PART_LBA;

    memset(s_disk, 0, sizeof(s_disk));

    entry[4] = type;
    entry[8] = (k_u8_t)(ROOT_TEST_PART_LBA & 0xFFu);
    entry[9] = (k_u8_t)((ROOT_TEST_PART_LBA >> 8) & 0xFFu);
    entry[10] = (k_u8_t)((ROOT_TEST_PART_LBA >> 16) & 0xFFu);
    entry[11] = (k_u8_t)((ROOT_TEST_PART_LBA >> 24) & 0xFFu);
    entry[12] = (k_u8_t)(length & 0xFFu);
    entry[13] = (k_u8_t)((length >> 8) & 0xFFu);
    entry[14] = (k_u8_t)((length >> 16) & 0xFFu);
    entry[15] = (k_u8_t)((length >> 24) & 0xFFu);

    s_disk[510] = 0x55u;
    s_disk[511] = 0xAAu;
}

/* --- a medium of 512-byte sectors -------------------------------------- */

/*
 * The other unit of a device: the sector of a partition of a disk made by a
 * tool of the host, where the magic of ext2 is fifty-six bytes into the third
 * sector and the probe has to read it from there.
 */
#define ROOT_TEST_SMALL_SECTORS 8u
#define ROOT_TEST_SMALL_SECTOR 512u

static k_u8_t s_small[ROOT_TEST_SMALL_SECTORS * ROOT_TEST_SMALL_SECTOR];

static int small_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_SMALL_SECTORS || count > ROOT_TEST_SMALL_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, s_small + sector * ROOT_TEST_SMALL_SECTOR, count * ROOT_TEST_SMALL_SECTOR);

    return 0;
}

static int small_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= ROOT_TEST_SMALL_SECTORS || count > ROOT_TEST_SMALL_SECTORS - sector) {
        return -1;
    }

    memcpy(s_small + sector * ROOT_TEST_SMALL_SECTOR, buf, count * ROOT_TEST_SMALL_SECTOR);

    return 0;
}

static const struct k_blockdev_ops s_small_ops = {
    .read = small_read,
    .write = small_write,
};

static const struct k_blockdev s_small_device = {
    .name = "root1",
    .sector_size = ROOT_TEST_SMALL_SECTOR,
    .sectors = ROOT_TEST_SMALL_SECTORS,
    .ops = &s_small_ops,
    .ctx = s_small,
};

/* --- the types of the registry ---------------------------------------- */

/*
 * A mount function for the test: the type is registered so that the probe has
 * a name to answer with, and a mount of it is not what this test is about.
 */
static int test_mount(const char *point, const char *device)
{
    K_UNUSED(point);
    K_UNUSED(device);

    return KERR_INVAL;
}

static void bytes_put_le16(k_u8_t *out, k_u16_t value)
{
    out[0] = (k_u8_t)(value & 0xFFu);
    out[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

static void bytes_put_le32(k_u8_t *out, k_u32_t value)
{
    out[0] = (k_u8_t)(value & 0xFFu);
    out[1] = (k_u8_t)((value >> 8) & 0xFFu);
    out[2] = (k_u8_t)((value >> 16) & 0xFFu);
    out[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* The header of a MicroFS volume: the last block of it, "MFS1" at the start. */
static void volume_make_mfs(void)
{
    memset(s_volume, 0, sizeof(s_volume));
    bytes_put_le32(s_volume + (ROOT_TEST_SECTORS - 1u) * ROOT_TEST_SECTOR, 0x4D465331u);
}

/* The superblock of ext2: the magic at the second kilobyte plus 56. */
static void volume_make_ext2(void)
{
    memset(s_volume, 0, sizeof(s_volume));
    bytes_put_le16(s_volume + 1080u, 0xEF53u);
}

/* The boot sector of FAT32: the label of the format and the signature. */
static void volume_make_fat32(void)
{
    memset(s_volume, 0, sizeof(s_volume));
    memcpy(s_volume + 82u, "FAT32", 5u);
    s_volume[510] = 0x55u;
    s_volume[511] = 0xAAu;
}

/* A volume of a format this image does not have: four bytes of a person. */
static void volume_make_unknown(void)
{
    memset(s_volume, 0, sizeof(s_volume));
    memcpy(s_volume, "ZFS\0", 4u);
}

void test_root(void)
{
    const char *type;
    int rc;

    ktest_begin("root");

    /* --- the registry: three types of the image ---------------------- */
    rc = vfs_register_type("mfs", test_mount);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        return;
    }

    rc = vfs_register_type("ext2", test_mount);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        (void)vfs_unregister_type("mfs");
        return;
    }

    rc = vfs_register_type("fat32", test_mount);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        (void)vfs_unregister_type("ext2");
        (void)vfs_unregister_type("mfs");
        return;
    }

    KT_CHECK_INT_EQ(k_blockdev_register(&s_volume_device), 0);

    /* --- the magic of the volume is the answer ----------------------- */
    volume_make_mfs();
    KT_CHECK_STR_EQ(k_root_type("root0"), "mfs");

    volume_make_ext2();
    KT_CHECK_STR_EQ(k_root_type("root0"), "ext2");

    volume_make_fat32();
    KT_CHECK_STR_EQ(k_root_type("root0"), "fat32");

    /* A format of another machine is a refusal, not a guess. */
    volume_make_unknown();
    KT_CHECK(k_root_type("root0") == NULL);

    /* --- a medium nothing was written to ----------------------------- */
    /* The flash of a board comes all ones: MicroFS is what the machine makes
     * on it, which is what the boot of a fresh disk did before the type was
     * read at all. */
    memset(s_volume, 0xFF, sizeof(s_volume));
    KT_CHECK_STR_EQ(k_root_type("root0"), "mfs");

    /* A medium of zeros is a device and not a blank flash: no format of it
     * would be honest, and the boot has no root instead of an empty one. */
    memset(s_volume, 0, sizeof(s_volume));
    KT_CHECK(k_root_type("root0") == NULL);

    /* --- a type that is not in the image is not a root --------------- */
    KT_CHECK_INT_EQ(vfs_unregister_type("ext2"), 0);
    volume_make_ext2();
    KT_CHECK(k_root_type("root0") == NULL);
    KT_CHECK_INT_EQ(vfs_register_type("ext2", test_mount), 0);

    /* A volume of the four bytes nobody claims says so and answers nothing. */
    volume_make_unknown();
    KT_CHECK(k_root_type("root0") == NULL);
    KT_CHECK(k_root_type("no-such-device") == NULL);

    /* --- the type of the partition table is a hint, not the answer --- */
    KT_CHECK_INT_EQ(k_blockdev_register(&s_disk_device), 0);

    /* A FAT32 partition with the bytes of a FAT32 volume. */
    disk_mbr(0x0Cu);
    KT_CHECK_INT_EQ(partition_scan(&s_disk_device), 1);
    KT_CHECK_INT_EQ(partition_type_of(k_blockdev_find("rt0p1")), 0x0C);

    /* The partition starts at LBA 2048 of the disk: the boot sector of the
     * volume there is the sector the child reads as its own zero. */
    memcpy(s_disk + ROOT_TEST_PART_BYTES + 82u, "FAT32", 5u);
    s_disk[ROOT_TEST_PART_BYTES + 510u] = 0x55u;
    s_disk[ROOT_TEST_PART_BYTES + 511u] = 0xAAu;
    KT_CHECK_STR_EQ(k_root_type("rt0p1"), "fat32");

    /*
     * A partition labelled "Linux" whose volume is MicroFS: the byte says
     * ext2 and the bytes say mfs, and the bytes win. This is the case of a
     * disk a person made with a tool that writes 0x83 on everything.
     */
    disk_mbr(0x83u);
    KT_CHECK_INT_EQ(partition_scan(&s_disk_device), 1);
    KT_CHECK_INT_EQ(partition_type_of(k_blockdev_find("rt0p1")), 0x83);

    {
        k_u8_t *base = s_disk + ROOT_TEST_PART_BYTES;
        k_size_t part_bytes =
            (k_size_t)ROOT_TEST_DISK_SECTORS * ROOT_TEST_DISK_SECTOR - ROOT_TEST_PART_BYTES;

        bytes_put_le32(base + part_bytes - 1024u, 0x4D465331u);
        KT_CHECK_STR_EQ(k_root_type("rt0p1"), "mfs");
    }

    /* The device of the whole disk is not a partition and has no byte. */
    KT_CHECK_INT_EQ(partition_type_of(&s_disk_device), -1);
    KT_CHECK_INT_EQ(partition_type_of(NULL), -1);
    KT_CHECK_INT_EQ(partition_type_of(&s_volume_device), -1);

    /* --- the magic of a device of another unit ----------------------- */
    /*
     * A partition of a table of 512-byte sectors: the superblock of ext2
     * starts at 1024 and its magic is at 1080, which is fifty-six bytes into
     * the third sector of the device. A probe that read the whole volume at
     * once would need a buffer of the volume; this one reads the sector.
     */
    KT_CHECK_INT_EQ(k_blockdev_register(&s_small_device), 0);
    bytes_put_le16(s_small + 1080u, 0xEF53u);
    KT_CHECK_STR_EQ(k_root_type("root1"), "ext2");
    (void)k_blockdev_unregister(&s_small_device);

    /* --- a mount of the root with the type found --------------------- */
    /* The type of the test does not mount, and the call says which one it
     * tried: the answer names the volume and not "there is no root". */
    volume_make_ext2();
    KT_CHECK_INT_EQ(k_root_mount("root0", &type), KERR_INVAL);

    /* Nothing is mounted at the root, so the mount went through the call of
     * the VFS with the type of the volume and the volume refused it. */
    KT_CHECK_INT_EQ((int)vfs_mount_count(), 0);

    (void)vfs_unregister_type("fat32");
    (void)vfs_unregister_type("ext2");
    (void)vfs_unregister_type("mfs");

    partition_release(&s_disk_device);
    (void)k_blockdev_unregister(&s_volume_device);
}
