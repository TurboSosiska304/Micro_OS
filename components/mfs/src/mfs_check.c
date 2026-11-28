/*
 * Micro OS — the check and the repair of the volume of MicroFS.
 *
 * The volume has no journal and no second copy of anything, so a power cut,
 * a brownout or a bug leaves whatever it leaves. The check answers the
 * question of a person who is about to trust the volume with a file: is
 * what the header, the table of the inodes, the directories and the map of
 * the free blocks say about each other still one story?
 *
 * The check itself never writes. The blocks are only read, the state of the
 * mounted volume is never touched, and the format of the volume does not
 * change: a volume that is reported as damaged is left exactly the way it
 * was found, so that it can be copied off the board before anything touches
 * it.
 *
 * The repair pass is the second half of the file, and it is the one writer:
 * with the permission of the person at the console it changes the volume
 * where the medium itself proves what the change has to be - an orphan
 * block, an entry nobody can reach, a chain that loops, a size that does not
 * match its chain, the counter of a directory, a record no name reaches. It
 * does not guess about the header, the data of a file or a shared directory,
 * and the check runs again after it, so a volume that reads "0 problem(s)
 * after the repair" is one the reader walked whole (ADR 0013 and the rules
 * in `mfs/mfs.h`).
 */
#include "mfs/mfs.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/kmm.h"
#include "mfs_private.h"

/*
 * The biggest directory the filesystem can build: dir_add() refuses to
 * grow the buffer of a directory beyond four blocks, so a record that
 * claims more than that describes something the system cannot read back.
 */
#define CHECK_DIR_MAX (MFS_BLOCK_SIZE * 4)

/*
 * How deep the tree of the directories may go. A path is limited to
 * MFS_PATH_MAX bytes and every level of the tree costs at least two of
 * them ("a/"), so a tree that was made through the interface is shallower;
 * a deeper one is a loop that has to be found instead of followed.
 */
#define CHECK_DEPTH_MAX 64

/* The blocks of one erase sector of the flash, see mfs.c. */
#define CHECK_SECTOR_BLOCKS 4

/*
 * The flags of a record hold the type, the mode and the owner (ADR 0005);
 * the bits between the type and the mode are the only ones the format does
 * not define, so a set bit there is a damage.
 */
#define CHECK_FLAGS_MASK                                                                           \
    (MFS_INODE_TYPE_MASK | MFS_INODE_PENDING | (MFS_MODE_MASK << MFS_MODE_SHIFT) |                 \
     (MFS_OWNER_MASK << MFS_OWNER_SHIFT))

/* What the message of the report is about. */
enum check_subject {
    CHECK_SUBJECT_HEADER,
    CHECK_SUBJECT_VOLUME,
    CHECK_SUBJECT_INODE,
    CHECK_SUBJECT_BLOCK,
};

static void add_error(struct mfs_check_report *report, enum check_subject subject, k_u32_t number,
                      const char *text);

/*
 * The blocks of the data that the records of the table own. The map is one
 * bit per data block, the same shape as the map of the free blocks, and it
 * is allocated for the volume that is walked: the mount checks the same
 * ceiling, so a volume that mounted always fits. A volume that asks for
 * more is reported and not walked; the map goes back to the allocator when
 * the walk is over, so the check of a big volume costs its memory only
 * while it runs.
 */
static k_u8_t *s_claimed;
static k_u32_t s_claimed_bytes;

/* Take the map for a volume, or say in the report why there is none. */
static int claimed_alloc(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t blocks = view->bitmap_blocks;
    k_size_t size;
    k_u8_t *map;

    if (blocks == 0u || blocks > MFS_BITMAP_MAX_BLOCKS) {
        add_error(report, CHECK_SUBJECT_VOLUME, 0,
                  "the map of the free blocks is too big to check");
        return MFS_EINVAL;
    }

    size = (k_size_t)blocks * MFS_BLOCK_SIZE;
    map = (k_u8_t *)kmm_alloc(size, KMM_PRIORITY_SYSTEM);

    if (map == NULL) {
        add_error(report, CHECK_SUBJECT_VOLUME, 0, "no memory for the map of the blocks");
        return MFS_ENOMEM;
    }

    memset(map, 0, size);
    s_claimed = map;
    s_claimed_bytes = (k_u32_t)size;

    return MFS_OK;
}

static void claimed_free(void)
{
    if (s_claimed != NULL) {
        kmm_free(s_claimed);
        s_claimed = NULL;
    }

    s_claimed_bytes = 0;
}

/* A frame of the walk of the tree of the directories. */
struct walk_frame {
    k_u32_t inode;
    k_u32_t offset; /* where the next entry of the directory starts */
    k_u32_t depth;
    k_u32_t size; /* the bytes of the directory, valid when loaded */
    bool loaded;
    bool validated;
    bool broken;
    bool repaired; /* the repair has met this directory before */
};

/*
 * The frames of the walk. The tree is walked one directory at a time, so a
 * damaged volume cannot take the stack of the task down with a deep tree;
 * the buffer belongs to the filesystem and there is one walk at a time
 * under its lock, so the check and the repair share the frames. The repair
 * uses inode, offset, depth, size, loaded and broken; validated is the
 * check's.
 */
static struct walk_frame s_frames[CHECK_DEPTH_MAX];

/* --- the report --- */

/*
 * The kernel has no printf, so the first problem is put together by hand.
 * The message is short and is cut at the end of the buffer: a person wants
 * to know what to look at, not to read an essay.
 */
static void msg_text(struct mfs_check_report *report, const char *text)
{
    k_size_t len = strlen(report->first);

    while (*text != '\0' && len + 1 < MFS_CHECK_MESSAGE_MAX) {
        report->first[len++] = *text++;
    }

    report->first[len] = '\0';
}

static void msg_number(struct mfs_check_report *report, k_u32_t value)
{
    char digits[10];
    int count = 0;

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < (int)sizeof(digits));

    while (count > 0) {
        char one[2];

        one[0] = digits[--count];
        one[1] = '\0';
        msg_text(report, one);
    }
}

/*
 * Count a problem. Only the first one is written down: it is the one that
 * a person sees first, and the rest of the report is the counters.
 */
static void add_error(struct mfs_check_report *report, enum check_subject subject, k_u32_t number,
                      const char *text)
{
    if (report->errors == 0) {
        switch (subject) {
        case CHECK_SUBJECT_HEADER:
            msg_text(report, "header: ");
            break;
        case CHECK_SUBJECT_VOLUME:
            msg_text(report, "volume: ");
            break;
        case CHECK_SUBJECT_INODE:
            msg_text(report, "inode ");
            msg_number(report, number);
            msg_text(report, ": ");
            break;
        case CHECK_SUBJECT_BLOCK:
            msg_text(report, "block ");
            msg_number(report, number);
            msg_text(report, ": ");
            break;
        }
        msg_text(report, text);
    }

    kerror("mfs: fsck problem %u: %s #%u: %s", (unsigned)(report->errors + 1),
           subject == CHECK_SUBJECT_HEADER   ? "header"
           : subject == CHECK_SUBJECT_VOLUME ? "volume"
           : subject == CHECK_SUBJECT_INODE  ? "inode"
                                             : "block",
           (unsigned)number, text);

    report->errors++;
}

