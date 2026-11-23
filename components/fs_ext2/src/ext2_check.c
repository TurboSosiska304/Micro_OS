/*
 * Micro OS — the check of an ext2 volume that the machine can run itself.
 *
 * e2fsck is the tool of the format and it is the truth this filesystem is
 * tested against (ADR 0019, M2.2 of the plan). But a machine whose root is
 * ext2 cannot hand its medium to a tool of a host: it has to say whether
 * the counters, the bitmaps and the chain of the directories still describe
 * one tree. This file is that answer, and only that: it reads and it
 * reports, it never repairs.
 *
 * The check is the minimum a root needs before it trusts a crash:
 *
 *   - the counters of a group agree with the bits of its two bitmaps, and
 *     the counters of the superblock agree with the sum of the groups;
 *   - the tree from the root is a chain: every block of a directory is a
 *     whole number of records that covers the block, "." and ".." name the
 *     directory and its parent, every name is inside the table, its inode
 *     is marked used, its directory block is marked used, and no directory
 *     is reached twice.
 *
 * A file that no directory names and a block whose bit is wrong are not
 * searched for: that is a full accounting of every inode, the work of
 * e2fsck, and a later step of the kernel.
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"

/* The deepest tree the check walks and the most directories it remembers.
 * A root deeper or wider than this is reported as too large for the check
 * rather than walked with a map that hides a loop. */
#define EXT2_CHECK_DEPTH 16u
#define EXT2_CHECK_DIRS 256u

static void message_text(struct ext2_check_report *report, const char *text)
{
    k_size_t used = strlen(report->first);

    while (*text != '\0' && used + 1u < sizeof(report->first)) {
        report->first[used++] = *text++;
    }

    report->first[used] = '\0';
}

static void message_number(struct ext2_check_report *report, k_u32_t value)
{
    char digits[11];
    int count = 0;

    do {
        digits[count++] = (char)('0' + (char)(value % 10u));
        value /= 10u;
    } while (value != 0u && count < (int)sizeof(digits));

    while (count > 0) {
        char one[2];

        one[0] = digits[--count];
        one[1] = '\0';
        message_text(report, one);
    }
}

/*
 * Count one problem. Only the first is written down, because a person reads
 * that one first and the counters are the rest of the report.
 */
static void add_error(struct ext2_check_report *report, const char *subject, k_u32_t number,
                      const char *text)
{
    if (report->errors == 0u) {
        message_text(report, subject);
        message_number(report, number);
        message_text(report, ": ");
        message_text(report, text);
    }

    report->errors++;
}

static void add_error_plain(struct ext2_check_report *report, const char *text)
{
    if (report->errors == 0u) {
        message_text(report, text);
    }

    report->errors++;
}

struct check_state {
    struct ext2_volume *vol;
    struct ext2_check_report *report;
    k_u32_t seen[EXT2_CHECK_DIRS];
    k_u32_t seen_count;
    char name[EXT2_NAME_MAX + 1];
};

/* The group that holds a block, the way the writer computes it. */
static k_u32_t check_block_group(const struct ext2_volume *vol, k_u32_t block)
{
    if (block <= vol->first_data_block) {
        return 0;
    }

    return (block - vol->first_data_block) / vol->blocks_per_group;
}

/* Is the bit of a block or of an inode set in its bitmap? */
static int bitmap_bit(struct check_state *state, k_u32_t bitmap_block, k_u32_t index, bool *set)
{
    k_u8_t byte = 0;
    int rc;

    rc = ext2_read_bytes(state->vol, (k_u64_t)bitmap_block * state->vol->block_size + index / 8u,
                         &byte, 1);
    if (rc != 0) {
        return rc;
    }

    *set = (byte & (k_u8_t)(1u << (index % 8u))) != 0u;

    return 0;
}

static int inode_is_used(struct check_state *state, k_u32_t number, bool *used)
{
    struct ext2_group group;
    k_u32_t group_index = (number - 1u) / state->vol->inodes_per_group;
    k_u32_t index = (number - 1u) % state->vol->inodes_per_group;
    int rc;

    rc = ext2_group_read(state->vol, group_index, &group);
    if (rc != 0) {
        return rc;
    }

    return bitmap_bit(state, group.inode_bitmap, index, used);
}

static int block_is_used(struct check_state *state, k_u32_t block, bool *used)
{
    struct ext2_group group;
    k_u32_t group_index = check_block_group(state->vol, block);
    k_u32_t first = state->vol->first_data_block + group_index * state->vol->blocks_per_group;
    int rc;

    rc = ext2_group_read(state->vol, group_index, &group);
    if (rc != 0) {
        return rc;
    }

    return bitmap_bit(state, group.block_bitmap, block - first, used);
}

