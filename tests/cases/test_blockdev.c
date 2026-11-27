/*
 * Micro OS — the block devices of the system.
 *
 * A filesystem is a consumer of blocks: MicroFS asks for sector 17 of
 * "flash0" and does not care what stands behind it. The test registers a
 * device of its own and walks the table the way a mount does - by name,
 * and through the calls the filesystem will make.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/partition.h"

#define TEST_SECTORS 8
#define TEST_SECTOR_SIZE 16

static k_u8_t s_data[TEST_SECTORS][TEST_SECTOR_SIZE];

static int test_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > TEST_SECTORS) {
        return -1;
    }

    memcpy(buf, s_data[sector], count * TEST_SECTOR_SIZE);
    return 0;
}

static int test_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > TEST_SECTORS) {
        return -1;
    }

    memcpy(s_data[sector], buf, count * TEST_SECTOR_SIZE);
    return 0;
}

static const struct k_blockdev_ops s_ops = {
    .read = test_read,
    .write = test_write,
    .erase = NULL,
    .sync = NULL,
};

static const struct k_blockdev s_device = {
    .name = "test0",
    .sector_size = TEST_SECTOR_SIZE,
    .sectors = TEST_SECTORS,
    .ops = &s_ops,
    .ctx = s_data,
};

/* --- the door of the writes (ADR 0041 §2) -------------------------------- */

static bool s_gate_open;
static int s_gate_calls;

static int test_writes(void *ctx, bool open)
{
    K_UNUSED(ctx);

    s_gate_calls++;
    s_gate_open = open;

    return 0;
}

static const struct k_blockdev_ops s_gate_ops = {
    .read = test_read,
    .write = test_write,
    .erase = NULL,
    .sync = NULL,
    .writes = test_writes,
};

static const struct k_blockdev s_gate_device = {
    .name = "gate0",
    .sector_size = TEST_SECTOR_SIZE,
    .sectors = TEST_SECTORS,
    .ops = &s_gate_ops,
    .ctx = s_data,
};

void test_blockdev(void)
{
    const struct k_blockdev *found;
    k_u8_t written[TEST_SECTOR_SIZE];
    k_u8_t read_back[TEST_SECTOR_SIZE];
    k_size_t i;

    ktest_begin("blockdev");

    /* A device without the calls a filesystem needs is not a device. */
    KT_CHECK_INT_EQ(k_blockdev_register(NULL), -1);

    KT_CHECK_INT_EQ(k_blockdev_register(&s_device), 0);

    found = k_blockdev_find("test0");
    KT_CHECK(found == &s_device);
    KT_CHECK(k_blockdev_find("nope") == NULL);
    KT_CHECK(k_blockdev_find(NULL) == NULL);

    /* The device is in the table, and the table is walkable. */
    KT_CHECK(k_blockdev_count() >= 1);
    if (k_blockdev_count() >= 1) {
        KT_CHECK(k_blockdev_at(0) == &s_device);
    }
    KT_CHECK(k_blockdev_at(k_blockdev_count()) == NULL);

    /* One sector written and read back through the operations. */
    for (i = 0; i < TEST_SECTOR_SIZE; i++) {
        written[i] = (k_u8_t)(i * 3 + 1);
    }

    KT_CHECK_INT_EQ(s_device.ops->write(s_device.ctx, 2, written, 1), 0);
    KT_CHECK_INT_EQ(s_device.ops->read(s_device.ctx, 2, read_back, 1), 0);
    KT_CHECK_INT_EQ(memcmp(written, read_back, TEST_SECTOR_SIZE), 0);

    /* The bounds are the bounds of the device. */
    KT_CHECK_INT_EQ(s_device.ops->read(s_device.ctx, TEST_SECTORS, read_back, 1), -1);

    /* --- the door of the writes --------------------------------------- */

    /* A medium with no door answers the question every device is asked. */
    KT_CHECK(!k_blockdev_gated(&s_device));
    KT_CHECK_INT_EQ(k_blockdev_writes(&s_device, true), KERR_NOTSUP);
    KT_CHECK_INT_EQ(k_blockdev_writes(NULL, true), KERR_INVAL);

    /* A controller that put the door there is asked through the table. */
    KT_CHECK_INT_EQ(k_blockdev_register(&s_gate_device), 0);

    found = k_blockdev_find("gate0");
    KT_CHECK(found != NULL);
    KT_CHECK(k_blockdev_gated(found));

    s_gate_open = false;
    s_gate_calls = 0;

    KT_CHECK_INT_EQ(k_blockdev_writes(found, true), 0);
    KT_CHECK(s_gate_open);
    KT_CHECK_INT_EQ(k_blockdev_writes(found, false), 0);
    KT_CHECK(!s_gate_open);
    KT_CHECK_INT_EQ(s_gate_calls, 2);

    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_gate_device), 0);
    KT_CHECK(k_blockdev_find("gate0") == NULL);

    /*
     * The cache between a mount and the controller forwards the door: the
     * device the table hands out is the cache's view, and the driver is the
     * one that keeps the state.
     */
    KT_CHECK_INT_EQ(k_blockdev_register_cached(&s_gate_device, false), 0);

    found = k_blockdev_find("gate0");
    KT_CHECK(found != NULL);
    KT_CHECK(k_blockdev_gated(found));

    s_gate_open = false;

    KT_CHECK_INT_EQ(k_blockdev_writes(found, true), 0);
    KT_CHECK(s_gate_open);

    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_gate_device), 0);
    KT_CHECK(k_blockdev_find("gate0") == NULL);
}