/* --- the small helpers of the volume --- */

static bool bit_test(const k_u8_t *map, k_u32_t index)
{
    return (map[index / 8u] & (k_u8_t)(1u << (index % 8u))) != 0u;
}

static void bit_set(k_u8_t *map, k_u32_t index)
{
    map[index / 8u] |= (k_u8_t)(1u << (index % 8u));
}

static void bit_clear(k_u8_t *map, k_u32_t index)
{
    map[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));
}

/*
 * Decode a record of a block of the table that was already read, and say
 * whether it may be trusted.
 *
 * A used record of a version 3 volume has to be the one that was written. A
 * record that does not match its checksum is one that a power cut left half
 * written: nothing it says can be trusted, so it is read as a free record and
 * the repair of the volume gives it back (ADR 0038).
 */
static void record_decode(const struct mfs_private *view, struct mfs_check_report *report,
                          k_u32_t index, const k_u8_t *block, k_u32_t offset, struct mfs_inode *out)
{
    mfs_priv_inode_decode(block + offset, view->inode_size, out);

    if ((out->flags & MFS_INODE_USED) != 0u &&
        !mfs_priv_inode_sound(block + offset, view->inode_size)) {
        add_error(report, CHECK_SUBJECT_INODE, index,
                  "the checksum of the record does not match: the write it was is not whole");
        memset(out, 0, sizeof(*out));
    }
}

static int read_inode(const struct mfs_private *view, struct mfs_check_report *report,
                      k_u32_t index, struct mfs_inode *out)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t where;
    k_u32_t offset;
    int rc;

    if (index == 0 || index > view->inode_capacity) {
        add_error(report, CHECK_SUBJECT_INODE, index, "the record is outside the table");
        memset(out, 0, sizeof(*out));
        return MFS_OK;
    }

    mfs_priv_inode_address(view, index, &where, &offset);

    rc = mfs_priv_block_read(where, block);
    if (rc != MFS_OK) {
        add_error(report, CHECK_SUBJECT_BLOCK, where, "cannot be read");
        return rc;
    }

    record_decode(view, report, index, block, offset, out);

    return MFS_OK;
}

static k_u32_t entry_inode(const k_u8_t *buf, k_size_t offset)
{
    k_u32_t inode;

    memcpy(&inode, buf + offset, sizeof(inode));
    return inode;
}

static k_u16_t entry_name_len(const k_u8_t *buf, k_size_t offset)
{
    k_u16_t len;

    memcpy(&len, buf + offset + 4u, sizeof(len));
    return len;
}

/* A name is one component of a path, so it holds no slash. */
static bool entry_has_slash(const k_u8_t *buf, k_size_t offset, k_u16_t name_len)
{
    k_u16_t i;

    for (i = 0; i < name_len; i++) {
        if (buf[offset + sizeof(struct mfs_dirent_header) + i] == '/') {
            return true;
        }
    }

    return false;
}

/* --- the header of the volume --- */

static int check_header(const struct mfs_private *view, struct mfs_check_report *report)
{
    const struct mfs_superblock *mounted = view->sb;
    struct mfs_superblock sb;
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t bitmap_bytes;
    k_u32_t bitmap_blocks;
    bool sound = true;
    int rc;

    rc = mfs_priv_block_read(mounted->total_blocks - 1u, block);
    if (rc != MFS_OK) {
        add_error(report, CHECK_SUBJECT_BLOCK, mounted->total_blocks - 1u, "cannot be read");
        return rc;
    }

    memcpy(&sb, block, sizeof(sb));

    if (sb.magic != MFS_MAGIC) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the magic is not the one of MicroFS");
        sound = false;
    }

    if (sb.version != MFS_VERSION && sb.version != MFS_VERSION_TIME &&
        sb.version != MFS_VERSION_LEGACY && sb.version != MFS_VERSION_JOURNAL) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the version is not one of the system");
        sound = false;
    }

    /*
     * The version and the time feature go together: a record has the room
     * for the two moments or it has not, and a header that says otherwise
     * is a header a reader cannot trust (ADR 0013). A version 3 volume
     * promises the group and the checksum of a record as well (ADR 0038).
     */
    if (sb.version >= MFS_VERSION_TIME && (sb.features & MFS_FEATURE_TIME) == 0u) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the version has no time feature");
        sound = false;
    }
    if (sb.version == MFS_VERSION_LEGACY && (sb.features & MFS_FEATURE_TIME) != 0u) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "a legacy volume claims the time feature");
        sound = false;
    }
    if (sb.version >= MFS_VERSION) {
        if ((sb.features & MFS_FEATURE_RECORD_CRC) == 0u || sb.inode_size != MFS_INODE_SIZE) {
            add_error(report, CHECK_SUBJECT_HEADER, 0,
                      "the version has no checksum of a record or no room for one");
            sound = false;
        }
    }

    if (sb.block_size != MFS_BLOCK_SIZE) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the block size is not the one of the volume");
        sound = false;
    }

    if (sb.total_blocks != view->device_blocks) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the size does not match the device");
        sound = false;
    }

    if ((sb.features & MFS_FEATURE_CRC) != 0u && sb.crc != mfs_priv_header_crc(&sb)) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the checksum does not match");
        sound = false;
    }

    /*
     * The three sections have to fill the volume in the order the design
     * gives them: data from the first block, then META, then INFO up to
     * the last block of the device.
     */
    if (sb.data_blocks == 0u || sb.meta_blocks == 0u || sb.info_blocks == 0u ||
        sb.data_blocks + sb.meta_blocks + sb.info_blocks != sb.total_blocks ||
        sb.data_start != 0u || sb.meta_start + sb.meta_blocks != sb.info_start ||
        sb.info_start + sb.info_blocks != sb.total_blocks) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the sections do not fill the volume");
        sound = false;
    }

    /*
     * The header has to be alone in its erase sector: writing the table of
     * the inodes may then never erase it together with the sector.
     */
    if ((sb.info_start % CHECK_SECTOR_BLOCKS) != 0u) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "INFO is not at the border of a sector");
        sound = false;
    }

    if (sb.root_inode != MFS_ROOT_INODE) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the root is not the record one");
        sound = false;
    }

    /* The map of the free blocks has to fit into META: at its start in a
     * version 1 and 2 volume, at its top in a version 3 one. */
    bitmap_bytes = (sb.data_blocks + 7u) / 8u;
    bitmap_blocks = (bitmap_bytes + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;

    if (bitmap_blocks == 0u || bitmap_blocks != view->bitmap_blocks ||
        bitmap_blocks >= sb.meta_blocks) {
        add_error(report, CHECK_SUBJECT_HEADER, 0, "the map of the free blocks does not fit META");
        sound = false;
    }

    /*
     * A version 3 volume says where its map and its table are; the two have
     * to fill META and the table has to hold the records the header counts
     * (ADR 0038). A version 4 volume has the journal between them, and the
     * three have to fill META in the order the design gives (ADR 0047).
     */
    if (sb.version >= MFS_VERSION) {
        k_u32_t journal_blocks = (sb.version >= MFS_VERSION_JOURNAL) ? sb.journal_blocks : 0u;
        k_u32_t journal_slots = (sb.version >= MFS_VERSION_JOURNAL) ? sb.journal_slots : 0u;

        if (sb.bitmap_blocks != bitmap_blocks || sb.inode_blocks == 0u ||
            sb.bitmap_blocks + sb.inode_blocks + journal_blocks != sb.meta_blocks ||
            sb.inode_records != sb.inode_blocks * (MFS_BLOCK_SIZE / sb.inode_size)) {
            add_error(report, CHECK_SUBJECT_HEADER, 0,
                      "the map, the table and the journal do not fill META");
            sound = false;
        }

        if (sb.version >= MFS_VERSION_JOURNAL) {
            if ((sb.features & MFS_FEATURE_JOURNAL) == 0u || journal_blocks < 3u ||
                journal_slots == 0u || journal_blocks != 1u + journal_slots * 2u) {
                add_error(report, CHECK_SUBJECT_HEADER, 0, "the journal is not a journal");
                sound = false;
            }
        } else if ((sb.features & MFS_FEATURE_JOURNAL) != 0u || journal_blocks != 0u) {
            add_error(report, CHECK_SUBJECT_HEADER, 0, "a volume without the journal names one");
            sound = false;
        }
    }

    /*
     * The copy in the memory is the one of the mount. A header that is
     * sound on the device but is not that copy means that the volume
     * changed under the mount, which no operation of the filesystem does.
     */
    if (sound && memcmp(&sb, mounted, sizeof(sb)) != 0) {
        add_error(report, CHECK_SUBJECT_HEADER, 0,
                  "the device and the volume that is mounted disagree");
    }

    return MFS_OK;
}

