/*
 * Micro OS — the rule of the disks (ADR 0041).
 *
 * The test makes the bytes of a medium of every kind the rule knows - blank,
 * a volume of this system, and the superblock of each foreign system the
 * plan names - and asks the rule what it sees. The last question of every
 * case is the one the rule exists for: the writes. A device that counts them
 * proves that reading a medium never writes to it, which is the promise the
 * first boot of a new controller makes.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/volume.h"
#include "kernel/vfs.h"

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* --- a medium of the memory ------------------------------------------- */

#define VOL_SEC 1024u
#define VOL_SECTORS 4096u /* 4 MiB: the magic of btrfs is at 0x10040 of a volume */
#define VOL_BYTES (VOL_SEC * VOL_SECTORS)
#define VOL_MBR_PARTITION 2048u /* the first partition, in LBAs of 512 bytes */

static k_u8_t s_bytes[VOL_BYTES];
static int s_writes;

static int medium_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= VOL_SECTORS || count > VOL_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, s_bytes + (k_size_t)sector * VOL_SEC, count * VOL_SEC);

    return 0;
}

static int medium_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= VOL_SECTORS || count > VOL_SECTORS - sector) {
        return -1;
    }

    s_writes++;
    memcpy(s_bytes + (k_size_t)sector * VOL_SEC, buf, count * VOL_SEC);

    return 0;
}

static const struct k_blockdev_ops s_medium_ops = {
    .read = medium_read,
    .write = medium_write,
};

static const struct k_blockdev s_medium = {
    .name = "vol0",
    .sector_size = VOL_SEC,
    .sectors = VOL_SECTORS,
    .ops = &s_medium_ops,
};

static void put_le16(k_u8_t *bytes, k_u16_t value)
{
    bytes[0] = (k_u8_t)(value & 0xFFu);
    bytes[1] = (k_u8_t)(value >> 8);
}

