/*
 * Micro OS — the install: inventory, gaps and a safe GPT append.
 *
 * The tests build synthetic GPT disks in memory and ask the partition layer
 * what it sees and what it refuses. Nothing in this file writes to a real
 * device: every blockdev is an array of bytes with a write counter, so a
 * failure can be injected at exactly the step the test names.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/partition.h"

#define INST_SECTOR 512u
#define INST_SECTORS 32768u /* 16 MiB */
#define INST_BYTES ((k_size_t)INST_SECTORS * INST_SECTOR)

static k_u8_t g_disk[INST_BYTES];
static unsigned g_writes;
static unsigned g_fail_write; /* 1-based write call to fail, 0 = never */

static int disk_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= INST_SECTORS || count > (k_size_t)INST_SECTORS - sector) {
        return -1;
    }

    memcpy(buf, g_disk + (k_size_t)sector * INST_SECTOR, count * INST_SECTOR);
    return 0;
}

static int disk_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= INST_SECTORS || count > (k_size_t)INST_SECTORS - sector) {
        return -1;
    }

    g_writes++;
    if (g_fail_write != 0u && g_writes == g_fail_write) {
        return -1;
    }

    memcpy(g_disk + (k_size_t)sector * INST_SECTOR, buf, count * INST_SECTOR);
    return 0;
}

static int disk_sync(void *ctx)
{
    K_UNUSED(ctx);
    return 0;
}

static const struct k_blockdev_ops g_disk_ops = {
    .read = disk_read,
    .write = disk_write,
    .sync = disk_sync,
};

static struct k_blockdev g_dev = {
    .name = "inst0",
    .sector_size = INST_SECTOR,
    .sectors = INST_SECTORS,
    .ops = &g_disk_ops,
    .ctx = NULL,
};

static void put_le32(k_u8_t *p, k_u32_t v)
{
    p[0] = (k_u8_t)(v & 0xFFu);
    p[1] = (k_u8_t)((v >> 8) & 0xFFu);
    p[2] = (k_u8_t)((v >> 16) & 0xFFu);
    p[3] = (k_u8_t)((v >> 24) & 0xFFu);
}