/* --- the partitions of a disk -------------------------------------------- */

/*
 * A disk of 8 MiB in the memory of the test. It is larger than the other
 * devices on purpose: the tables of the partitions are written in sectors
 * of 512 bytes at the beginning of a medium, and a table is only worth
 * reading if the numbers in it can be held against the size of the disk.
 */
#define PART_DISK_SECTORS 16384u
#define PART_SECTOR_SIZE 512u
#define PART_DISK_BYTES ((k_size_t)PART_DISK_SECTORS * PART_SECTOR_SIZE)

#define PART_LBA_BYTES 512u
#define PART_MBR_OFFSET 446u
#define PART_MBR_ENTRY 16u
#define PART_MBR_SIGNATURE 510u

#define PART_GPT_HEADER 512u
#define PART_GPT_ENTRIES 1024u
#define PART_GPT_ENTRY_COUNT 128u
#define PART_GPT_ENTRY_SIZE 128u

static k_u8_t s_part_disk[PART_DISK_BYTES];

static int part_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= PART_DISK_SECTORS || count > (k_size_t)PART_DISK_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, s_part_disk + (k_size_t)sector * PART_SECTOR_SIZE, count * PART_SECTOR_SIZE);

    return 0;
}

static int part_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= PART_DISK_SECTORS || count > (k_size_t)PART_DISK_SECTORS - sector) {
        return -1;
    }

    memcpy(s_part_disk + (k_size_t)sector * PART_SECTOR_SIZE, buf, count * PART_SECTOR_SIZE);

    return 0;
}

static int s_part_gate_open;

static int part_writes(void *ctx, bool open)
{
    K_UNUSED(ctx);

    s_part_gate_open = open;

    return 0;
}

static const struct k_blockdev_ops s_part_ops = {
    .read = part_read,
    .write = part_write,
    .erase = NULL,
    .sync = NULL,
    .writes = part_writes,
};

static struct k_blockdev s_part_device = {
    .name = "pmbr0",
    .sector_size = PART_SECTOR_SIZE,
    .sectors = PART_DISK_SECTORS,
    .ops = &s_part_ops,
    .ctx = NULL,
};