/* The counters of one group against the bits of its bitmaps. */
static void check_group(struct check_state *state, k_u32_t index)
{
    struct ext2_volume *vol = state->vol;
    struct ext2_group group;
    k_u32_t first = vol->first_data_block + index * vol->blocks_per_group;
    k_u32_t block_bits = vol->blocks_per_group;
    k_u32_t inode_bits = vol->inodes_per_group;
    k_u32_t free_blocks = 0;
    k_u32_t free_inodes = 0;
    k_u32_t i;
    int rc;

    rc = ext2_group_read(vol, index, &group);
    if (rc != 0) {
        add_error(state->report, "group ", index, "cannot be read");
        return;
    }

    if (first >= vol->blocks_count) {
        add_error(state->report, "group ", index, "begins past the volume");
        return;
    }

    if (block_bits > vol->blocks_count - first) {
        block_bits = vol->blocks_count - first;
    }

    if (index * vol->inodes_per_group >= vol->inodes_count) {
        inode_bits = 0;
    } else if (inode_bits > vol->inodes_count - index * vol->inodes_per_group) {
        inode_bits = vol->inodes_count - index * vol->inodes_per_group;
    }

    rc = ext2_read_block(vol, group.block_bitmap, vol->block_buf);
    if (rc != 0) {
        add_error(state->report, "group ", index, "has no block bitmap");
        return;
    }

    for (i = 0; i < block_bits; i++) {
        if ((vol->block_buf[i / 8u] & (k_u8_t)(1u << (i % 8u))) == 0u) {
            free_blocks++;
        }
    }

    rc = ext2_read_block(vol, group.inode_bitmap, vol->block_buf);
    if (rc != 0) {
        add_error(state->report, "group ", index, "has no inode bitmap");
        return;
    }

    for (i = 0; i < inode_bits; i++) {
        if ((vol->block_buf[i / 8u] & (k_u8_t)(1u << (i % 8u))) == 0u) {
            free_inodes++;
        }
    }

    if (free_blocks != group.free_blocks) {
        add_error(state->report, "group ", index, "counts its free blocks wrong");
    }

    if (free_inodes != group.free_inodes) {
        add_error(state->report, "group ", index, "counts its free inodes wrong");
    }

    state->report->free_blocks += free_blocks;
    state->report->free_inodes += free_inodes;
}