/* --- the table of the inodes --- */

/* Take a block of the data for an object, and say when it is taken twice. */
static void claim_block(struct mfs_check_report *report, k_u32_t block)
{
    if (bit_test(s_claimed, block)) {
        add_error(report, CHECK_SUBJECT_BLOCK, block, "belongs to more than one object");
        return;
    }

    bit_set(s_claimed, block);
    report->used_blocks++;
}

/*
 * Walk the chain of an object: every block has to be a block of the data,
 * no block may be held twice, and the chain has to be long enough for the
 * bytes the record claims.
 *
 * A directory is allowed to keep blocks after its last entry: taking an
 * entry out of a directory shortens it, and the filesystem does not give
 * the blocks at the tail back. A file has no such reason, so its chain is
 * expected to end with the file.
 */
static int check_chain(const struct mfs_private *view, struct mfs_check_report *report,
                       k_u32_t index, const struct mfs_inode *inode, bool is_dir)
{
    k_u32_t data_blocks = view->sb->data_blocks;
    k_u32_t current = inode->first_block;
    k_u32_t length = 0;
    k_u64_t needed;
    k_u32_t steps = 0;

    if (current == MFS_NEXT_NONE) {
        if (inode->size > 0u) {
            add_error(report, CHECK_SUBJECT_INODE, index,
                      is_dir ? "the directory has no blocks" : "the file has no blocks");
        }
        return MFS_OK;
    }

    while (current != MFS_NEXT_NONE) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t next;
        int rc;

        if (steps++ >= data_blocks) {
            add_error(report, CHECK_SUBJECT_INODE, index, "the chain loops");
            return MFS_OK;
        }

        if (current >= data_blocks) {
            add_error(report, CHECK_SUBJECT_INODE, index, "the chain leaves the data section");
            return MFS_OK;
        }

        claim_block(report, current);
        length++;

        rc = mfs_priv_block_read(current, block);
        if (rc != MFS_OK) {
            add_error(report, CHECK_SUBJECT_BLOCK, current, "cannot be read");
            return rc;
        }

        memcpy(&next, block, sizeof(next));
        current = next;
    }

    needed = ((k_u64_t)inode->size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;

    if ((k_u64_t)length < needed) {
        add_error(report, CHECK_SUBJECT_INODE, index,
                  is_dir ? "the chain is shorter than the directory"
                         : "the chain is shorter than the file");
    } else if (!is_dir && (k_u64_t)length > needed) {
        add_error(report, CHECK_SUBJECT_INODE, index, "the file keeps blocks beyond its end");
    }

    return MFS_OK;
}

static int check_inode(const struct mfs_private *view, struct mfs_check_report *report,
                       k_u32_t index, const struct mfs_inode *inode)
{
    k_u32_t flags = inode->flags;
    bool is_dir;

    /*
     * This format has no field of its own for the mode and the owner: they
     * share the word of the flags with the type (ADR 0005), and the bit
     * between them belongs to nobody.
     */
    if ((flags & ~CHECK_FLAGS_MASK) != 0u) {
        add_error(report, CHECK_SUBJECT_INODE, index, "the reserved bits of the flags are set");
    }

    if ((flags & MFS_INODE_USED) == 0u) {
        if (flags != 0u) {
            add_error(report, CHECK_SUBJECT_INODE, index, "a record that is free carries flags");
        }
        return MFS_OK;
    }

    is_dir = (flags & MFS_INODE_DIR) != 0u;

    /*
     * A pending record is the draft of a write that a close did not
     * publish. The mount of a version 4 volume discards it before the check
     * runs, so a record that is still here is a volume that was not mounted
     * by this system (ADR 0047).
     */
    if ((flags & MFS_INODE_PENDING) != 0u) {
        kerror("mfs: fsck inode %u pending: flags 0x%x size %u first %u entries %u",
               (unsigned)index, (unsigned)flags, (unsigned)inode->size,
               (unsigned)inode->first_block, (unsigned)inode->entries);
        add_error(report, CHECK_SUBJECT_INODE, index, "is a write that a close did not publish");
    }

    if (is_dir) {
        report->directories++;
    } else {
        report->files++;
    }

    if ((k_u64_t)inode->size > (k_u64_t)view->sb->data_blocks * MFS_BLOCK_DATA) {
        add_error(report, CHECK_SUBJECT_INODE, index,
                  is_dir ? "the directory is larger than the volume"
                         : "the file is larger than the volume");
    }

    if (is_dir && inode->size > CHECK_DIR_MAX) {
        add_error(report, CHECK_SUBJECT_INODE, index,
                  "the directory is bigger than the system reads");
    }

    /*
     * A directory always has a block of its own, even when it is empty:
     * the block holds the end of its chain.
     */
    if (is_dir && inode->first_block >= view->sb->data_blocks) {
        add_error(report, CHECK_SUBJECT_INODE, index, "the directory has no block of its own");
        return MFS_OK;
    }

    return check_chain(view, report, index, inode, is_dir);
}

static int scan_inodes(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t per_block = MFS_BLOCK_SIZE / view->inode_size;
    k_u32_t walk;

    /*
     * The table is walked one block of the medium at a time, whatever the
     * order the version numbers its records in: the check of a large table
     * costs the table and not the record count (ADR 0029, ADR 0038).
     */
    for (walk = 0; walk < view->inode_blocks; walk++) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t slot;
        int rc;

        rc = mfs_priv_block_read(mfs_priv_table_block(view, walk), block);
        if (rc != MFS_OK) {
            add_error(report, CHECK_SUBJECT_BLOCK, mfs_priv_table_block(view, walk),
                      "cannot be read");
            return rc;
        }

        for (slot = 0; slot < per_block; slot++) {
            k_u32_t index = walk * per_block + slot;
            struct mfs_inode inode;
            k_u32_t where;
            k_u32_t offset;

            /* The record 0 is not used; the table may have a tail of them. */
            if (index == 0u || index > view->inode_capacity) {
                continue;
            }

            mfs_priv_inode_address(view, index, &where, &offset);
            record_decode(view, report, index, block, offset, &inode);

            if (check_inode(view, report, index, &inode) != MFS_OK) {
                return MFS_EIO;
            }
        }
    }

    return MFS_OK;
}

