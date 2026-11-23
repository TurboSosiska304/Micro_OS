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
