/*
 * Micro OS — the directory of the `progs` partition (ADR 0022).
 *
 * The parser of components/port_esp32/src/progs_format.c is the only reader
 * of the table the host tool writes, and the bytes come from a partition of
 * the flash: a record with a wild offset or a count of a billion has to be
 * refused before the port asks esp_partition_read for it. The case builds the
 * good table of two programs and then damages it in each way the format
 * names.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/types.h"
#include "progs_format.h"

#define TEST_PARTITION PROGS_PARTITION_SIZE
#define TEST_SLOT 0x10000u

static k_u8_t g_table[PROGS_TABLE_HEADER_SIZE + PROGS_MAX_FILES * PROGS_ENTRY_SIZE];

static void put_le32(k_u8_t *at, k_u32_t value)
{
    at[0] = (k_u8_t)(value & 0xFFu);
    at[1] = (k_u8_t)((value >> 8) & 0xFFu);
    at[2] = (k_u8_t)((value >> 16) & 0xFFu);
    at[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static void put_name(k_u8_t *record, const char *name)
{
    size_t length = strlen(name);

    memset(record, 0, 24);
    memcpy(record, name, length);
}

static void put_entry(k_size_t index, const char *name, k_u32_t offset, k_u32_t size)
{
    k_u8_t *record = g_table + PROGS_TABLE_HEADER_SIZE + index * PROGS_ENTRY_SIZE;

    put_name(record, name);
    put_le32(record + 24, offset);
    put_le32(record + 28, size);
    put_le32(record + 32, 0);
}

/* The table the tool writes for /bin/hello and /bin/cat. */
static k_size_t make_table(void)
{
    memset(g_table, 0, sizeof(g_table));
    memcpy(g_table, PROGS_MAGIC, PROGS_MAGIC_SIZE);
    put_le32(g_table + 8, PROGS_VERSION);
    put_le32(g_table + 12, 2);
    put_le32(g_table + 16, TEST_SLOT);
    put_le32(g_table + 20, 0);
    put_entry(0, "hello", 0, 0x200);
    put_entry(1, "cat", TEST_SLOT, 0x180);
    return sizeof(g_table);
}

static int parse(struct progs_entry *entries, k_size_t max, k_size_t *count)
{
    return progs_directory_parse(g_table, sizeof(g_table), TEST_PARTITION, entries, max, count);
}

void test_progs(void)
{
    struct progs_entry entries[PROGS_MAX_FILES];
    k_size_t count = 0;
    k_size_t size;

    /* The good table: two files at their slots, in the order of the records. */
    size = make_table();
    KT_CHECK(size == sizeof(g_table));
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), 0);
    KT_CHECK_INT_EQ(count, 2u);
    KT_CHECK_STR_EQ(entries[0].name, "hello");
    KT_CHECK_INT_EQ(entries[0].offset, 0u);
    KT_CHECK_INT_EQ(entries[0].size, 0x200u);
    KT_CHECK_STR_EQ(entries[1].name, "cat");
    KT_CHECK_INT_EQ(entries[1].offset, TEST_SLOT);
    KT_CHECK_INT_EQ(entries[1].size, 0x180u);

    /* A partition with no programs is a good table: /bin is empty. */
    make_table();
    put_le32(g_table + 12, 0);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), 0);
    KT_CHECK_INT_EQ(count, 0u);

    /* The magic, the version and the count. */
    make_table();
    g_table[0] = 'X';
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);
    KT_CHECK_INT_EQ(count, 0u);

    make_table();
    put_le32(g_table + 8, PROGS_VERSION + 1);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_le32(g_table + 12, PROGS_MAX_FILES + 1);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    KT_CHECK_INT_EQ(parse(entries, 1, &count), -1);

    /* The slot: aligned, at least the 64 KiB of the format. */
    make_table();
    put_le32(g_table + 16, PROGS_SLOT_SIZE / 2);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_le32(g_table + 16, PROGS_SLOT_SIZE + 0x800);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* The offset of a file: a slot boundary, and never over the directory. */
    make_table();
    put_entry(0, "hello", TEST_SLOT + 4, 0x200);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_entry(0, "hello", 3 * TEST_SLOT, 0x1000);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* A file that does not fit its slot or the partition. */
    make_table();
    put_entry(0, "hello", TEST_SLOT, TEST_SLOT + 1);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_entry(0, "hello", 4 * TEST_SLOT, 0x1000);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_entry(0, "hello", TEST_SLOT, 0);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* The name: a path cannot carry it, or it is the tree itself. */
    make_table();
    put_entry(0, "a/b", TEST_SLOT, 0x200);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_entry(0, "..", TEST_SLOT, 0x200);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    make_table();
    put_entry(0, "", TEST_SLOT, 0x200);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* A name longer than the 23 characters of the field is refused even when
     * its first 24 bytes have no zero. */
    make_table();
    memset(g_table + PROGS_TABLE_HEADER_SIZE, 'a', 24);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* Two files of the same name: the second would shadow the first. */
    make_table();
    put_entry(1, "hello", 2 * TEST_SLOT, 0x180);
    KT_CHECK_INT_EQ(parse(entries, PROGS_MAX_FILES, &count), -1);

    /* A short buffer is not a table. */
    KT_CHECK_INT_EQ(progs_directory_parse(g_table, PROGS_TABLE_HEADER_SIZE - 1, TEST_PARTITION,
                                          entries, PROGS_MAX_FILES, &count),
                    -1);

    /* A partition with no file area before the directory is not one. */
    make_table();
    KT_CHECK_INT_EQ(progs_directory_parse(g_table, sizeof(g_table), PROGS_DIRECTORY_SIZE, entries,
                                          PROGS_MAX_FILES, &count),
                    -1);
}