/* --- the map of the free blocks --- */

static void check_blocks(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t data_blocks = view->sb->data_blocks;
    k_u32_t bytes = (data_blocks + 7u) / 8u;
    k_u32_t i;

    if (bytes > s_claimed_bytes) {
        add_error(report, CHECK_SUBJECT_VOLUME, 0,
                  "the map of the free blocks is too big to check");
        return;
    }

    for (i = 0; i < data_blocks; i++) {
        bool marked = bit_test(view->bitmap, i);
        bool held = bit_test(s_claimed, i);

        if (!marked) {
            report->free_blocks++;
        }

        if (marked && !held) {
            add_error(report, CHECK_SUBJECT_BLOCK, i, "is marked used but no object owns it");
        } else if (!marked && held) {
            add_error(report, CHECK_SUBJECT_BLOCK, i, "is used by an object but marked free");
        }
    }
}

/* --- the tree of the directories --- */

/*
 * Read the content of a directory into the scratch buffer. The blocks of
 * the chain are followed the way the filesystem does it; the number of
 * bytes the chain really holds is what *out_size says, and *complete says
 * whether that is the whole of the record: a chain that ends before the
 * bytes of the record do is not an error of its own here, because the walk
 * of the records has already named it.
 */
static int dir_load(const struct mfs_private *view, const struct mfs_inode *inode,
                    k_u32_t *out_size, bool *complete)
{
    k_u32_t block = inode->first_block;
    k_u32_t offset = 0;
    k_u32_t steps = 0;

    *out_size = 0;
    *complete = true;

    /*
     * A chain that starts outside the data section is damage of the record,
     * which the walk of the records has already named: the check of the
     * tree does not ask the device for a block that is not there.
     */
    if (inode->first_block == MFS_NEXT_NONE || inode->first_block >= view->sb->data_blocks) {
        *complete = false;
        return MFS_OK;
    }

    while (offset < inode->size) {
        k_u8_t block_data[MFS_BLOCK_SIZE];
        k_size_t chunk = inode->size - offset;
        k_u32_t next;
        int rc;

        if (chunk > MFS_BLOCK_DATA) {
            chunk = MFS_BLOCK_DATA;
        }

        rc = mfs_priv_block_read(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        memcpy(view->dir_scratch + offset, block_data + 4, chunk);
        offset += (k_u32_t)chunk;

        if (offset < inode->size) {
            memcpy(&next, block_data, sizeof(next));

            if (next == MFS_NEXT_NONE || next >= view->sb->data_blocks) {
                *complete = false;
                break;
            }

            block = next;
        }

        if (++steps > view->sb->data_blocks) {
            *complete = false;
            break;
        }
    }

    *out_size = offset;

    return MFS_OK;
}

/* Two entries of a directory carry the same bytes as a name. */
static bool same_name(const k_u8_t *buf, k_size_t left, k_size_t right)
{
    k_u16_t len = entry_name_len(buf, left);

    if (len != entry_name_len(buf, right)) {
        return false;
    }

    return memcmp(buf + left + sizeof(struct mfs_dirent_header),
                  buf + right + sizeof(struct mfs_dirent_header), len) == 0;
}

/*
 * Read the entries of a directory and say whether the walk of its children
 * may trust them: a directory whose entries do not fit its own records is
 * reported, but not followed.
 */
static int validate_dir(const struct mfs_private *view, struct mfs_check_report *report,
                        k_u32_t index, const struct mfs_inode *inode, const k_u8_t *buf,
                        k_size_t size, bool *walkable)
{
    k_size_t offset = 0;
    k_u32_t count = 0;

    *walkable = true;

    while (offset + sizeof(struct mfs_dirent_header) <= size) {
        k_u16_t name_len = entry_name_len(buf, offset);
        k_u32_t child = entry_inode(buf, offset);
        k_u8_t type = buf[offset + 6];
        struct mfs_inode target;
        k_size_t previous;
        int rc;

        if (name_len == 0u || name_len > MFS_NAME_MAX ||
            offset + sizeof(struct mfs_dirent_header) + name_len > size) {
            add_error(report, CHECK_SUBJECT_INODE, index, "an entry does not fit the directory");
            *walkable = false;
            break;
        }

        count++;

        /* A name may appear once in a directory, and it is not a path. */
        for (previous = 0; previous < offset;
             previous += sizeof(struct mfs_dirent_header) + entry_name_len(buf, previous)) {
            if (same_name(buf, previous, offset)) {
                add_error(report, CHECK_SUBJECT_INODE, index, "two entries carry the same name");
                break;
            }
        }

        if (entry_has_slash(buf, offset, name_len)) {
            add_error(report, CHECK_SUBJECT_INODE, index, "a name holds a slash");
        }

        if ((type & (k_u8_t)~MFS_INODE_TYPE_MASK) != 0u) {
            add_error(report, CHECK_SUBJECT_INODE, index, "an entry carries an unknown type");
        }

        if (child == 0u || child > view->inode_capacity) {
            add_error(report, CHECK_SUBJECT_INODE, index, "an entry points outside the table");
        } else {
            rc = read_inode(view, report, child, &target);
            if (rc != MFS_OK) {
                return rc;
            }

            if ((target.flags & MFS_INODE_USED) == 0u) {
                add_error(report, CHECK_SUBJECT_INODE, index, "an entry points at a free record");
            } else if ((target.flags & (MFS_INODE_USED | MFS_INODE_DIR)) !=
                       (type & (MFS_INODE_USED | MFS_INODE_DIR))) {
                add_error(report, CHECK_SUBJECT_INODE, index,
                          "an entry and its record disagree about the type");
            }
        }

        offset += sizeof(struct mfs_dirent_header) + name_len;
    }

    if (offset != size) {
        add_error(report, CHECK_SUBJECT_INODE, index, "the entries do not fill the directory");
        *walkable = false;
    }

    if (count != inode->entries) {
        add_error(report, CHECK_SUBJECT_INODE, index,
                  "the counter of the entries does not match the entries");
    }

    return MFS_OK;
}

/*
 * Walk the tree from the root. The walk is not recursive: the frames of the
 * path live in a small array, so a damaged volume cannot take the stack of
 * the task down with a deep tree. A frame that comes back from a child
 * loads its directory again, because the scratch buffer belongs to whoever
 * is reading it at the moment.
 */
static int walk_tree(const struct mfs_private *view, struct mfs_check_report *report)
{
    struct walk_frame *stack = s_frames;
    k_u32_t visits = 0;
    k_size_t top = 0;

    memset(stack, 0, sizeof(s_frames));
    stack[0].inode = view->sb->root_inode;
    stack[0].offset = 0;
    stack[0].depth = 1;
    stack[0].size = 0;
    stack[0].loaded = false;
    stack[0].validated = false;
    stack[0].broken = false;

    for (;;) {
        struct walk_frame *frame = &stack[top];

        if (!frame->loaded) {
            struct mfs_inode inode;
            bool complete = true;
            int rc;

            rc = read_inode(view, report, frame->inode, &inode);
            if (rc != MFS_OK) {
                return rc;
            }

            if ((inode.flags & (MFS_INODE_USED | MFS_INODE_DIR)) !=
                (MFS_INODE_USED | MFS_INODE_DIR)) {
                add_error(report, CHECK_SUBJECT_INODE, frame->inode,
                          "the tree holds a record that is not a directory");
                frame->broken = true;
            } else if (inode.size > view->dir_scratch_size) {
                /* The check of the records has already named this one. */
                frame->broken = true;
            } else {
                rc = dir_load(view, &inode, &frame->size, &complete);
                if (rc != MFS_OK) {
                    return rc;
                }

                if (!complete) {
                    frame->broken = true;
                } else if (!frame->validated) {
                    bool walkable = true;

                    rc = validate_dir(view, report, frame->inode, &inode, view->dir_scratch,
                                      frame->size, &walkable);
                    if (rc != MFS_OK) {
                        return rc;
                    }

                    frame->validated = true;
                    frame->broken = !walkable;

                    /*
                     * A tree that is walked more times than the table has
                     * records is not a tree any more: a loop in it would
                     * otherwise keep the walk busy forever.
                     */
                    if (++visits > view->inode_capacity) {
                        add_error(report, CHECK_SUBJECT_INODE, frame->inode,
                                  "the tree visits more directories than the table holds");
                        return MFS_OK;
                    }
                }
            }

            frame->loaded = true;
        }

        if (frame->broken || frame->offset + sizeof(struct mfs_dirent_header) > frame->size) {
            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        {
            const k_u8_t *buf = view->dir_scratch;
            k_size_t offset = frame->offset;
            k_u16_t name_len = entry_name_len(buf, offset);
            k_u32_t child = entry_inode(buf, offset);
            k_u8_t type = buf[offset + 6];

            frame->offset += (k_u32_t)(sizeof(struct mfs_dirent_header) + name_len);

            if ((type & MFS_INODE_DIR) != 0u) {
                if (frame->depth + 1u > CHECK_DEPTH_MAX) {
                    add_error(report, CHECK_SUBJECT_INODE, child,
                              "the tree is deeper than a path can name");
                    continue;
                }

                /*
                 * The buffer belongs to the child now: the parent loads
                 * its directory again when the child is done.
                 */
                frame->loaded = false;

                top++;
                stack[top].inode = child;
                stack[top].offset = 0;
                stack[top].depth = frame->depth + 1u;
                stack[top].size = 0;
                stack[top].loaded = false;
                stack[top].validated = false;
                stack[top].broken = false;
            }
        }
    }

    return MFS_OK;
}

/* ========================================================================== */
/* the repair pass                                                            */
/* ========================================================================== */

/*
 * The repair reads the volume the way the check does, but where the check
 * stops at the first problem of a chain it writes the change the medium
 * itself proves. The rules are in mfs/mfs.h; this section is the code of
 * them and nothing else.
 */

/*
 * A chain whose head another object already holds is not this object's to
 * cut: nothing is written and the caller decides. The value is not one of
 * the errors of the filesystem, which are all negative or zero.
 */
#define REPAIR_BUSY 1

/* The records the walk of the tree has reached: a rebuild of the orphan
 * map. The map is taken for the volume of the pass and given back at its
 * end, so the repair of a table of any size costs its memory only while it
 * runs. */
static k_u8_t *s_reached;

/*
 * The walk could not be finished - a shared directory, a subtree deeper
 * than a path can name, a chain that still cannot be followed - so a block
 * or a record that no name reaches may still belong to a person's data:
 * nothing is given back in that case.
 */
static bool s_incomplete;

static int reached_alloc(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t bytes = (view->inode_capacity + 7u) / 8u;
    k_u8_t *map;

    if (bytes == 0u) {
        add_error(report, CHECK_SUBJECT_VOLUME, 0, "the table of the records has no map to repair");
        return MFS_EINVAL;
    }

    map = (k_u8_t *)kmm_alloc((k_size_t)bytes, KMM_PRIORITY_SYSTEM);
    if (map == NULL) {
        add_error(report, CHECK_SUBJECT_VOLUME, 0, "no memory for the map of the records");
        return MFS_ENOMEM;
    }

    memset(map, 0, bytes);
    s_reached = map;

    return MFS_OK;
}

static void reached_free(void)
{
    if (s_reached != NULL) {
        kmm_free(s_reached);
        s_reached = NULL;
    }
}

static bool reach_test(k_u32_t index)
{
    return (s_reached != NULL) ? bit_test(s_reached, index) : false;
}

static void reach_mark(k_u32_t index)
{
    if (s_reached != NULL) {
        bit_set(s_reached, index);
    }
}

/* A repair that the medium refused: the pass stops and says why. */
static void add_repair_error(struct mfs_check_report *report, const char *text)
{
    add_error(report, CHECK_SUBJECT_VOLUME, 0, text);
}

/*
 * Read a record of the table without saying anything about it: the repair
 * reads a record it has already checked the index of. A record that does not
 * checksum is read as a free one, the rule of read_inode(), so that the
 * entry that names it goes and its blocks come back (ADR 0038).
 */
static int read_record(const struct mfs_private *view, k_u32_t index, struct mfs_inode *out)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t where;
    k_u32_t offset;
    int rc;

    if (index == 0u || index > view->inode_capacity) {
        return MFS_EINVAL;
    }

    mfs_priv_inode_address(view, index, &where, &offset);

    rc = mfs_priv_block_read(where, block);
    if (rc != MFS_OK) {
        return rc;
    }

    mfs_priv_inode_decode(block + offset, view->inode_size, out);

    if ((out->flags & MFS_INODE_USED) != 0u &&
        !mfs_priv_inode_sound(block + offset, view->inode_size)) {
        memset(out, 0, sizeof(*out));
    }

    return MFS_OK;
}

/* Write one record back; a record written is one repair. */
static int write_record(const struct mfs_private *view, struct mfs_check_report *report,
                        k_u32_t index, const struct mfs_inode *inode)
{
    int rc = mfs_priv_inode_write(index, inode);

    if (rc != MFS_OK) {
        add_repair_error(report, "the medium did not take a repair");
        return rc;
    }

    report->fixed++;

    return MFS_OK;
}

/* End a chain at the last block the repair keeps. */
static int chain_cut(struct mfs_check_report *report, k_u32_t previous)
{
    int rc;

    if (previous == MFS_NEXT_NONE) {
        return MFS_OK; /* there is no block to end: the head was the broken one */
    }

    rc = mfs_priv_chain_set_next(previous, MFS_NEXT_NONE);
    if (rc != MFS_OK) {
        add_repair_error(report, "the medium did not take a repair");
        return rc;
    }

    report->fixed++;

    return MFS_OK;
}

/* Give up the claim of the blocks of a tail that no record keeps. */
static void unclaim_chain(const struct mfs_private *view, k_u32_t first)
{
    k_u32_t current = first;
    k_u32_t steps = 0;

    while (current != MFS_NEXT_NONE && current < view->sb->data_blocks &&
           steps++ < view->sb->data_blocks) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t next;
        int rc;

        bit_clear(s_claimed, current);

        rc = mfs_priv_block_read(current, block);
        if (rc != MFS_OK) {
            return;
        }

        memcpy(&next, block, sizeof(next));
        current = next;
    }
}