static void put_le64(k_u8_t *p, k_u64_t v)
{
    put_le32(p, (k_u32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (k_u32_t)(v >> 32));
}

static k_u32_t get_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static k_u32_t crc32_bytes(const void *data, k_size_t size)
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

static void disk_blank(void)
{
    memset(g_disk, 0, sizeof(g_disk));
    g_writes = 0;
    g_fail_write = 0;
}

/* Build a protective MBR: one 0xEE entry covering the disk. */
static void make_protective_mbr(void)
{
    k_u8_t *mbr = g_disk;

    memset(mbr, 0, 512u);
    mbr[446u + 4u] = 0xEEu;
    put_le32(mbr + 446u + 8u, 1u);
    put_le32(mbr + 446u + 12u, INST_SECTORS - 1u);
    mbr[510] = 0x55u;
    mbr[511] = 0xAAu;
}

/* Write a GPT header at `lba` with `entries_lba` pointing at its array. */
static void make_gpt_header(k_u8_t *hdr, k_u64_t my_lba, k_u64_t alt_lba, k_u64_t entries_lba,
                            k_u64_t first_usable, k_u64_t last_usable, k_u32_t entries,
                            k_u32_t entry_size, k_u32_t entries_crc)
{
    static const char sig[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};

    memset(hdr, 0, 512u);
    memcpy(hdr, sig, 8u);
    put_le32(hdr + 8, 0x00010000u);
    put_le32(hdr + 12, 92u);
    put_le64(hdr + 24, my_lba);
    put_le64(hdr + 32, alt_lba);
    put_le64(hdr + 40, first_usable);
    put_le64(hdr + 48, last_usable);
    /* Disk GUID at 56: leave zero - fine for tests. */
    put_le64(hdr + 72, entries_lba);
    put_le32(hdr + 80, entries);
    put_le32(hdr + 84, entry_size);
    put_le32(hdr + 88, entries_crc);
    put_le32(hdr + 16, 0u);
    put_le32(hdr + 16, crc32_bytes(hdr, 92u));
}

static void gpt_set_entry(k_u8_t *entries, k_u32_t index, k_u32_t entry_size,
                          const k_u8_t type_guid[16], k_u64_t first, k_u64_t last,
                          const char *label)
{
    k_u8_t *e = entries + (k_size_t)index * entry_size;
    k_size_t i;
    k_size_t len = 0;

    memset(e, 0, entry_size);
    memcpy(e, type_guid, 16u);
    /* Unique partition GUID: simple deterministic value. */
    e[16] = 0xAAu;
    e[17] = (k_u8_t)(index + 1u);
    put_le64(e + 32, first);
    put_le64(e + 40, last);

    if (label != NULL) {
        while (label[len] != '\0' && len < 36u) {
            len++;
        }
        for (i = 0; i < len; i++) {
            e[56u + i * 2u] = (k_u8_t)label[i];
            e[56u + i * 2u + 1u] = 0;
        }
    }
}

static const k_u8_t GUID_ESP[16] = {0x28u, 0x73u, 0x2Au, 0xC1u, 0x1Fu, 0xF8u, 0xD2u, 0x11u,
                                    0xBAu, 0x4Bu, 0x00u, 0xA0u, 0xC9u, 0x3Eu, 0xC9u, 0x3Bu};
static const k_u8_t GUID_LINUX[16] = {0xAFu, 0x3Du, 0xC6u, 0x0Fu, 0x83u, 0x84u, 0x72u, 0x47u,
                                      0x8Eu, 0x79u, 0x3Du, 0x69u, 0xD8u, 0x47u, 0x7Du, 0xE4u};

/* Build a valid GPT disk: 128 entries * 128 bytes = 32 sectors at LBA 2 and
 * mirrored at the end. Usable range is [34, disk_sectors-34]. */
static void make_gpt_disk(k_u8_t *entries, k_u32_t entries_count, k_u32_t entry_size)
{
    k_u32_t array_bytes = entries_count * entry_size;
    k_u32_t array_sectors = (array_bytes + 511u) / 512u;
    k_u64_t first_usable = 2u + array_sectors;
    k_u64_t last_usable = (k_u64_t)INST_SECTORS - 1u - array_sectors - 1u;
    k_u64_t backup_entries_lba = last_usable + 1u;
    k_u64_t backup_header_lba = (k_u64_t)INST_SECTORS - 1u;
    k_u32_t ecrc = crc32_bytes(entries, array_bytes);
    k_u8_t hdr[512];

    make_protective_mbr();
    memcpy(g_disk + 2u * 512u, entries, array_bytes);
    memcpy(g_disk + backup_entries_lba * 512u, entries, array_bytes);

    make_gpt_header(hdr, 1u, backup_header_lba, 2u, first_usable, last_usable, entries_count,
                    entry_size, ecrc);
    memcpy(g_disk + 512u, hdr, 512u);

    make_gpt_header(hdr, backup_header_lba, 1u, backup_entries_lba, first_usable, last_usable,
                    entries_count, entry_size, ecrc);
    memcpy(g_disk + backup_header_lba * 512u, hdr, 512u);
}

static void make_base_gpt(void)
{
    static k_u8_t entries[128u * 128u];

    disk_blank();
    memset(entries, 0, sizeof(entries));
    /* ESP: 2048..4095 (1 MiB) */
    gpt_set_entry(entries, 0, 128u, GUID_ESP, 2048u, 4095u, "EFI System");
    /* Foreign Linux: 4096..8191 (2 MiB) - leaves rest as gap */
    gpt_set_entry(entries, 1, 128u, GUID_LINUX, 4096u, 8191u, "Linux");
    make_gpt_disk(entries, 128u, 128u);
}

static k_u32_t entries_array_crc(void)
{
    return crc32_bytes(g_disk + 2u * 512u, 128u * 128u);
}

static k_u32_t backup_entries_array_crc(void)
{
    k_u64_t backup_entries_lba = (k_u64_t)INST_SECTORS - 1u - 32u;
    return crc32_bytes(g_disk + backup_entries_lba * 512u, 128u * 128u);
}

static bool header_crc_ok_at(k_u64_t lba)
{
    k_u8_t hdr[512];
    k_u32_t stored;

    memcpy(hdr, g_disk + lba * 512u, 512u);
    stored = get_le32(hdr + 16);
    put_le32(hdr + 16, 0u);
    return crc32_bytes(hdr, 92u) == stored;
}

static void test_inventory_gaps(void)
{
    struct partition_inventory inv;
    int rc;

    make_base_gpt();
    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(inv.gpt);
    KT_CHECK(inv.has_esp);
    KT_CHECK(!inv.hybrid_mbr);
    KT_CHECK(inv.backup_ok);
    KT_CHECK_INT_EQ((int)inv.entry_count, 2);

    /* Two entries: ESP and Linux. */
    KT_CHECK_INT_EQ((int)inv.entries[0].first_lba, 2048);
    KT_CHECK_INT_EQ((int)inv.entries[0].last_lba, 4095);
    KT_CHECK((inv.entries[0].flags & PARTITION_FLAG_ESP) != 0u);

    KT_CHECK_INT_EQ((int)inv.entries[1].first_lba, 4096);
    KT_CHECK_INT_EQ((int)inv.entries[1].last_lba, 8191);
    KT_CHECK((inv.entries[1].flags & PARTITION_FLAG_FOREIGN_OS) != 0u);

    /* Gaps: before ESP (34..2047, but aligned up to 2048 -> zero after align
     * except if raw starts below 2048; 34 aligned up to 2048, 2048>2047 so
     * gap has sectors 0) and after Linux (8192..last_usable). */
    KT_CHECK(inv.gap_count >= 1u);

    {
        /* Find the large gap after the Linux partition. */
        bool found = false;
        k_u32_t i;

        for (i = 0; i < inv.gap_count; i++) {
            if (inv.gaps[i].raw_first_lba == 8192u) {
                found = true;
                KT_CHECK_INT_EQ((int)(inv.gaps[i].first_lba % 2048u), 0);
                KT_CHECK(inv.gaps[i].sectors > 0u);
                KT_CHECK(inv.gaps[i].first_lba >= inv.gaps[i].raw_first_lba);
                KT_CHECK(inv.gaps[i].last_lba == inv.last_usable);
            }
        }

        KT_CHECK(found);
    }
}

static void test_inventory_alignment(void)
{
    struct partition_inventory inv;
    static k_u8_t entries[128u * 128u];
    int rc;
    k_u32_t i;
    bool found = false;

    disk_blank();
    memset(entries, 0, sizeof(entries));
    /* One partition ending at an unaligned boundary, leaving a gap starting
     * at 5000 (not 1 MiB aligned). */
    gpt_set_entry(entries, 0, 128u, GUID_ESP, 2048u, 4999u, "EFI");
    make_gpt_disk(entries, 128u, 128u);

    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, 0);

    for (i = 0; i < inv.gap_count; i++) {
        if (inv.gaps[i].raw_first_lba == 5000u) {
            found = true;
            KT_CHECK_INT_EQ((int)inv.gaps[i].first_lba, 6144); /* aligned up to 1MiB */
            KT_CHECK_INT_EQ((int)inv.gaps[i].raw_sectors, (int)(inv.last_usable - 5000u + 1u));
            KT_CHECK(inv.gaps[i].sectors < inv.gaps[i].raw_sectors);
        }
    }

    KT_CHECK(found);
}