static void put_le32(k_u8_t *out, k_u32_t value)
{
    out[0] = (k_u8_t)(value & 0xFFu);
    out[1] = (k_u8_t)((value >> 8) & 0xFFu);
    out[2] = (k_u8_t)((value >> 16) & 0xFFu);
    out[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static void put_le64(k_u8_t *out, k_u64_t value)
{
    put_le32(out, (k_u32_t)value);
    put_le32(out + 4, (k_u32_t)(value >> 32));
}

/*
 * A GPT on a disk whose sectors are 512 bytes - the real laptop case.
 * The NVMe namespace reports LBA size 512 (lbads 9); the driver must expose
 * 512-byte sectors so a partition ending on an odd 512-byte LBA is
 * representable. entries_lba is 2016 (not 2), 128 entries of 128 bytes.
 * p2 ends past last_usable but inside the disk - sgdisk -v accepts it.
 */
#define PART_GPT512_SECTOR 512u
#define PART_GPT512_SECTORS 1000215216u
#define PART_GPT512_ENTRIES_LBA 2016u
#define PART_GPT512_ENTRIES_BYTES (128u * 128u)

/* LBA 0 (protective MBR) and LBA 1 (GPT header). */
static k_u8_t s_gpt512_head[2u * PART_GPT512_SECTOR];
static k_u8_t s_gpt512_entries[PART_GPT512_ENTRIES_BYTES];

static int gpt512_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    k_size_t i;

    K_UNUSED(ctx);

    for (i = 0; i < count; i++) {
        k_u32_t s = sector + (k_u32_t)i;
        k_u8_t *out = (k_u8_t *)buf + i * PART_GPT512_SECTOR;

        if (s < 2u) {
            memcpy(out, s_gpt512_head + (k_size_t)s * PART_GPT512_SECTOR, PART_GPT512_SECTOR);
        } else if (s >= PART_GPT512_ENTRIES_LBA &&
                   s < PART_GPT512_ENTRIES_LBA + PART_GPT512_ENTRIES_BYTES / PART_GPT512_SECTOR) {
            k_size_t off = (k_size_t)(s - PART_GPT512_ENTRIES_LBA) * PART_GPT512_SECTOR;

            memcpy(out, s_gpt512_entries + off, PART_GPT512_SECTOR);
        } else {
            memset(out, 0, PART_GPT512_SECTOR);
        }
    }

    return 0;
}

static const struct k_blockdev_ops s_gpt512_ops = {
    .read = gpt512_read,
    .write = NULL,
    .erase = NULL,
    .sync = NULL,
};

static struct k_blockdev s_gpt512_device = {
    .name = "gpt5120",
    .sector_size = PART_GPT512_SECTOR,
    .sectors = PART_GPT512_SECTORS,
    .ops = &s_gpt512_ops,
    .ctx = NULL,
};

/* The blank medium of the test: what an image that was just made reads as. */
static void part_disk_blank(void)
{
    memset(s_part_disk, 0xFF, sizeof(s_part_disk));
}

/*
 * The table area of an MBR that a tool wrote: the four entries are zeroed
 * where the disk says "empty" and the signature ends the sector. The blank
 * medium of the test is all ones, and a table of ones is not what a
 * formatter leaves behind.
 */
static void part_mbr_begin(void)
{
    memset(s_part_disk + PART_MBR_OFFSET, 0, 4u * PART_MBR_ENTRY);
    s_part_disk[PART_MBR_SIGNATURE] = 0x55u;
    s_part_disk[PART_MBR_SIGNATURE + 1u] = 0xAAu;
}

static void part_mbr_entry(k_size_t slot, k_u8_t type, k_u32_t first, k_u32_t sectors)
{
    k_u8_t *entry = s_part_disk + PART_MBR_OFFSET + slot * PART_MBR_ENTRY;

    entry[4] = type;
    put_le32(entry + 8, first);
    put_le32(entry + 12, sectors);
}

/* The checksum of the GPT, written the way the parser reads it back. */
static k_u32_t part_crc32(const void *data, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_u32_t crc = 0xFFFFFFFFu;
    k_size_t i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/* A GPT of two partitions, with both checksums: a table a machine would
 * write. The MBR in front of it only protects the table, as the standard
 * says, and the parser has to prefer the GPT. */
static void part_gpt_make(void)
{
    k_u8_t *header = s_part_disk + PART_GPT_HEADER;
    k_u8_t *entry = s_part_disk + PART_GPT_ENTRIES;
    const char magic[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    k_size_t i;

    part_disk_blank();

    part_mbr_begin();
    part_mbr_entry(0, 0xEEu, 1u, 0xFFFFFFFFu);

    for (i = 0; i < sizeof(magic); i++) {
        header[i] = (k_u8_t)magic[i];
    }

    put_le32(header + 8, 0x00010000u); /* the revision of the format */
    put_le32(header + 12, 92u);        /* the size of the header */
    put_le64(header + 24, 1u);         /* this header is the one of LBA 1 */
    put_le64(header + 32, PART_DISK_SECTORS - 1u);
    put_le64(header + 40, 34u); /* the first usable sector */
    put_le64(header + 48, PART_DISK_SECTORS - 34u);
    put_le64(header + 72, 2u); /* the array of the entries */
    put_le32(header + 80, PART_GPT_ENTRY_COUNT);
    put_le32(header + 84, PART_GPT_ENTRY_SIZE);

    /* An unused entry is sixteen zero bytes, not sixteen ones: the array of
     * a table a tool wrote is zeroed where it is empty. */
    memset(s_part_disk + PART_GPT_ENTRIES, 0, PART_GPT_ENTRY_COUNT * PART_GPT_ENTRY_SIZE);

    /* The first entry: sectors 2048 to 4095. */
    entry[0] = 0x0Cu;
    put_le64(entry + 32, 2048u);
    put_le64(entry + 40, 4095u);

    /* The second: 6144 to 8191. */
    entry += PART_GPT_ENTRY_SIZE;
    entry[0] = 0x0Cu;
    put_le64(entry + 32, 6144u);
    put_le64(entry + 40, 8191u);

    put_le32(header + 88, part_crc32(s_part_disk + PART_GPT_ENTRIES,
                                     PART_GPT_ENTRY_COUNT * PART_GPT_ENTRY_SIZE));

    /* The CRC of the header is taken with its own field zero. */
    put_le32(header + 16, 0u);
    put_le32(header + 16, part_crc32(header, 92u));
}

void test_partition(void)
{
    const struct k_blockdev *child;
    k_u8_t sector[PART_SECTOR_SIZE];
    k_u32_t checksum;
    k_size_t devices;

    ktest_begin("partition");

    s_part_device.sector_size = PART_SECTOR_SIZE;
    s_part_device.sectors = PART_DISK_SECTORS;

    /* --- a medium without a table is not a volume --------------------- */
    part_disk_blank();
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_NOENT);
    KT_CHECK(k_blockdev_find("pmbr0p1") == NULL);

    /* --- the MBR: two primaries --------------------------------------- */
    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 2048u, 4096u);
    part_mbr_entry(1, 0x0Cu, 8192u, 2048u);

    KT_CHECK_INT_EQ(partition_scan(&s_part_device), 2);

    child = k_blockdev_find("pmbr0p1");
    KT_CHECK(child != NULL);
    if (child != NULL) {
        KT_CHECK_INT_EQ((int)child->sector_size, (int)PART_SECTOR_SIZE);
        KT_CHECK_INT_EQ((int)child->sectors, 4096);
        KT_CHECK_INT_EQ(child->name[6], '1');
        KT_CHECK(child->ctx != NULL);
    }

    child = k_blockdev_find("pmbr0p2");
    KT_CHECK(child != NULL);
    if (child != NULL) {
        KT_CHECK_INT_EQ((int)child->sectors, 2048);
    }

    /*
     * A partition of a disk with the door of the writes carries the door
     * too: the mount of a partition asks the controller below it, and a
     * partition of a medium without one answers KERR_NOTSUP.
     */
    child = k_blockdev_find("pmbr0p1");
    if (child != NULL) {
        KT_CHECK(k_blockdev_gated(child));

        s_part_gate_open = false;
        KT_CHECK_INT_EQ(k_blockdev_writes(child, true), 0);
        KT_CHECK(s_part_gate_open);
        KT_CHECK_INT_EQ(k_blockdev_writes(child, false), 0);
        KT_CHECK(!s_part_gate_open);
    }

    /* The type of the entry travels with the child: the boot of the root
     * reads it as a hint about the filesystem inside (ADR 0027). */
    KT_CHECK_INT_EQ(partition_type_of(k_blockdev_find("pmbr0p1")), 0x83);
    KT_CHECK_INT_EQ(partition_type_of(k_blockdev_find("pmbr0p2")), 0x0C);
    KT_CHECK_INT_EQ(partition_type_of(&s_part_device), -1);
    KT_CHECK_INT_EQ(partition_type_of(NULL), -1);

    /* The child reads the sector of the disk where the partition starts. */
    s_part_disk[2048u * PART_LBA_BYTES + 7u] = 0x42u;
    child = k_blockdev_find("pmbr0p1");
    if (child != NULL) {
        KT_CHECK_INT_EQ(child->ops->read(child->ctx, 0, sector, 1), 0);
        KT_CHECK_INT_EQ(sector[7], 0x42);

        /* ... and a write through it goes to the same place. */
        memset(sector, 0x5A, sizeof(sector));
        KT_CHECK_INT_EQ(child->ops->write(child->ctx, 1, sector, 1), 0);
        KT_CHECK_INT_EQ(s_part_disk[2049u * PART_LBA_BYTES + 100u], 0x5A);

        /* The end of the partition is the end of the device it shows. */
        KT_CHECK_INT_EQ(child->ops->read(child->ctx, child->sectors, sector, 1), -1);
        KT_CHECK_INT_EQ(child->ops->read(child->ctx, child->sectors - 1u, sector, 2), -1);
        KT_CHECK_INT_EQ(child->ops->write(child->ctx, child->sectors, sector, 1), -1);
    }

    /* A scan of the same disk replaces its children, it does not add them. */
    devices = k_blockdev_count();
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), 2);
    KT_CHECK_INT_EQ((int)k_blockdev_count(), (int)devices);

    /* --- a table that contradicts itself is refused ------------------- */
    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 16000u, 1024u); /* ends past the disk */
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);

    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 2048u, 4096u);
    part_mbr_entry(1, 0x83u, 4096u, 1024u); /* the same sectors twice */
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);

    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 0u, 1024u); /* LBA 0 is the table itself */
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);

    /* An MBR without its signature is not a table at all. */
    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 2048u, 4096u);
    s_part_disk[PART_MBR_SIGNATURE] = 0x00u; /* no signature, no table */
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_NOENT);

    /* --- a disk of 1 KB sectors and a partition that misses a sector -- */
    s_part_device.sector_size = 1024u;
    s_part_device.sectors = PART_DISK_SECTORS / 2u;

    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 2049u, 4095u); /* starts in the middle of one */
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);

    part_disk_blank();
    part_mbr_begin();
    part_mbr_entry(0, 0x83u, 2048u, 4096u);
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), 1);

    child = k_blockdev_find("pmbr0p1");
    KT_CHECK(child != NULL);
    if (child != NULL) {
        KT_CHECK_INT_EQ((int)child->sector_size, 1024);
        KT_CHECK_INT_EQ((int)child->sectors, 2048);
    }

    /* --- the GPT ------------------------------------------------------ */
    s_part_device.sector_size = PART_SECTOR_SIZE;
    s_part_device.sectors = PART_DISK_SECTORS;

    part_gpt_make();
    checksum = part_crc32(s_part_disk, sizeof(s_part_disk));

    KT_CHECK_INT_EQ(partition_scan(&s_part_device), 2);

    /* The scan reads: the medium is the same after it as before it. */
    KT_CHECK_INT_EQ((int)part_crc32(s_part_disk, sizeof(s_part_disk)), (int)checksum);

    child = k_blockdev_find("pmbr0p1");
    KT_CHECK(child != NULL);
    if (child != NULL) {
        KT_CHECK_INT_EQ((int)child->sectors, 2048);
        KT_CHECK_INT_EQ(child->ops->read(child->ctx, 0, sector, 1), 0);
        KT_CHECK_INT_EQ(sector[0], s_part_disk[2048u * PART_LBA_BYTES]);
    }

    child = k_blockdev_find("pmbr0p2");
    KT_CHECK(child != NULL);
    if (child != NULL) {
        KT_CHECK_INT_EQ((int)child->sectors, 2048);
        KT_CHECK_INT_EQ(child->ops->read(child->ctx, 0, sector, 1), 0);
        KT_CHECK_INT_EQ(sector[0], s_part_disk[6144u * PART_LBA_BYTES]);
    }

    /* A table with a broken entry is not half read: the CRC covers it. */
    s_part_disk[PART_GPT_ENTRIES + 32u] ^= 0xFFu;
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);
    s_part_disk[PART_GPT_ENTRIES + 32u] ^= 0xFFu;

    /* The same for a header that does not match its own checksum. */
    s_part_disk[PART_GPT_HEADER + 16u] ^= 0xFFu;
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);
    s_part_disk[PART_GPT_HEADER + 16u] ^= 0xFFu;

    /* A protective MBR of a disk whose GPT is gone is a damaged table. */
    s_part_disk[PART_GPT_HEADER] = 0xFFu;
    KT_CHECK_INT_EQ(partition_scan(&s_part_device), KERR_INVAL);

    /* --- GPT at real laptop geometry (512-byte sectors, entries_lba 2016)
     * The Samsung MZVLB512HBJQ reports 512-byte LBA (lbads 9). The driver
     * must expose 512-byte sectors, not 1024-byte MFS blocks. entries_lba is
     * 2016 (not 2), so the array lives at byte 1 032 192. p2 ends past
     * last_usable but inside the disk - sgdisk -v says "No problems found",
     * so the scan must accept it. */
    {
        k_u8_t *hdr = s_gpt512_head + PART_GPT_HEADER;
        k_u8_t *ent;
        k_u64_t disk_512 = 1000215216ull;
        k_u64_t last_usable = 1000214158ull;
        k_u64_t p2_last = 1000215182ull;

        memset(s_gpt512_head, 0, sizeof(s_gpt512_head));
        memset(s_gpt512_entries, 0, sizeof(s_gpt512_entries));

        /* Protective MBR: one 0xEE entry covering the disk. */
        s_gpt512_head[PART_MBR_SIGNATURE] = 0x55u;
        s_gpt512_head[PART_MBR_SIGNATURE + 1u] = 0xAAu;
        {
            k_u8_t *pmbr = s_gpt512_head + PART_MBR_OFFSET;

            pmbr[4] = 0xEEu;
            put_le32(pmbr + 8, 1u);
            put_le32(pmbr + 12, 0xFFFFFFFFu);
        }

        memcpy(hdr, "EFI PART", 8);
        put_le32(hdr + 8, 0x00010000u);
        put_le32(hdr + 12, 92u);
        put_le64(hdr + 24, 1u);                  /* current LBA */
        put_le64(hdr + 32, 1000214191ull);        /* alternate LBA */
        put_le64(hdr + 40, 2048u);                /* first usable */
        put_le64(hdr + 48, last_usable);          /* last usable */
        put_le64(hdr + 72, PART_GPT512_ENTRIES_LBA);
        put_le32(hdr + 80, 128u);
        put_le32(hdr + 84, 128u);

        /* p1: 2048 .. 206847 */
        ent = s_gpt512_entries;
        ent[0] = 0xAFu; /* any non-zero type GUID byte */
        put_le64(ent + 32, 2048u);
        put_le64(ent + 40, 206847u);

        /* p2: 206848 .. 1 000 215 182 (past last_usable, inside disk) */
        ent = s_gpt512_entries + 128u;
        ent[0] = 0xAFu;
        put_le64(ent + 32, 206848u);
        put_le64(ent + 40, p2_last);

        put_le32(hdr + 88, part_crc32(s_gpt512_entries, PART_GPT512_ENTRIES_BYTES));
        put_le32(hdr + 16, 0u);
        put_le32(hdr + 16, part_crc32(hdr, 92u));

        KT_CHECK_INT_EQ((int)s_gpt512_device.sector_size, 512);
        KT_CHECK_INT_EQ((int)s_gpt512_device.sectors, 1000215216);
        KT_CHECK(p2_last > last_usable);
        KT_CHECK(p2_last < disk_512);

        KT_CHECK_INT_EQ(partition_scan(&s_gpt512_device), 2);

        child = k_blockdev_find("gpt5120p1");
        KT_CHECK(child != NULL);
        if (child != NULL) {
            KT_CHECK_INT_EQ((int)child->sector_size, 512);
            KT_CHECK_INT_EQ((int)child->sectors, 204800); /* 206847-2048+1 */
        }

        child = k_blockdev_find("gpt5120p2");
        KT_CHECK(child != NULL);
        if (child != NULL) {
            KT_CHECK_INT_EQ((int)child->sectors, (int)(p2_last - 206848u + 1u));
        }

        partition_release(&s_gpt512_device);
    }

    /* --- the slots go back when the disk is let go -------------------- */
    partition_release(&s_part_device);
    KT_CHECK(k_blockdev_find("pmbr0p1") == NULL);
    KT_CHECK(partition_name(&s_part_device, 0) == NULL);
    KT_CHECK_INT_EQ(partition_scan(NULL), KERR_INVAL);
}