/*
 * Claim the chain of an object and make it followable: a loop, a block
 * outside the data section and a block another object already holds cut the
 * chain at the last good block, so the object keeps the data that is still
 * its own. The blocks of a cut tail are left unclaimed; the pass of the map
 * of the free blocks gives them back.
 *
 * REPAIR_BUSY is a head that another object holds: only the caller knows
 * whether the name of the object can give the chain up.
 */
static int repair_chain(const struct mfs_private *view, struct mfs_check_report *report,
                        struct mfs_inode *inode, k_u32_t *length)
{
    k_u32_t data_blocks = view->sb->data_blocks;
    k_u32_t current = inode->first_block;
    k_u32_t previous = MFS_NEXT_NONE;
    k_u32_t steps = 0;
    k_u32_t count = 0;

    *length = 0;

    if (current == MFS_NEXT_NONE) {
        return MFS_OK;
    }

    while (current != MFS_NEXT_NONE) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t next;
        int rc;

        if (current >= data_blocks || steps++ >= data_blocks) {
            rc = chain_cut(report, previous);
            if (rc != MFS_OK) {
                return rc;
            }
            break;
        }

        if (bit_test(s_claimed, current)) {
            if (previous == MFS_NEXT_NONE) {
                return REPAIR_BUSY;
            }

            rc = chain_cut(report, previous);
            if (rc != MFS_OK) {
                return rc;
            }
            break;
        }