static void test_append_success(void)
{
    struct partition_inventory inv;
    static k_u8_t entries_before[128u * 128u];
    static k_u8_t entries_after[128u * 128u];
    k_u32_t crc_before;
    int rc;
    k_u64_t backup_entries_lba;

    make_base_gpt();
    memcpy(entries_before, g_disk + 2u * 512u, sizeof(entries_before));
    crc_before = crc32_bytes(entries_before, sizeof(entries_before));

    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, 0);

    /* Append in the gap after 8191. */
    rc = partition_append(&g_dev, 8192u, 4096u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, 0);

    /* Both headers have valid CRC. */
    KT_CHECK(header_crc_ok_at(1u));
    KT_CHECK(header_crc_ok_at((k_u64_t)INST_SECTORS - 1u));

    /* Both entry arrays have the same CRC and match their headers. */
    KT_CHECK_INT_EQ((int)entries_array_crc(), (int)backup_entries_array_crc());
    KT_CHECK_INT_EQ((int)get_le32(g_disk + 512u + 88u), (int)entries_array_crc());

    /* New entry is present in primary and backup. */
    memcpy(entries_after, g_disk + 2u * 512u, sizeof(entries_after));
    backup_entries_lba = (k_u64_t)INST_SECTORS - 1u - 32u;

    {
        bool found_primary = false;
        bool found_backup = false;
        k_u32_t i;

        for (i = 0; i < 128u; i++) {
            k_u8_t *pe = entries_after + (k_size_t)i * 128u;
            k_u8_t *be = g_disk + backup_entries_lba * 512u + (k_size_t)i * 128u;
            k_u64_t first = (k_u64_t)get_le32(pe + 32) | ((k_u64_t)get_le32(pe + 36) << 32);
            k_u64_t last = (k_u64_t)get_le32(pe + 40) | ((k_u64_t)get_le32(pe + 44) << 32);
            k_u64_t bfirst = (k_u64_t)get_le32(be + 32) | ((k_u64_t)get_le32(be + 36) << 32);
            k_u64_t blast = (k_u64_t)get_le32(be + 40) | ((k_u64_t)get_le32(be + 44) << 32);

            if (memcmp(pe, GUID_LINUX, 16u) == 0 && first == 8192u && last == 12287u) {
                found_primary = true;
            }
            if (memcmp(be, GUID_LINUX, 16u) == 0 && bfirst == 8192u && blast == 12287u) {
                found_backup = true;
            }
        }

        KT_CHECK(found_primary);
        KT_CHECK(found_backup);
    }

    /* All other entries byte-identical: hash of table without the new entry
     * is the same as before. */
    {
        k_u32_t i;
        k_u32_t crc_after_masked = 0;
        static k_u8_t tmp[128u * 128u];

        memcpy(tmp, entries_after, sizeof(tmp));
        for (i = 0; i < 128u; i++) {
            k_u8_t *e = tmp + (k_size_t)i * 128u;
            k_u64_t first = (k_u64_t)get_le32(e + 32) | ((k_u64_t)get_le32(e + 36) << 32);
            k_u64_t last = (k_u64_t)get_le32(e + 40) | ((k_u64_t)get_le32(e + 44) << 32);
            bool was_empty = true;
            k_u32_t j;

            /* Identify the newly added entry by its range. */
            if (memcmp(e, GUID_LINUX, 16u) == 0 && first == 8192u && last == 12287u) {
                /* Check it wasn't in the original (original had Linux at 4096..8191). */
                k_u8_t *orig = entries_before + (k_size_t)i * 128u;
                for (j = 0; j < 16u; j++) {
                    if (orig[j] != 0u) {
                        was_empty = false;
                        break;
                    }
                }
                if (was_empty) {
                    memset(e, 0, 128u);
                }
            }
        }

        crc_after_masked = crc32_bytes(tmp, sizeof(tmp));
        KT_CHECK_INT_EQ((int)crc_after_masked, (int)crc_before);
    }
}