static void put_le32(k_u8_t *bytes, k_u32_t value)
{
    bytes[0] = (k_u8_t)(value & 0xFFu);
    bytes[1] = (k_u8_t)((value >> 8) & 0xFFu);
    bytes[2] = (k_u8_t)((value >> 16) & 0xFFu);
    bytes[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* A medium with the MBR of a PC and one Linux partition at LBA 2048. */
static void make_mbr(void)
{
    k_u8_t *entry = s_bytes + 446u + 4u;

    memset(s_bytes, 0, VOL_BYTES);

    s_bytes[446u] = 0x00u;      /* not bootable */
    s_bytes[446u + 4u] = 0x83u; /* Linux */
    put_le32(s_bytes + 446u + 8u, VOL_MBR_PARTITION);
    put_le32(s_bytes + 446u + 12u, VOL_SECTORS * 2u - VOL_MBR_PARTITION);
    s_bytes[510] = 0x55u;
    s_bytes[511] = 0xAAu;
    K_UNUSED(entry);
}

/* The header block of MicroFS at the end of the volume. */
static void make_mfs(void)
{
    k_u8_t *header = s_bytes + (VOL_SECTORS - 1u) * VOL_SEC;

    put_le32(header, 0x4D465331u); /* "MFS1" */
    put_le32(header + 4u, 3u);     /* the current version of the format */
    put_le32(header + 8u, VOL_SEC);
}

static void make_ext2(bool ext4)
{
    put_le16(s_bytes + 1080u, 0xEF53u);

    if (ext4) {
        put_le32(s_bytes + 1120u, 0x0040u); /* EXT4_FEATURE_INCOMPAT_EXTENTS */
    }
}

static void make_btrfs(void)
{
    memcpy(s_bytes + 0x10040u, "_BHRfS_M", 8u);
}

static void make_zeroes(void)
{
    memset(s_bytes, 0, VOL_BYTES);
}

static void make_ones(void)
{
    memset(s_bytes, 0xFF, VOL_BYTES);
}

static void expect_kind(enum k_volume_kind kind, const char *format, k_u32_t table_sectors,
                        const char *title)
{
    struct k_volume_id id;
    int before = s_writes;
    int rc;

    rc = k_volume_identify(&s_medium, &id);

    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ((int)id.kind, (int)kind);

    if (format == NULL) {
        KT_CHECK(id.format == NULL);
    } else {
        KT_CHECK_STR_EQ(id.format, format);
    }

    KT_CHECK_INT_EQ((int)id.table_sectors, (int)table_sectors);

    /* The reader of the rule is not a writer of the medium. */
    KT_CHECK_INT_EQ(s_writes, before);

    if (kind != K_VOLUME_OURS) {
        const char *why = NULL;

        KT_CHECK(!k_volume_may_write(&s_medium, &id, &why));
        KT_CHECK(why != NULL);
    }

    K_UNUSED(title);
}

/*
 * The list of /etc/volumes of the mounted root, and the positive answer of
 * the rule: a volume of this system that the list names may be written. The
 * root is a volume of the memory ("ram0", the device of the tests of the
 * filesystem), mounted for the length of this case and put back as it was.
 */
static void named_checks(void)
{
    static const struct k_blockdev s_other = {
        .name = "vol1",
        .sector_size = VOL_SEC,
        .sectors = VOL_SECTORS,
        .ops = &s_medium_ops,
    };
    struct mfs_ram_device memory;
    struct k_blockdev root;
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    static const char text[] = "# the volumes of this machine\nvol0\n";
    struct k_volume_id id;
    const char *why = NULL;
    int rc;

    KT_CHECK_INT_EQ(mfs_ram_init(&memory, 128u), 0);
    mfs_ram_device(&memory, &root);
    KT_CHECK_INT_EQ(mfs_format(&root, 32u), MFS_OK);
    KT_CHECK_INT_EQ(k_blockdev_register(&root), 0);
    KT_CHECK_INT_EQ(mfs_vfs_register_type(), 0);
    KT_CHECK_INT_EQ(vfs_mount_type("/", "mfs", "ram0"), 0);

    KT_CHECK_INT_EQ(vfs_mkdir_p("/etc"), 0);

    rc = vfs_open(K_VOLUME_LIST_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, text, sizeof(text) - 1u), (int)(sizeof(text) - 1u));
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK(k_volume_named(&s_medium));
    KT_CHECK(!k_volume_named(&s_other));

    /* The whole rule, positively: a volume of this system that the list names
     * is the one the rule lets through. */
    make_mbr();
    make_mfs();
    KT_CHECK_INT_EQ(k_volume_identify(&s_medium, &id), 0);
    KT_CHECK(k_volume_may_write(&s_medium, &id, &why));
    KT_CHECK(why == NULL);

    (void)vfs_unlink(K_VOLUME_LIST_PATH);
    (void)vfs_rmdir("/etc");
    KT_CHECK_INT_EQ(vfs_umount("/"), 0);
    mfs_unmount();
    KT_CHECK_INT_EQ(k_blockdev_unregister(&root), 0);
    mfs_ram_free(&memory);
}

void test_volume(void)
{
    struct k_volume_id id;
    const char *why = NULL;
    static const char s_list[] = "# the volumes of this machine\n"
                                 "\n"
                                 "  sd0p1  \n"
                                 "sd0p2 # the data of a person\n"
                                 "#sd0p3\n";

    ktest_begin("volume (the rule of ADR 0041)");

    /* A medium nothing was ever written to: no volume, no write. */
    make_ones();
    expect_kind(K_VOLUME_BLANK, NULL, 0u, "all ones");
    make_zeroes();
    expect_kind(K_VOLUME_BLANK, NULL, 0u, "all zeros");

    /* A volume of this system behind the table of a PC: the first partition
     * is at LBA 2048, which is sector 1024 of a medium of a kilobyte. */
    make_mbr();
    make_mfs();
    expect_kind(K_VOLUME_OURS, "mfs", VOL_MBR_PARTITION / 2u, "our volume");
    KT_CHECK_INT_EQ(k_volume_identify(&s_medium, &id), 0);

    /* Ours, but not named in a list this boot can read: still no write. The
     * root of the test has no /etc/volumes and no volume at all. */
    KT_CHECK(!k_volume_may_write(&s_medium, &id, &why));
    KT_CHECK(why != NULL);
    KT_CHECK_STR_EQ(why, "the volume is not named in " K_VOLUME_LIST_PATH);

    /* The superblock of another system is a refusal, and the rule says which
     * one it saw. */
    make_zeroes();
    make_ext2(false);
    expect_kind(K_VOLUME_FOREIGN, "ext2", 0u, "ext2");
    KT_CHECK_INT_EQ(k_volume_identify(&s_medium, &id), 0);
    KT_CHECK(!k_volume_may_write(&s_medium, &id, &why));
    KT_CHECK_STR_EQ(why, "the medium carries a volume of another system");

    make_zeroes();
    make_ext2(true);
    expect_kind(K_VOLUME_FOREIGN, "ext4", 0u, "ext4");

    make_zeroes();
    make_btrfs();
    expect_kind(K_VOLUME_FOREIGN, "btrfs", 0u, "btrfs");

    make_zeroes();
    memcpy(s_bytes + 3u, "NTFS    ", 8u);
    expect_kind(K_VOLUME_FOREIGN, "ntfs", 0u, "ntfs");

    make_zeroes();
    memcpy(s_bytes, "XFSB", 4u);
    expect_kind(K_VOLUME_FOREIGN, "xfs", 0u, "xfs");

    make_zeroes();
    memcpy(s_bytes + 82u, "FAT32", 5u);
    s_bytes[510] = 0x55u;
    s_bytes[511] = 0xAAu;
    expect_kind(K_VOLUME_FOREIGN, "fat32", 0u, "fat32");

    /* The bytes that are neither a volume nor blank: an unknown medium, and
     * an unknown medium is not writable either. */
    make_zeroes();
    memcpy(s_bytes, "nonsense", 8u);
    expect_kind(K_VOLUME_UNKNOWN, NULL, 0u, "unknown");

    /* The list of the volumes of /etc: one name a line, comments and spaces
     * around it, and no prefix matching. */
    KT_CHECK(k_volume_list_has(s_list, "sd0p1"));
    KT_CHECK(k_volume_list_has(s_list, "sd0p2"));
    KT_CHECK(!k_volume_list_has(s_list, "sd0p"));
    KT_CHECK(!k_volume_list_has(s_list, "sd0p1x"));
    KT_CHECK(!k_volume_list_has(s_list, "sd0p3"));
    KT_CHECK(!k_volume_list_has(s_list, "ssd0p1"));
    KT_CHECK(!k_volume_list_has(s_list, ""));
    KT_CHECK(!k_volume_list_has(NULL, "sd0p1"));
    KT_CHECK(!k_volume_list_has(s_list, NULL));
    KT_CHECK(k_volume_list_has("sd0p1", "sd0p1"));
    KT_CHECK(k_volume_list_has("sd0p1\r\n", "sd0p1"));

    /* The table of the medium is never written: a request below the first
     * sector of the first partition is refused, one at it or above it is
     * not. */
    make_mbr();
    make_mfs();
    KT_CHECK_INT_EQ(k_volume_identify(&s_medium, &id), 0);
    KT_CHECK(!k_volume_write_above_table(&id, 0u, 1u));
    KT_CHECK(!k_volume_write_above_table(&id, id.table_sectors - 1u, 1u));
    KT_CHECK(k_volume_write_above_table(&id, id.table_sectors, 1u));
    KT_CHECK(k_volume_write_above_table(&id, id.table_sectors + 8u, 4u));

    /* A medium without a table has no boundary to draw; the rule is then the
     * other half of it - the superblock and the list. */
    make_zeroes();
    KT_CHECK_INT_EQ(k_volume_identify(&s_medium, &id), 0);
    KT_CHECK_INT_EQ((int)id.table_sectors, 0);
    KT_CHECK(k_volume_write_above_table(&id, 0u, 1u));

    /* The last question is the list of /etc/volumes of a mounted root: the
     * positive answer of the rule, and the name that is not on the list. */
    named_checks();

    /* Nothing of the test wrote the medium: every call above was a reader. */
    KT_CHECK_INT_EQ(s_writes, 0);
}