        bit_set(s_claimed, current);
        count++;

        rc = mfs_priv_block_read(current, block);
        if (rc != MFS_OK) {
            add_repair_error(report, "the medium did not answer a read");
            return rc;
        }

        memcpy(&next, block, sizeof(next));
        previous = current;
        current = next;
    }

    *length = count;

    return MFS_OK;
}

/*
 * The size of a file against the chain the repair left: a chain shorter than
 * the file cuts the size down (the bytes were never on the medium), a chain
 * longer than the file gives the tail back (no name reaches those blocks).
 */
static int trim_file(const struct mfs_private *view, struct mfs_check_report *report,
                     struct mfs_inode *inode, k_u32_t length, k_u64_t needed, bool *changed)
{
    k_u32_t current;
    k_u32_t keep = MFS_NEXT_NONE;
    k_u32_t i;

    if (length == 0u) {
        if (inode->first_block != MFS_NEXT_NONE) {
            inode->first_block = MFS_NEXT_NONE;
            *changed = true;
        }
        if (inode->size != 0u) {
            inode->size = 0;
            *changed = true;
        }
        return MFS_OK;
    }

    if ((k_u64_t)length < needed) {
        inode->size = length * MFS_BLOCK_DATA;
        *changed = true;
        return MFS_OK;
    }

    if ((k_u64_t)length == needed) {
        return MFS_OK;
    }

    /*
     * The first `needed` blocks stay with the file and the chain ends at the
     * last of them; the rest is nobody's, and the pass of the map of the
     * free blocks takes the bits back.
     */
    current = inode->first_block;

    for (i = 0; i < (k_u32_t)needed; i++) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t next;
        int rc;

        keep = current;

        rc = mfs_priv_block_read(current, block);
        if (rc != MFS_OK) {
            add_repair_error(report, "the medium did not answer a read");
            return rc;
        }

        memcpy(&next, block, sizeof(next));
        current = next;
    }

    if (keep == MFS_NEXT_NONE) {
        inode->first_block = MFS_NEXT_NONE;
        *changed = true;
    } else {
        int rc = chain_cut(report, keep);

        if (rc != MFS_OK) {
            return rc;
        }
    }

    unclaim_chain(view, current);
    report->fixed++;

    return MFS_OK;
}

/*
 * The chain of a directory, which always has a block of its own: an empty
 * directory keeps the block that holds the end of its chain. A record whose
 * block is outside the data section has lost the place of its entries, and
 * nothing on the medium says where they were, so the directory is given an
 * empty block and its name stays.
 */
static int repair_dir_chain(const struct mfs_private *view, struct mfs_check_report *report,
                            struct mfs_inode *inode, k_u32_t *capacity, bool *changed)
{
    k_u32_t length = 0;
    int rc;

    rc = repair_chain(view, report, inode, &length);
    if (rc != MFS_OK) {
        return rc;
    }

    if (length == 0u) {
        k_u32_t block;

        rc = mfs_priv_block_alloc(&block);
        if (rc != MFS_OK) {
            add_repair_error(report, "no room for the block of a directory");
            return rc;
        }

        rc = mfs_priv_chain_set_next(block, MFS_NEXT_NONE);
        if (rc != MFS_OK) {
            mfs_priv_bitmap_set(block, false);
            add_repair_error(report, "the medium did not take a repair");
            return rc;
        }

        bit_set(s_claimed, block);
        inode->first_block = block;
        inode->size = 0;
        inode->entries = 0;
        *changed = true;
        report->fixed++;
        length = 1;
    }

    *capacity = length * MFS_BLOCK_DATA;

    /* A record cannot promise more bytes than its chain holds, and the
     * system reads four blocks of a directory and no more. */
    if (inode->size > *capacity) {
        inode->size = *capacity;
        *changed = true;
    }

    if (inode->size > CHECK_DIR_MAX) {
        inode->size = CHECK_DIR_MAX;
        *changed = true;
    }

    return MFS_OK;
}

/* Whether one of the records the repair keeps is the one of `child`. */
static bool record_kept(const k_u8_t *buf, k_size_t kept, k_u32_t child)
{
    k_size_t offset = 0;

    while (offset + sizeof(struct mfs_dirent_header) <= kept) {
        if (entry_inode(buf, offset) == child) {
            return true;
        }

        offset += sizeof(struct mfs_dirent_header) + entry_name_len(buf, offset);
    }

    return false;
}

/* Whether one of the names the repair keeps is the same as `name`. */
static bool name_kept(const k_u8_t *buf, k_size_t kept, const k_u8_t *name, k_u16_t name_len)
{
    k_size_t offset = 0;

    while (offset + sizeof(struct mfs_dirent_header) <= kept) {
        k_u16_t len = entry_name_len(buf, offset);

        if (len == name_len &&
            memcmp(buf + offset + sizeof(struct mfs_dirent_header), name, name_len) == 0) {
            return true;
        }

        offset += sizeof(struct mfs_dirent_header) + len;
    }

    return false;
}

/*
 * Walk the entries of a directory and remove what no path can reach: a name
 * that is not a name (a slash in it, the second of two equal names), an
 * entry whose record is free or outside the table. An entry whose record
 * disagrees with it about the type takes the type of the record, because the
 * record is the object and the entry is a copy of its name. The entries that
 * stay are packed at the front of the buffer, and the size of the directory
 * becomes the size of that run: the bytes after it cannot be read as an
 * entry, so they are not entries any more.
 */