static void test_refusals(void)
{
    struct partition_inventory inv;
    int rc;
    k_u8_t entries[128u * 128u];

    /* Bad CRC primary. */
    make_base_gpt();
    g_disk[512u + 40u] ^= 0x01u; /* corrupt header payload, CRC mismatch */
    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, KERR_IO);

    /* Missing/bad backup. */
    make_base_gpt();
    memset(g_disk + ((k_u64_t)INST_SECTORS - 1u) * 512u, 0, 512u);
    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, KERR_IO);

    /* Hybrid MBR: protective + another entry. */
    make_base_gpt();
    g_disk[446u + 16u + 4u] = 0x83u; /* add a non-protective entry */
    put_le32(g_disk + 446u + 16u + 8u, 100u);
    put_le32(g_disk + 446u + 16u + 12u, 10u);
    rc = partition_inventory(&g_dev, &inv);
    KT_CHECK_INT_EQ(rc, KERR_IO);

    /* Start not aligned. */
    make_base_gpt();
    rc = partition_append(&g_dev, 8193u, 2048u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, KERR_INVAL);

    /* Gap smaller than requested / intersection. */
    make_base_gpt();
    /* Request goes into the ESP at 2048-4095. */
    rc = partition_append(&g_dev, 2048u, 2048u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, KERR_INVAL);

    /* Request larger than remaining usable: goes past last_usable. */
    make_base_gpt();
    {
        k_u64_t last_usable = (k_u64_t)INST_SECTORS - 1u - 32u;
        rc = partition_append(&g_dev, 8192u, last_usable, GUID_LINUX, "MICROOS_ROOT");
        KT_CHECK_INT_EQ(rc, KERR_INVAL);
    }

    /* Intersection with existing entry. */
    make_base_gpt();
    rc = partition_append(&g_dev, 4096u, 2048u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, KERR_INVAL);

    /* Append on a disk with bad backup CRC should be refused. */
    make_base_gpt();
    g_disk[((k_u64_t)INST_SECTORS - 1u) * 512u + 40u] ^= 0x01u;
    rc = partition_append(&g_dev, 8192u, 2048u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, KERR_IO);

    (void)entries;
}

static void test_append_restore_on_write_failure(void)
{
    static k_u8_t before[INST_BYTES];
    int rc;

    make_base_gpt();
    memcpy(before, g_disk, sizeof(before));

    /* Fail on the write of the backup header: backup entries array is
     * 128*128 / 512 = 32 sector writes, so the 33rd write is the backup
     * header - the "second step" after the entries array. */
    g_writes = 0;
    g_fail_write = 33u;

    rc = partition_append(&g_dev, 8192u, 2048u, GUID_LINUX, "MICROOS_ROOT");
    KT_CHECK_INT_EQ(rc, KERR_IO);

    /* The disk is back as it was: both GPT copies and the MBR unchanged. */
    KT_CHECK(memcmp(g_disk, before, sizeof(before)) == 0);

    /* And the table is still valid and readable. */
    g_fail_write = 0;
    {
        struct partition_inventory inv;

        rc = partition_inventory(&g_dev, &inv);
        KT_CHECK_INT_EQ(rc, 0);
        KT_CHECK_INT_EQ((int)inv.entry_count, 2);
    }
}

void test_install(void)
{
    ktest_begin("install (inventory, gaps, GPT append)");

    test_inventory_gaps();
    test_inventory_alignment();
    test_append_success();
    test_refusals();
    test_append_restore_on_write_failure();
}