/* Walk one directory and everything its entries name. */
static void check_directory(struct check_state *state, k_u32_t number, k_u32_t parent,
                            unsigned depth)
{
    struct ext2_volume *vol = state->vol;
    struct ext2_inode inode;
    k_u64_t blocks;
    k_u64_t logical;
    k_u32_t i;
    int rc;

    rc = ext2_read_inode(vol, number, &inode);
    if (rc != 0) {
        add_error(state->report, "directory ", number, "cannot be read");
        return;
    }

    if (!ext2_inode_is_dir(&inode)) {
        add_error(state->report, "inode ", number, "is named as a directory and is not one");
        return;
    }

    if (inode.links == 0u) {
        add_error(state->report, "directory ", number, "has no link");
    }

    {
        bool in_use = false;

        if (inode_is_used(state, number, &in_use) != 0 || !in_use) {
            add_error(state->report, "directory ", number, "is not marked used");
        }
    }

    if (depth > EXT2_CHECK_DEPTH) {
        add_error(state->report, "directory ", number, "is deeper than the check walks");
        return;
    }

    for (i = 0; i < state->seen_count; i++) {
        if (state->seen[i] == number) {
            add_error(state->report, "directory ", number, "is reached twice");
            return;
        }
    }

    if (state->seen_count >= EXT2_CHECK_DIRS) {
        add_error(state->report, "directory ", number, "is past the room of the check");
        return;
    }

    state->seen[state->seen_count++] = number;
    state->report->directories++;

    if (inode.size == 0u || (inode.size % vol->block_size) != 0u) {
        add_error(state->report, "directory ", number, "is not a whole number of blocks");
        return;
    }

    blocks = inode.size / vol->block_size;

    for (logical = 0; logical < blocks; logical++) {
        k_u32_t block = 0;
        k_u32_t offset = 0;
        k_u32_t index = 0;
        bool used = false;

        rc = ext2_inode_get_block(vol, &inode, logical, &block, false);
        if (rc != 0) {
            add_error(state->report, "directory ", number, "names a block outside the volume");
            return;
        }

        if (block == 0u) {
            add_error(state->report, "directory ", number, "has a hole");
            continue;
        }

        rc = block_is_used(state, block, &used);
        if (rc != 0 || !used) {
            add_error(state->report, "block ", block, "of a directory is not marked used");
        }

        while (offset + 8u <= vol->block_size) {
            k_u8_t raw[8];
            k_u32_t child;
            k_u16_t rec_len;
            k_size_t name_len;

            rc = ext2_read_bytes(vol, (k_u64_t)block * vol->block_size + offset, raw, sizeof(raw));
            if (rc != 0) {
                add_error(state->report, "directory ", number, "cannot be read");
                return;
            }

            child = ext2_le32(raw + 0);
            rec_len = ext2_le16(raw + 4);

            if (child == 0u && rec_len == 0u) {
                /* The rest of the block was never used; that is how an
                 * unused directory block ends. */
                offset = (k_u32_t)vol->block_size;
                break;
            }

            if (rec_len < 8u || (rec_len & 3u) != 0u || offset + rec_len > vol->block_size) {
                add_error(state->report, "directory ", number, "has a record of a bad length");
                offset = (k_u32_t)vol->block_size;
                break;
            }

            if (child != 0u) {
                bool file_type = (vol->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u;

                name_len = file_type ? raw[6] : ext2_le16(raw + 6);

                if (name_len == 0u || name_len > EXT2_NAME_MAX || name_len + 8u > rec_len) {
                    add_error(state->report, "directory ", number, "has a name of a bad length");
                } else if (child > vol->inodes_count) {
                    add_error(state->report, "directory ", number,
                              "names an inode outside the table");
                } else {
                    bool in_use = false;

                    rc = inode_is_used(state, child, &in_use);
                    if (rc != 0 || !in_use) {
                        add_error(state->report, "inode ", child,
                                  "is named and is not marked used");
                    }

                    rc = ext2_read_bytes(vol, (k_u64_t)block * vol->block_size + offset + 8u,
                                         state->name, name_len);
                    if (rc != 0) {
                        add_error(state->report, "directory ", number, "cannot be read");
                        return;
                    }

                    state->name[name_len] = '\0';

                    if (logical == 0u && index == 0u) {
                        if (strcmp(state->name, ".") != 0 || child != number) {
                            add_error(state->report, "directory ", number, "has no '.' of itself");
                        }
                    } else if (logical == 0u && index == 1u) {
                        if (strcmp(state->name, "..") != 0 || child != parent) {
                            add_error(state->report, "directory ", number,
                                      "has a '..' of another directory");
                        }
                    } else if (strcmp(state->name, ".") != 0 && strcmp(state->name, "..") != 0) {
                        struct ext2_inode entry;

                        rc = ext2_read_inode(vol, child, &entry);
                        if (rc == 0 && ext2_inode_is_dir(&entry)) {
                            check_directory(state, child, number, depth + 1u);
                        } else if (rc == 0) {
                            state->report->files++;
                        } else {
                            add_error(state->report, "inode ", child, "cannot be read");
                        }
                    }
                }
            }

            offset += rec_len;
            index++;
        }

        if (offset != vol->block_size) {
            add_error(state->report, "directory ", number, "has a block the records do not cover");
        }
    }
}

int ext2_check_volume(struct ext2_volume *vol, struct ext2_check_report *report)
{
    struct check_state state;
    k_u32_t i;
    int rc;

    if (vol == NULL || report == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    memset(report, 0, sizeof(*report));
    memset(&state, 0, sizeof(state));
    state.vol = vol;
    state.report = report;

    report->groups = vol->groups;
    report->blocks = vol->blocks_count;
    report->inodes = vol->inodes_count;

    for (i = 0; i < vol->groups; i++) {
        check_group(&state, i);
    }

    if (report->free_blocks != vol->free_blocks) {
        add_error_plain(report, "superblock: the free blocks of the groups are not its own");
    }

    if (report->free_inodes != vol->free_inodes) {
        add_error_plain(report, "superblock: the free inodes of the groups are not its own");
    }

    /* A bitmap that is wrong makes the walk of the tree say the same thing
     * twice; the walk still answers about the chain, which is the part a
     * counter never shows. */
    {
        struct ext2_inode root;

        rc = ext2_read_inode(vol, EXT2_ROOT_INODE, &root);
        if (rc != 0) {
            add_error_plain(report, "superblock: there is no root directory");
            return 0;
        }
    }

    check_directory(&state, EXT2_ROOT_INODE, EXT2_ROOT_INODE, 0u);

    return 0;
}

/* --- the repair pass --- */

/* The depth the walk of the repair enters, and the first record that is not
 * one of the reserved inodes of the format (1..10). */
#define EXT2_REPAIR_DEPTH 64u
#define EXT2_REPAIR_RESERVED 11u
#define EXT2_FEATURE_COMPAT_SPARSE_SUPER 0x0001u

static bool repair_bit_test(const k_u8_t *map, k_u32_t index)
{
    return (map[index / 8u] & (k_u8_t)(1u << (index % 8u))) != 0u;
}

static void repair_bit_set(k_u8_t *map, k_u32_t index)
{
    map[index / 8u] |= (k_u8_t)(1u << (index % 8u));
}

static void repair_bit_clear(k_u8_t *map, k_u32_t index)
{
    map[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));
}

struct repair_state {
    struct ext2_volume *vol;
    struct ext2_check_report *report;
    struct ext2_group *groups;
    k_u8_t *used_blocks;
    k_u8_t *used_inodes;
    k_u8_t *reached;
    k_u16_t *refs;
    k_u16_t *group_dirs;
    k_u32_t visits;
    bool incomplete;
};

static k_u32_t repair_group_first(const struct ext2_volume *vol, k_u32_t group)
{
    return vol->first_data_block + group * vol->blocks_per_group;
}

static k_u32_t repair_group_blocks(const struct ext2_volume *vol, k_u32_t group)
{
    k_u32_t first = repair_group_first(vol, group);

    if (first >= vol->blocks_count) {
        return 0;
    }

    if (vol->blocks_per_group > vol->blocks_count - first) {
        return vol->blocks_count - first;
    }

    return vol->blocks_per_group;
}

static k_u32_t repair_group_inodes(const struct ext2_volume *vol, k_u32_t group)
{
    k_u32_t first = group * vol->inodes_per_group;

    if (first >= vol->inodes_count) {
        return 0;
    }

    if (vol->inodes_per_group > vol->inodes_count - first) {
        return vol->inodes_count - first;
    }

    return vol->inodes_per_group;
}

static bool repair_has_super(const struct ext2_volume *vol, k_u32_t group)
{
    static const k_u32_t primes[] = {3u, 5u, 7u};
    k_size_t i;

    if (group == 0u) {
        return true;
    }

    if ((vol->feature_compat & EXT2_FEATURE_COMPAT_SPARSE_SUPER) == 0u) {
        return true;
    }

    for (i = 0; i < K_ARRAY_SIZE(primes); i++) {
        k_u32_t rest = group;

        while (rest % primes[i] == 0u) {
            rest /= primes[i];
        }

        if (rest == 1u) {
            return true;
        }
    }

    return false;
}

static k_u32_t repair_descriptor_blocks(const struct ext2_volume *vol)
{
    return (vol->groups * 32u + vol->block_size - 1u) / vol->block_size;
}

static void repair_mark_block(struct repair_state *state, k_u32_t block)
{
    if (block < state->vol->blocks_count) {
        repair_bit_set(state->used_blocks, block);
    }
}

static void repair_ref_bump(struct repair_state *state, k_u32_t number)
{
    if (number <= state->vol->inodes_count && state->refs[number] < 0xFFFFu) {
        state->refs[number]++;
    }
}

/*
 * Mark the blocks of one table of pointers: the table itself, its entries and
 * the tables below them. `count` is the number of blocks the inode owns, for
 * the i_blocks field the format keeps in sectors.
 */
static int repair_mark_table(struct repair_state *state, k_u32_t table, unsigned level,
                             k_u32_t *count)
{
    struct ext2_volume *vol = state->vol;
    k_u8_t entries[EXT2_BLOCK_MAX];
    k_u32_t pointers = vol->block_size / 4u;
    k_u32_t i;
    int rc;

    if (table == 0u) {
        return 0;
    }

    if (table >= vol->blocks_count) {
        return KERR_IO;
    }

    repair_mark_block(state, table);
    (*count)++;

    rc = ext2_read_block(vol, table, entries);
    if (rc != 0) {
        return rc;
    }

    for (i = 0; i < pointers; i++) {
        k_u32_t child = ext2_le32(entries + i * 4u);

        if (child == 0u) {
            continue;
        }

        if (level == 1u) {
            if (child >= vol->blocks_count) {
                return KERR_IO;
            }

            repair_mark_block(state, child);
            (*count)++;
        } else {
            rc = repair_mark_table(state, child, level - 1u, count);
            if (rc != 0) {
                return rc;
            }
        }
    }

    return 0;
}

static int repair_mark_inode(struct repair_state *state, const struct ext2_inode *inode,
                             k_u32_t *count)
{
    k_u32_t i;
    int rc;

    *count = 0;

    for (i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
        if (inode->block[i] == 0u) {
            continue;
        }

        if (inode->block[i] >= state->vol->blocks_count) {
            return KERR_IO;
        }

        repair_mark_block(state, inode->block[i]);
        (*count)++;
    }

    rc = repair_mark_table(state, inode->block[12], 1u, count);
    if (rc != 0) {
        return rc;
    }

    rc = repair_mark_table(state, inode->block[13], 2u, count);
    if (rc != 0) {
        return rc;
    }

    return repair_mark_table(state, inode->block[14], 3u, count);
}

/* The blocks of a directory, from its first logical block to the hole that
 * ends the chain. A directory never has a hole inside its chain. */
static k_u32_t repair_chain_length(struct repair_state *state, const struct ext2_inode *inode)
{
    k_u32_t count = 0;
    k_u64_t logical;
    k_u64_t limit = state->vol->blocks_count;

    if (limit > 65536u) {
        limit = 65536u;
    }

    for (logical = 0; logical < limit; logical++) {
        k_u32_t block = 0;

        if (ext2_inode_get_block(state->vol, (struct ext2_inode *)inode, logical, &block, false) !=
                0 ||
            block == 0u) {
            break;
        }

        count++;
    }

    return count;
}

/* A directory is a whole number of blocks; a size that is not one, or that
 * names more blocks than its chain holds, is put right. */
static int repair_directory_size(struct repair_state *state, struct ext2_inode *inode)
{
    k_u32_t count = repair_chain_length(state, inode);
    k_u64_t expected;

    if (count == 0u) {
        return KERR_IO;
    }

    expected = (k_u64_t)count * state->vol->block_size;

    if (inode->size == expected) {
        return 0;
    }

    inode->size = expected;
    inode->mtime = ext2_now();
    inode->ctime = inode->mtime;
    state->report->fixed++;

    return ext2_inode_write(state->vol, inode, true);
}

/* The two entries every directory begins with. They are written only when
 * one of their words differs, so a sound volume keeps its bytes. */
static int repair_dots(struct repair_state *state, struct ext2_inode *inode, k_u32_t number,
                       k_u32_t parent)
{
    struct ext2_volume *vol = state->vol;
    k_u8_t block[EXT2_BLOCK_MAX];
    k_u8_t before[EXT2_BLOCK_MAX];
    k_u32_t raw = 0;
    k_u16_t rec_len;
    k_u32_t second;
    bool file_type = (vol->feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u;
    int rc;

    rc = ext2_inode_get_block(vol, inode, 0, &raw, false);
    if (rc != 0 || raw == 0u) {
        return KERR_IO;
    }

    rc = ext2_read_block(vol, raw, block);
    if (rc != 0) {
        return rc;
    }

    memcpy(before, block, vol->block_size);

    rec_len = ext2_le16(block + 4);
    if (rec_len < 12u || (rec_len & 3u) != 0u || rec_len > vol->block_size) {
        rec_len = 12u;
        ext2_put16(block + 4, rec_len);
    }

    ext2_put32(block + 0, number);
    if (file_type) {
        block[6] = 1u;
        block[7] = 2u;
    } else {
        ext2_put16(block + 6, 1u);
    }
    block[8] = '.';

    second = rec_len;
    if (second + 8u > vol->block_size) {
        return KERR_IO;
    }

    rec_len = ext2_le16(block + second + 4);
    if (rec_len < 12u || (rec_len & 3u) != 0u || second + rec_len > vol->block_size) {
        rec_len = (k_u16_t)(vol->block_size - second);
        ext2_put16(block + second + 4, rec_len);
    }

    ext2_put32(block + second + 0, parent);
    if (file_type) {
        block[second + 6] = 2u;
        block[second + 7] = 2u;
    } else {
        ext2_put16(block + second + 6, 2u);
    }
    block[second + 8] = '.';
    block[second + 9] = '.';

    if (memcmp(before, block, vol->block_size) == 0) {
        return 0;
    }

    state->report->fixed++;

    return ext2_write_block(vol, raw, block);
}

static int repair_walk(struct repair_state *state, k_u32_t number, k_u32_t parent, unsigned depth)
{
    struct ext2_volume *vol = state->vol;
    struct ext2_inode inode;
    k_u64_t position = 0;
    struct ext2_dirent entry;
    int rc;

    if (depth > EXT2_REPAIR_DEPTH || ++state->visits > vol->inodes_count) {
        state->incomplete = true;
        add_error(state->report, "directory ", number, "is deeper than the repair walks");
        return 0;
    }

    if (repair_bit_test(state->reached, number)) {
        state->incomplete = true;
        add_error(state->report, "directory ", number, "is reached twice");
        return 0;
    }
    repair_bit_set(state->reached, number);

    rc = ext2_read_inode(vol, number, &inode);
    if (rc != 0 || !ext2_inode_is_dir(&inode)) {
        add_error(state->report, "inode ", number, "is named as a directory and is not one");
        state->incomplete = true;
        return 0;
    }

    rc = repair_directory_size(state, &inode);
    if (rc != 0) {
        add_error(state->report, "directory ", number, "has no chain that can be read");
        state->incomplete = true;
        return 0;
    }

    rc = repair_dots(state, &inode, number, parent);
    if (rc != 0) {
        add_error(state->report, "directory ", number, "has no dot entries to put right");
        state->incomplete = true;
        return 0;
    }

    while ((rc = ext2_dir_next(vol, &inode, &position, &entry)) == 1) {
        struct ext2_inode child;

        if (entry.inode == 0u || entry.inode > vol->inodes_count) {
            add_error(state->report, "directory ", number, "names an inode outside the table");
            state->incomplete = true;
            continue;
        }

        repair_ref_bump(state, entry.inode);

        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
            continue;
        }

        rc = ext2_read_inode(vol, entry.inode, &child);
        if (rc != 0) {
            add_error(state->report, "inode ", entry.inode, "cannot be read");
            state->incomplete = true;
            continue;
        }

        if ((child.mode & EXT2_S_IFMT) == 0u) {
            add_error(state->report, "inode ", entry.inode, "is named and has no type");
            state->incomplete = true;
            continue;
        }

        /*
         * A name may point at an inode whose bit is clear after a power cut.
         * The record is still an object the medium holds, so the repair
         * marks it used again instead of throwing the name away.
         */
        if (!repair_bit_test(state->used_inodes, entry.inode)) {
            repair_bit_set(state->used_inodes, entry.inode);
            state->report->fixed++;
        }

        if (ext2_inode_is_dir(&child)) {
            rc = repair_walk(state, entry.inode, number, depth + 1u);
            if (rc != 0) {
                return rc;
            }
        } else {
            /* A file is reached by its name too: the map is the one of all
             * records a name reaches, not only the directories the walk
             * entered. */
            repair_bit_set(state->reached, entry.inode);
        }
    }

    if (rc < 0) {
        add_error(state->report, "directory ", number, "has records the repair cannot walk");
        state->incomplete = true;
    }

    return 0;
}

static void repair_lost_name(char out[16], k_u32_t number)
{
    char digits[12];
    int count = 0;
    int at = 0;

    do {
        digits[count++] = (char)('0' + (char)(number % 10u));
        number /= 10u;
    } while (number != 0u);

    out[at++] = '#';
    while (count > 0) {
        out[at++] = digits[--count];
    }
    out[at] = '\0';
}

static k_u32_t repair_lost_found(struct repair_state *state)
{
    struct ext2_inode root;
    struct ext2_inode found;
    k_u32_t number = 0;

    if (ext2_read_inode(state->vol, EXT2_ROOT_INODE, &root) != 0) {
        return 0;
    }

    if (ext2_dir_find(state->vol, &root, "lost+found", &number) != 0) {
        return 0;
    }

    if (ext2_read_inode(state->vol, number, &found) != 0 || !ext2_inode_is_dir(&found) ||
        !repair_bit_test(state->used_inodes, number) || !repair_bit_test(state->reached, number)) {
        return 0;
    }

    return number;
}

/*
 * A record that no name reaches is what a create or an unlink leaves when the
 * power stops between the two writes. A file is reconnected under /lost+found
 * as `#<inode>`, which is what e2fsck does: the bytes of the person survive,
 * and the name that was lost is the name no medium can give back. A directory
 * (or a special record) is given back instead: the tree below it would need a
 * second ".." and a walk of its own, and a record that is not a file has no
 * bytes a name would save.
 */
static void repair_orphans(struct repair_state *state, k_u32_t lost_found)
{
    struct ext2_volume *vol = state->vol;
    struct ext2_inode directory;
    bool have_directory = false;
    k_u32_t number;

    if (lost_found != 0u && ext2_read_inode(vol, lost_found, &directory) == 0) {
        have_directory = true;
    }

    for (number = EXT2_REPAIR_RESERVED; number <= vol->inodes_count; number++) {
        struct ext2_inode inode;
        int rc;

        if (!repair_bit_test(state->used_inodes, number) ||
            repair_bit_test(state->reached, number)) {
            continue;
        }

        if (ext2_read_inode(vol, number, &inode) != 0) {
            continue;
        }

        if (ext2_inode_is_file(&inode) && have_directory) {
            char name[16];

            repair_lost_name(name, number);
            rc = ext2_dir_insert(vol, &directory, name, number, 1);
            if (rc == 0) {
                directory.mtime = ext2_now();
                directory.ctime = directory.mtime;

                if (ext2_inode_write(vol, &directory, true) == 0) {
                    state->refs[number] = 1u;
                    repair_bit_set(state->reached, number);
                    state->report->fixed++;
                    continue;
                }
            }
        }

        {
            struct ext2_inode empty;

            memset(&empty, 0, sizeof(empty));
            empty.number = number;

            if (ext2_inode_write(vol, &empty, false) == 0) {
                repair_bit_clear(state->used_inodes, number);
                state->report->fixed++;
            }
        }
    }
}

/*
 * The counters and the link counts of the records that the walk reaches, and
 * the map of the blocks every one of them owns. The map is built here and not
 * while the tree is walked, because a record that no name reaches must give
 * its blocks back rather than keep them as "used" in the map.
 */
static int repair_records(struct repair_state *state)
{
    struct ext2_volume *vol = state->vol;
    k_u32_t number;

    for (number = 2u; number <= vol->inodes_count; number++) {
        struct ext2_inode inode;
        k_u32_t count = 0;
        k_u32_t sectors;
        bool changed = false;
        int rc;

        if (!repair_bit_test(state->used_inodes, number)) {
            continue;
        }

        rc = ext2_read_inode(vol, number, &inode);
        if (rc != 0) {
            add_error(state->report, "inode ", number, "cannot be read");
            state->incomplete = true;
            continue;
        }

        if (number >= EXT2_ROOT_INODE && repair_bit_test(state->reached, number)) {
            k_u16_t links = state->refs[number];

            if (links == 0u) {
                links = 1u;
            }

            if (inode.links != links) {
                inode.links = links;
                inode.ctime = ext2_now();
                changed = true;
            }
        }

        if (ext2_inode_is_dir(&inode) && number >= EXT2_ROOT_INODE &&
            repair_bit_test(state->reached, number)) {
            k_u32_t group = (number - 1u) / vol->inodes_per_group;

            if (group < vol->groups && state->group_dirs[group] < 0xFFFFu) {
                state->group_dirs[group]++;
            }
        }

        if (!ext2_inode_is_dir(&inode) && !ext2_inode_is_file(&inode)) {
            continue;
        }

        rc = repair_mark_inode(state, &inode, &count);
        if (rc != 0) {
            add_error(state->report, "inode ", number, "names a block outside the volume");
            state->incomplete = true;
            continue;
        }

        sectors = count * (vol->block_size / 512u);
        if (inode.blocks != sectors) {
            inode.blocks = sectors;
            changed = true;
        }

        if (changed) {
            rc = ext2_inode_write(vol, &inode, true);
            if (rc != 0) {
                add_error(state->report, "inode ", number, "did not reach the medium");
                state->incomplete = true;
                continue;
            }

            state->report->fixed++;
        }
    }

    return 0;
}

/* The blocks the volume itself holds: the superblock and the descriptors of
 * every group that has a copy of them, the two bitmaps and the table of the
 * inodes. A block bitmap without them would hand the medium's own metadata to
 * the next file. */
static void repair_metadata(struct repair_state *state)
{
    struct ext2_volume *vol = state->vol;
    k_u32_t descriptor_blocks = repair_descriptor_blocks(vol);
    k_u32_t inode_table_blocks =
        (vol->inodes_per_group * vol->inode_size + vol->block_size - 1u) / vol->block_size;
    k_u32_t group;

    for (group = 0; group < vol->groups; group++) {
        const struct ext2_group *descriptor = &state->groups[group];
        k_u32_t first = repair_group_first(vol, group);
        k_u32_t i;

        if (repair_has_super(vol, group)) {
            for (i = 0; i <= descriptor_blocks; i++) {
                repair_mark_block(state, first + i);
            }
        }

        repair_mark_block(state, descriptor->block_bitmap);
        repair_mark_block(state, descriptor->inode_bitmap);

        for (i = 0; i < inode_table_blocks; i++) {
            repair_mark_block(state, descriptor->inode_table + i);
        }
    }
}

/* Rebuild the two bitmaps from the maps and the counters from the bits. The
 * padding of the last group is left as the medium had it: those bits name no
 * block or inode of the volume, and e2fsck tells them apart from a bit that
 * is wrong. */
static int repair_bitmaps(struct repair_state *state)
{
    struct ext2_volume *vol = state->vol;
    k_u32_t total_blocks = 0;
    k_u32_t total_inodes = 0;
    k_u32_t group;

    for (group = 0; group < vol->groups; group++) {
        struct ext2_group *descriptor = &state->groups[group];
        k_u32_t blocks = repair_group_blocks(vol, group);
        k_u32_t inodes = repair_group_inodes(vol, group);
        k_u32_t first_block = repair_group_first(vol, group);
        k_u32_t first_inode = group * vol->inodes_per_group;
        k_u32_t free_blocks = 0;
        k_u32_t free_inodes = 0;
        k_u32_t i;
        int rc;

        rc = ext2_read_block(vol, descriptor->block_bitmap, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        for (i = 0; i < blocks; i++) {
            k_u32_t block = first_block + i;

            if (repair_bit_test(state->used_blocks, block)) {
                vol->block_buf[i / 8u] |= (k_u8_t)(1u << (i % 8u));
            } else {
                vol->block_buf[i / 8u] &= (k_u8_t) ~(1u << (i % 8u));
                free_blocks++;
            }
        }

        rc = ext2_write_block(vol, descriptor->block_bitmap, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        rc = ext2_read_block(vol, descriptor->inode_bitmap, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        for (i = 0; i < inodes; i++) {
            k_u32_t number = first_inode + i + 1u;

            if (repair_bit_test(state->used_inodes, number)) {
                vol->block_buf[i / 8u] |= (k_u8_t)(1u << (i % 8u));
            } else {
                vol->block_buf[i / 8u] &= (k_u8_t) ~(1u << (i % 8u));
                free_inodes++;
            }
        }

        rc = ext2_write_block(vol, descriptor->inode_bitmap, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        if (descriptor->free_blocks != free_blocks || descriptor->free_inodes != free_inodes ||
            descriptor->used_dirs != state->group_dirs[group]) {
            descriptor->free_blocks = (k_u16_t)free_blocks;
            descriptor->free_inodes = (k_u16_t)free_inodes;
            descriptor->used_dirs = state->group_dirs[group];

            rc = ext2_group_write(vol, descriptor);
            if (rc != 0) {
                return rc;
            }

            state->report->fixed++;
        }

        total_blocks += free_blocks;
        total_inodes += free_inodes;
    }

    if (vol->free_blocks != total_blocks || vol->free_inodes != total_inodes) {
        vol->free_blocks = total_blocks;
        vol->free_inodes = total_inodes;
        vol->dirty = true;
        state->report->fixed++;
    }

    return 0;
}

int ext2_repair_volume(struct ext2_volume *vol, struct ext2_check_report *report)
{
    struct repair_state state;
    k_u32_t block_bytes;
    k_u32_t inode_bytes;
    k_u32_t group;
    k_u32_t lost_found;
    int rc;

    if (vol == NULL || report == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    memset(&state, 0, sizeof(state));
    state.vol = vol;
    state.report = report;
    report->groups = vol->groups;
    report->blocks = vol->blocks_count;
    report->inodes = vol->inodes_count;

    block_bytes = (vol->blocks_count + 7u) / 8u;
    inode_bytes = (vol->inodes_count + 7u) / 8u;

    state.groups = (struct ext2_group *)kmm_alloc((k_size_t)vol->groups * sizeof(struct ext2_group),
                                                  KMM_PRIORITY_SYSTEM);
    state.used_blocks = (k_u8_t *)kmm_alloc(block_bytes, KMM_PRIORITY_SYSTEM);
    state.used_inodes = (k_u8_t *)kmm_alloc(inode_bytes, KMM_PRIORITY_SYSTEM);
    state.reached = (k_u8_t *)kmm_alloc(inode_bytes, KMM_PRIORITY_SYSTEM);
    state.refs = (k_u16_t *)kmm_alloc(((k_size_t)vol->inodes_count + 1u) * sizeof(k_u16_t),
                                      KMM_PRIORITY_SYSTEM);
    state.group_dirs =
        (k_u16_t *)kmm_alloc((k_size_t)vol->groups * sizeof(k_u16_t), KMM_PRIORITY_SYSTEM);

    if (state.groups == NULL || state.used_blocks == NULL || state.used_inodes == NULL ||
        state.reached == NULL || state.refs == NULL || state.group_dirs == NULL) {
        rc = KERR_NOMEM;
        goto out;
    }

    memset(state.used_blocks, 0, block_bytes);
    memset(state.used_inodes, 0, inode_bytes);
    memset(state.reached, 0, inode_bytes);
    memset(state.refs, 0, ((k_size_t)vol->inodes_count + 1u) * sizeof(k_u16_t));
    memset(state.group_dirs, 0, (k_size_t)vol->groups * sizeof(k_u16_t));

    /* The groups as the medium holds them; the counters are written from the
     * bits at the end of the pass, and the walk needs the two bitmaps and the
     * table of every group. */
    for (group = 0; group < vol->groups; group++) {
        k_u32_t inodes = repair_group_inodes(vol, group);
        k_u32_t i;

        rc = ext2_group_read(vol, group, &state.groups[group]);
        if (rc != 0) {
            add_error(report, "group ", group, "cannot be read");
            rc = 0;
            goto out;
        }

        rc = ext2_read_block(vol, state.groups[group].inode_bitmap, vol->block_buf);
        if (rc != 0) {
            add_error(report, "group ", group, "has no inode bitmap");
            rc = 0;
            goto out;
        }

        for (i = 0; i < inodes; i++) {
            k_u32_t number = group * vol->inodes_per_group + i + 1u;

            if ((vol->block_buf[i / 8u] & (k_u8_t)(1u << (i % 8u))) != 0u) {
                repair_bit_set(state.used_inodes, number);
            }
        }
    }

    rc = repair_walk(&state, EXT2_ROOT_INODE, EXT2_ROOT_INODE, 0u);
    if (rc != 0) {
        goto out;
    }

    lost_found = state.incomplete ? 0u : repair_lost_found(&state);
    if (!state.incomplete) {
        repair_orphans(&state, lost_found);
    }

    rc = repair_records(&state);
    if (rc != 0) {
        goto out;
    }

    repair_metadata(&state);

    rc = repair_bitmaps(&state);
    if (rc != 0) {
        add_error(report, "volume", 0, "the bitmaps did not reach the medium");
        goto out;
    }

    rc = ext2_volume_flush(vol);
    if (rc != 0) {
        add_error(report, "volume", 0, "the superblock did not reach the medium");
    }

out:
    kmm_free(state.groups);
    kmm_free(state.used_blocks);
    kmm_free(state.used_inodes);
    kmm_free(state.reached);
    kmm_free(state.refs);
    kmm_free(state.group_dirs);

    return rc;
}