static int repair_entries(const struct mfs_private *view, struct mfs_check_report *report,
                          k_size_t size, k_u32_t *out_size, k_u32_t *out_entries, bool *changed)
{
    k_u8_t *buf = view->dir_scratch;
    k_size_t offset = 0;
    k_size_t kept = 0;
    k_u32_t count = 0;

    *changed = false;

    while (offset + sizeof(struct mfs_dirent_header) <= size) {
        k_u16_t name_len = entry_name_len(buf, offset);
        k_size_t entry_len;
        k_u32_t child;
        bool remove = false;
        int rc;

        if (name_len == 0u || name_len > MFS_NAME_MAX ||
            offset + sizeof(struct mfs_dirent_header) + name_len > size) {
            break;
        }

        entry_len = sizeof(struct mfs_dirent_header) + name_len;
        child = entry_inode(buf, offset);

        if (entry_has_slash(buf, offset, name_len)) {
            remove = true;
        } else if (name_kept(buf, kept, buf + offset + sizeof(struct mfs_dirent_header),
                             name_len)) {
            remove = true;
        } else if (child == 0u || child > view->inode_capacity) {
            remove = true;
        } else {
            struct mfs_inode target;

            rc = read_record(view, child, &target);
            if (rc != MFS_OK) {
                add_repair_error(report, "the medium did not answer a read");
                return rc;
            }

            if ((target.flags & MFS_INODE_USED) == 0u) {
                remove = true;
            } else if ((target.flags & MFS_INODE_PENDING) != 0u) {
                /*
                 * The record is the draft of a write that a close did not
                 * publish: the name is not the name of a file, and the pass
                 * of the orphans frees the record and the map its blocks
                 * (ADR 0047).
                 */
                remove = true;
            } else if (record_kept(buf, kept, child)) {
                /* This format has one name per record: the second entry
                 * that names it is the damage, not a second name. */
                remove = true;
            } else {
                k_u8_t type = (k_u8_t)(target.flags & MFS_INODE_TYPE_MASK);

                if (buf[offset + 6] != type) {
                    buf[offset + 6] = type;
                    *changed = true;
                    report->fixed++;
                }

                reach_mark(child);
            }
        }

        if (remove) {
            *changed = true;
            report->fixed++;
        } else {
            if (kept != offset) {
                memmove(buf + kept, buf + offset, entry_len);
            }

            kept += entry_len;
            count++;
        }

        offset += entry_len;
    }

    if (kept != size) {
        *changed = true;
        report->fixed++;
    }

    *out_size = (k_u32_t)kept;
    *out_entries = count;

    return MFS_OK;
}

/*
 * Load one directory of the walk: its record, its chain and its entries.
 * The repaired entries are left in the scratch buffer and *out_size says
 * how many bytes of it are the directory.
 *
 * REPAIR_BUSY is a directory the repair leaves alone - its first block is
 * another object's, or its record is not the directory the walk reached it
 * as - and the subtree below it is unknown.
 */
static int repair_directory(const struct mfs_private *view, struct mfs_check_report *report,
                            k_u32_t index, bool repaired, k_u32_t *out_size)
{
    struct mfs_inode inode;
    k_u32_t capacity = 0;
    k_u32_t loaded = 0;
    k_u32_t size = 0;
    k_u32_t entries = 0;
    bool complete = true;
    bool changed = false;
    bool content = false;
    int rc;

    *out_size = 0;

    rc = read_record(view, index, &inode);
    if (rc != MFS_OK) {
        add_repair_error(report, "the medium did not answer a read");
        return rc;
    }

    reach_mark(index);

    if ((inode.flags & (MFS_INODE_USED | MFS_INODE_DIR)) != (MFS_INODE_USED | MFS_INODE_DIR)) {
        add_error(report, CHECK_SUBJECT_INODE, index, "is reached as a directory but is not one");
        return REPAIR_BUSY;
    }

    /*
     * The chain of a directory is claimed and put right the first time the
     * walk enters it; the parent loads its own directory again when a child
     * is done, and a second claim would read the blocks of the directory as
     * the blocks of another object.
     */
    if (!repaired) {
        /* The bits between the type and the mode belong to nobody (ADR 0005). */
        if ((inode.flags & ~CHECK_FLAGS_MASK) != 0u) {
            inode.flags &= CHECK_FLAGS_MASK;
            changed = true;
        }

        rc = repair_dir_chain(view, report, &inode, &capacity, &changed);
        if (rc != MFS_OK) {
            return rc;
        }
    }

    rc = dir_load(view, &inode, &loaded, &complete);
    if (rc != MFS_OK) {
        add_repair_error(report, "the medium did not answer a read");
        return rc;
    }

    if (!complete || loaded > view->dir_scratch_size) {
        /* The chain still is not the one of the record: the subtree cannot
         * be read, so its blocks are not touched. */
        return REPAIR_BUSY;
    }

    rc = repair_entries(view, report, loaded, &size, &entries, &content);
    if (rc != MFS_OK) {
        return rc;
    }

    if (content) {
        rc = mfs_priv_dir_write(&inode, view->dir_scratch, size);
        if (rc != MFS_OK) {
            add_repair_error(report, "the medium did not take a repair");
            return rc;
        }
        report->fixed++;
    }

    if (inode.size != size || inode.entries != entries || changed) {
        inode.size = size;
        inode.entries = entries;
        rc = write_record(view, report, index, &inode);
        if (rc != MFS_OK) {
            return rc;
        }
    }

    *out_size = size;

    return MFS_OK;
}

/*
 * Repair one file that a name reaches: the reserved bits of its record, its
 * chain and its size. A file whose first block another object holds gives
 * the chain up and keeps its name as an empty file, which is the rule of
 * FAT: the data stays with the name that came first.
 */
static int repair_file(const struct mfs_private *view, struct mfs_check_report *report,
                       k_u32_t index)
{
    struct mfs_inode inode;
    k_u64_t needed;
    k_u32_t length = 0;
    bool changed = false;
    int rc;

    rc = read_record(view, index, &inode);
    if (rc != MFS_OK) {
        add_repair_error(report, "the medium did not answer a read");
        return rc;
    }

    if ((inode.flags & (MFS_INODE_USED | MFS_INODE_DIR)) != MFS_INODE_USED) {
        return MFS_OK; /* not a file any more: the walk reads it again */
    }

    if ((inode.flags & ~CHECK_FLAGS_MASK) != 0u) {
        inode.flags &= CHECK_FLAGS_MASK;
        changed = true;
    }

    needed = ((k_u64_t)inode.size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;

    rc = repair_chain(view, report, &inode, &length);
    if (rc == REPAIR_BUSY) {
        inode.first_block = MFS_NEXT_NONE;
        inode.size = 0;
        changed = true;
        length = 0;
    } else if (rc != MFS_OK) {
        return rc;
    }

    rc = trim_file(view, report, &inode, length, needed, &changed);
    if (rc != MFS_OK) {
        return rc;
    }

    if (changed) {
        return write_record(view, report, index, &inode);
    }

    return MFS_OK;
}

/*
 * Walk the tree from the root and repair it the way the check walks it. The
 * frames are the ones of the check; the parent loads its directory again
 * when a child is done, because the scratch buffer belongs to whoever is
 * reading it at the moment.
 */
static int repair_tree(const struct mfs_private *view, struct mfs_check_report *report)
{
    struct walk_frame *stack = s_frames;
    k_u32_t visits = 0;
    k_size_t top = 0;

    memset(stack, 0, sizeof(s_frames));
    stack[0].inode = view->sb->root_inode;
    stack[0].depth = 1;

    for (;;) {
        struct walk_frame *frame = &stack[top];

        if (!frame->loaded) {
            bool first = !frame->repaired;
            int rc = repair_directory(view, report, frame->inode, !first, &frame->size);

            if (rc == REPAIR_BUSY) {
                s_incomplete = true;
                frame->broken = true;
            } else if (rc != MFS_OK) {
                return rc;
            } else {
                /*
                 * A directory that comes back from a child starts at the
                 * entry that follows the child: its content is the same as
                 * the first load left it, so the position is not reset.
                 */
                if (first) {
                    frame->offset = 0;
                }
                frame->broken = false;
                frame->repaired = true;
            }

            frame->loaded = true;
        }

        if (frame->broken || frame->offset + sizeof(struct mfs_dirent_header) > frame->size) {
            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        {
            const k_u8_t *buf = view->dir_scratch;
            k_size_t offset = frame->offset;
            k_u16_t name_len = entry_name_len(buf, offset);
            k_u32_t child = entry_inode(buf, offset);
            k_u8_t type = buf[offset + 6];
            int rc;

            frame->offset += (k_u32_t)(sizeof(struct mfs_dirent_header) + name_len);

            if ((type & MFS_INODE_DIR) == 0u) {
                rc = repair_file(view, report, child);
                if (rc != MFS_OK) {
                    return rc;
                }
                continue;
            }

            if (frame->depth + 1u > CHECK_DEPTH_MAX) {
                s_incomplete = true;
                continue;
            }

            /*
             * A tree that is entered more often than the table has records
             * holds a loop; the walk stops and nothing of it is given back.
             */
            if (++visits > view->inode_capacity) {
                s_incomplete = true;
                frame->broken = true;
                continue;
            }

            frame->loaded = false;

            top++;
            memset(&stack[top], 0, sizeof(stack[top]));
            stack[top].inode = child;
            stack[top].depth = frame->depth + 1u;
        }
    }

    return MFS_OK;
}

/*
 * A record of the table that no name reaches is what a create or an unlink
 * leaves when it stops between the record and the entry: the name is not on
 * the medium, so nothing can give it back. The record is freed and its
 * blocks are given back by the pass of the map of the free blocks, unless
 * another object holds them. A free record with flags in it is the wake of
 * the same kind of interruption and goes the same way.
 */
static int repair_orphans(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t per_block = MFS_BLOCK_SIZE / view->inode_size;
    k_u32_t walk;

    for (walk = 0; walk < view->inode_blocks; walk++) {
        k_u8_t block[MFS_BLOCK_SIZE];
        k_u32_t slot;

        if (mfs_priv_block_read(mfs_priv_table_block(view, walk), block) != MFS_OK) {
            add_repair_error(report, "the medium did not answer a read");
            return MFS_EIO;
        }

        for (slot = 0; slot < per_block; slot++) {
            k_u32_t index = walk * per_block + slot;
            struct mfs_inode inode;
            struct mfs_inode free_inode;
            k_u32_t where;
            k_u32_t offset;
            int rc;

            if (index == 0u || index > view->inode_capacity) {
                continue;
            }

            mfs_priv_inode_address(view, index, &where, &offset);
            mfs_priv_inode_decode(block + offset, view->inode_size, &inode);

            if ((inode.flags & MFS_INODE_USED) != 0u) {
                if (reach_test(index) && mfs_priv_inode_sound(block + offset, view->inode_size)) {
                    continue;
                }
            } else if (inode.flags == 0u) {
                continue;
            }

            memset(&free_inode, 0, sizeof(free_inode));
            rc = write_record(view, report, index, &free_inode);
            if (rc != MFS_OK) {
                return rc;
            }
        }
    }

    return MFS_OK;
}

/*
 * The map of the free blocks, after the walk of the records: a bit that no
 * object holds goes free, and a block an object holds is marked, because the
 * next writer would otherwise take it and overwrite the object.
 */
static void repair_bitmap(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t data_blocks = view->sb->data_blocks;
    k_u32_t i;

    for (i = 0; i < data_blocks; i++) {
        bool marked = bit_test(view->bitmap, i);
        bool held = bit_test(s_claimed, i);

        if (marked == held) {
            continue;
        }

        mfs_priv_bitmap_set(i, held);
        report->fixed++;
    }
}

int mfs_repair_locked(const struct mfs_private *view, struct mfs_check_report *report)
{
    struct mfs_check_report probe;
    int rc;

    /*
     * The header is not repaired. A header whose magic, version, layout or
     * checksum is not the one of the design describes a volume this system
     * does not mount, and a header that disagrees with the mounted volume
     * means the medium changed under the mount: in both cases the sections
     * under it are not the sections of this volume, and nothing is written.
     */
    memset(&probe, 0, sizeof(probe));
    check_header(view, &probe);
    if (probe.errors != 0) {
        return MFS_OK;
    }

    /*
     * The map of the blocks the walk claims and the map of the records it
     * reaches are taken for this volume and given back at the end: a repair
     * of a big volume costs its memory only while it runs, the same way the
     * check does.
     */
    rc = claimed_alloc(view, report);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = reached_alloc(view, report);
    if (rc != MFS_OK) {
        claimed_free();
        return rc;
    }

    s_incomplete = false;

    rc = repair_tree(view, report);

    /*
     * The records and the blocks that no name reaches are given back only
     * when the tree was walked whole: a shared directory or a subtree the
     * walk refused to enter makes a block of a person's data look like an
     * orphan, and freeing it would destroy what the person could still
     * recover by fixing the entry by hand.
     */
    if (rc == MFS_OK && !s_incomplete) {
        rc = repair_orphans(view, report);
    }

    if (rc == MFS_OK && !s_incomplete) {
        repair_bitmap(view, report);
    }

    if (rc == MFS_OK) {
        rc = mfs_priv_sync();
        if (rc != MFS_OK) {
            add_repair_error(report, "the medium did not take a repair");
        }
    }

    reached_free();
    claimed_free();

    return rc;
}

/* --- the entry point --- */

int mfs_check_locked(const struct mfs_private *view, struct mfs_check_report *report)
{
    k_u32_t journal;
    int rc;

    /*
     * The journal of a version 4 volume is replayed by the mount, so a
     * committed transaction here means that the medium changed under the
     * volume that is mounted: the check names it instead of walking a state
     * the mount does not have (ADR 0047).
     */
    journal = mfs_priv_journal_pending();

    if (journal != 0u) {
        add_error(report, CHECK_SUBJECT_VOLUME, journal,
                  "the journal holds a transaction the mount did not replay");
    }

    rc = claimed_alloc(view, report);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = check_header(view, report);
    if (rc == MFS_OK) {
        rc = scan_inodes(view, report);

        if (rc == MFS_OK) {
            check_blocks(view, report);
            rc = walk_tree(view, report);
        }
    }

    claimed_free();

    return rc;
}
