/*
 * Micro OS — the check of a FAT volume.
 *
 * A FAT volume has no journal and no second copy of anything but its table
 * of the clusters, so a power cut in the middle of a write, or another
 * system that did not finish, leaves whatever it leaves. The check answers
 * the question of a person who is about to trust a card with a file: is the
 * boot sector, the table, the directories and the free map still one story?
 *
 * Nothing here writes. The volume is only read, and the report names the
 * first problem and counts the rest. A volume that is reported as damaged
 * is left exactly the way it was found, so that it can be copied off the
 * medium before anything touches it.
 *
 * The clusters that the walk has claimed live in one static map of bits. A
 * volume with more clusters than the map covers is refused with a message
 * instead of being walked with a map that would hide a cross-link; 65536
 * clusters are the whole of FAT12 and FAT16 and 8 KiB of the static RAM of
 * the reader.
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/errno.h"

#define FAT_DIR_ENTRY 32u
#define FAT_BOOT_SIGNATURE 0xAA55u

/* The clusters this check can claim, one bit each: 65536 is 8 KiB here. */
#define FAT_CHECK_CLUSTERS_MAX 65536u

/* The directories the walk of the tree can keep open on the way down. */
#define FAT_CHECK_DEPTH_MAX 64u

/* The bytes of the tables that a comparison reads at one call. */
#define FAT_CHECK_CHUNK 128u

/* What the first problem of the report is about. */
enum check_subject {
    CHECK_SUBJECT_BOOT,
    CHECK_SUBJECT_TABLE,
    CHECK_SUBJECT_CLUSTER,
    CHECK_SUBJECT_DIR,
    CHECK_SUBJECT_VOLUME,
};

/* One run of long-name entries that the scanner of a directory is building. */
struct lfn_run {
    bool active;
    k_u8_t checksum;
    k_u8_t next; /* the sequence number the next entry has to carry */
};

struct check_state {
    struct fat_check_report *report;
    k_u8_t claimed[(FAT_CHECK_CLUSTERS_MAX + 7u) / 8u];
};

/*
 * The state of the check. One volume is checked at a time, as MicroFS has
 * one volume and the check of its own uses one static map; the caller holds
 * the lock of the volume, so two tasks do not walk the same volume at once.
 */
static struct check_state s_check;

/* --- the report --- */

static void msg_text(struct fat_check_report *report, const char *text)
{
    k_size_t len = strlen(report->first);

    while (*text != '\0' && len + 1 < FAT_CHECK_MESSAGE_MAX) {
        report->first[len++] = *text++;
    }

    report->first[len] = '\0';
}

static void msg_number(struct fat_check_report *report, k_u32_t value)
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
 * Count a problem. Only the first one is written down: a person reads that
 * one first, and the rest of the report is the counters.
 */
static void add_error(struct check_state *state, enum check_subject subject, k_u32_t number,
                      const char *text)
{
    struct fat_check_report *report = state->report;

    if (report->errors == 0) {
        switch (subject) {
        case CHECK_SUBJECT_BOOT:
            msg_text(report, "boot sector: ");
            break;
        case CHECK_SUBJECT_TABLE:
            msg_text(report, "FAT copy ");
            msg_number(report, number);
            msg_text(report, ": ");
            break;
        case CHECK_SUBJECT_CLUSTER:
            msg_text(report, "cluster ");
            msg_number(report, number);
            msg_text(report, ": ");
            break;
        case CHECK_SUBJECT_DIR:
            if (number == 0u) {
                msg_text(report, "root: ");
            } else {
                msg_text(report, "directory ");
                msg_number(report, number);
                msg_text(report, ": ");
            }
            break;
        case CHECK_SUBJECT_VOLUME:
            msg_text(report, "volume: ");
            break;
        }
        msg_text(report, text);
    }

    report->errors++;
}

/* --- the cluster map --- */

static bool claimed_test(const struct check_state *state, k_u32_t cluster)
{
    k_u32_t index = cluster - 2u;

    return (state->claimed[index / 8u] & (k_u8_t)(1u << (index % 8u))) != 0u;
}

/* Take a cluster for a chain; false when another link already has it. */
static bool claimed_take(struct check_state *state, k_u32_t cluster)
{
    k_u32_t index = cluster - 2u;
    k_u8_t bit = (k_u8_t)(1u << (index % 8u));

    if ((state->claimed[index / 8u] & bit) != 0u) {
        return false;
    }

    state->claimed[index / 8u] |= bit;
    return true;
}

/* --- the values of one cluster --- */

/* The first value that ends a chain on this volume. */
static k_u32_t eoc_min(const struct fat_volume *vol)
{
    switch (vol->type) {
    case 12:
        return 0x0FF8u;
    case 16:
        return 0xFFF8u;
    default:
        return 0x0FFFFFF8u;
    }
}

/* The mark of a cluster the medium has retired. */
static k_u32_t bad_mark(const struct fat_volume *vol)
{
    switch (vol->type) {
    case 12:
        return 0x0FF7u;
    case 16:
        return 0xFFF7u;
    default:
        return 0x0FFFFFF7u;
    }
}

static bool cluster_in_volume(const struct fat_volume *vol, k_u32_t cluster)
{
    return cluster >= 2u && cluster < vol->cluster_count + 2u;
}

static int cluster_value(struct check_state *state, struct fat_volume *vol, k_u32_t cluster,
                         k_u32_t *value)
{
    int rc = fat_entry_get(vol, cluster, value);

    if (rc != 0) {
        add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "cannot be read");
    }

    return rc;
}

/*
 * Whether `target` was already visited by the chain that starts at `first`
 * before the link that names it now. The walk is repeated only when a
 * cluster is met twice, so a sound volume never pays for it: a loop is a
 * repeat inside one chain, a cross-link a repeat from another one.
 */
static bool visited_in_chain(struct fat_volume *vol, k_u32_t first, k_u32_t target)
{
    k_u32_t cluster = first;
    k_u32_t steps = 0;

    for (;;) {
        k_u32_t value;

        if (cluster == target) {
            return true;
        }

        if (fat_entry_get(vol, cluster, &value) != 0) {
            return false;
        }

        if (value == 0 || value == bad_mark(vol) || value >= eoc_min(vol)) {
            return false;
        }

        if (value < 2u || value >= vol->cluster_count + 2u) {
            return false;
        }

        if (value == target) {
            /* The link under the question already points at the target:
             * the cluster was not visited, it is the next one. */
            return false;
        }

        cluster = value;

        if (++steps > vol->cluster_count) {
            return false;
        }
    }
}

/*
 * Claim every cluster of a chain and say whether it is whole. The chain has
 * to stay inside the volume, no cluster may be free, the mark of a bad
 * cluster may not stand in it, and a cluster may not be named twice.
 */
static bool walk_chain(struct check_state *state, struct fat_volume *vol, k_u32_t first,
                       k_u32_t *length)
{
    k_u32_t cluster = first;
    k_u32_t steps = 0;
    k_u32_t count = 0;

    *length = 0;

    if (!cluster_in_volume(vol, first)) {
        add_error(state, CHECK_SUBJECT_CLUSTER, first, "is outside the volume");
        return false;
    }

    for (;;) {
        k_u32_t value;

        if (!claimed_take(state, cluster)) {
            if (visited_in_chain(vol, first, cluster)) {
                add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "the chain loops");
            } else {
                add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "belongs to more than one chain");
            }
            return false;
        }

        count++;

        if (cluster_value(state, vol, cluster, &value) != 0) {
            return false;
        }

        if (value == 0u) {
            add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "is free but a chain names it");
            return false;
        }

        if (value == bad_mark(vol)) {
            add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "is a bad cluster inside a chain");
            return false;
        }

        if (value >= eoc_min(vol)) {
            break;
        }

        if (!cluster_in_volume(vol, value)) {
            add_error(state, CHECK_SUBJECT_CLUSTER, value, "is outside the volume");
            return false;
        }

        cluster = value;

        if (++steps > vol->cluster_count) {
            add_error(state, CHECK_SUBJECT_CLUSTER, cluster, "the chain loops");
            return false;
        }
    }

    *length = count;
    return true;
}

/* --- the boot sector --- */

/*
 * The mount has already parsed the boot sector, so this call asks whether
 * the bytes on the medium still describe the volume that is mounted. The
 * answer matters when a volume was mounted and another system wrote it
 * afterwards; a volume the check mounts itself cannot disagree yet.
 */
static void check_boot(struct check_state *state, struct fat_volume *vol)
{
    k_u8_t boot[512];
    k_u32_t total;
    k_u32_t fat_sectors;

    if (fat_read_volume(vol, 0, boot, sizeof(boot)) != 0) {
        add_error(state, CHECK_SUBJECT_BOOT, 0, "cannot be read");
        return;
    }

    if (fat_le16(boot + 510) != FAT_BOOT_SIGNATURE) {
        add_error(state, CHECK_SUBJECT_BOOT, 0, "the signature is not 55 AA");
        return;
    }

    total = (fat_le16(boot + 19) != 0u) ? fat_le16(boot + 19) : fat_le32(boot + 32);
    fat_sectors = (fat_le16(boot + 22) != 0u) ? fat_le16(boot + 22) : fat_le32(boot + 36);

    if (fat_le16(boot + 11) != vol->bytes_per_sector || boot[13] != vol->sectors_per_cluster ||
        fat_le16(boot + 14) != vol->reserved_sectors || boot[16] != vol->num_fats ||
        fat_le16(boot + 17) != vol->root_entries || total != vol->total_sectors ||
        fat_sectors != vol->fat_sectors) {
        add_error(state, CHECK_SUBJECT_BOOT, 0, "the BPB and the mounted volume disagree");
        return;
    }

    if (vol->type == 32 && fat_le32(boot + 44) != vol->root_cluster) {
        add_error(state, CHECK_SUBJECT_BOOT, 0, "the root cluster and the volume disagree");
    }
}

/* --- the copies of the table --- */

/* A volume with two tables is a volume whose copies have to say one thing. */
static void check_tables(struct check_state *state, struct fat_volume *vol)
{
    k_u64_t fat_bytes = (k_u64_t)vol->fat_sectors * vol->bytes_per_sector;
    k_u64_t base = (k_u64_t)vol->reserved_sectors * vol->bytes_per_sector;
    k_u8_t first[FAT_CHECK_CHUNK];
    k_u8_t other[FAT_CHECK_CHUNK];
    k_u32_t copy;

    for (copy = 1u; copy < vol->num_fats; copy++) {
        k_u64_t offset = 0;

        while (offset < fat_bytes) {
            k_size_t chunk = FAT_CHECK_CHUNK;

            if ((k_u64_t)chunk > fat_bytes - offset) {
                chunk = (k_size_t)(fat_bytes - offset);
            }

            if (fat_read_volume(vol, base + offset, first, chunk) != 0 ||
                fat_read_volume(vol, base + (k_u64_t)copy * fat_bytes + offset, other, chunk) !=
                    0) {
                add_error(state, CHECK_SUBJECT_TABLE, copy + 1u, "cannot be read");
                break;
            }

            if (memcmp(first, other, chunk) != 0) {
                add_error(state, CHECK_SUBJECT_TABLE, copy + 1u, "differs from the first copy");
                break;
            }

            offset += chunk;
        }
    }
}

/* --- the entries of the directories --- */

static k_u8_t short_checksum(const k_u8_t entry[FAT_DIR_ENTRY])
{
    k_u8_t sum = 0;
    k_size_t i;

    for (i = 0; i < 11u; i++) {
        sum = (k_u8_t)(((sum & 1u) << 7) + (sum >> 1) + entry[i]);
    }

    return sum;
}

static void lfn_reset(struct lfn_run *run)
{
    run->active = false;
    run->checksum = 0;
    run->next = 0;
}

/*
 * Collect one long-name entry. The run is written from its last entry down,
 * so the sequence numbers fall by one and the checksum of the short name is
 * the same in every entry. A run that does not follow those rules is broken:
 * the reader falls back to the 8.3 name, and the check says so.
 */
static void lfn_feed(struct check_state *state, k_u32_t dir, struct lfn_run *run,
                     const k_u8_t entry[FAT_DIR_ENTRY])
{
    k_u8_t sequence = (k_u8_t)(entry[0] & 0x1Fu);
    bool last = (entry[0] & 0x40u) != 0u;

    if (sequence == 0u || sequence > FAT_LFN_ENTRIES) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "a long-name entry has a bad sequence number");
        lfn_reset(run);
        return;
    }

    if (last) {
        lfn_reset(run);
        run->active = true;
        run->checksum = entry[13];
        run->next = (k_u8_t)(sequence - 1u);
        return;
    }

    if (!run->active || sequence != run->next) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "a run of long-name entries is broken");
        lfn_reset(run);
        return;
    }

    if (entry[13] != run->checksum) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "the long-name entries do not carry one checksum");
        lfn_reset(run);
        return;
    }

    run->next = (k_u8_t)(sequence - 1u);
}

/*
 * The long name is whole when the run reached sequence one and its checksum
 * is the one of the short entry that follows. The reader would silently use
 * the 8.3 name of a broken run, which is why the check is not optional.
 */
static void lfn_close(struct check_state *state, k_u32_t dir, struct lfn_run *run,
                      const k_u8_t entry[FAT_DIR_ENTRY])
{
    if (!run->active) {
        return;
    }

    if (run->next != 0u) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "a long name misses an entry");
    } else if (run->checksum != short_checksum(entry)) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "a long name does not match its short entry");
    }

    lfn_reset(run);
}

/* The offset of the first byte of a cluster in the data section. */
static k_u64_t cluster_offset(const struct fat_volume *vol, k_u32_t cluster)
{
    return (k_u64_t)(cluster - 2u) * vol->cluster_bytes +
           (k_u64_t)vol->first_data_sector * vol->bytes_per_sector;
}

/*
 * The first cluster and the size of an entry. FAT12/16 keep the high word of
 * the cluster in a field that belongs to nobody, so it is read only where
 * the format defines it.
 */
static k_u32_t entry_cluster(const struct fat_volume *vol, const k_u8_t entry[FAT_DIR_ENTRY])
{
    k_u32_t cluster = fat_le16(entry + 26);

    if (vol->type == 32) {
        cluster |= (k_u32_t)fat_le16(entry + 20) << 16;
    }

    return cluster;
}

/* --- the tree of the directories --- */

/* One directory of the path that the check is walking. */
struct walk_frame {
    k_u32_t first;   /* the first cluster, 0 for the fixed root */
    k_u32_t cluster; /* the cluster of the chain being read */
    k_u32_t offset;  /* the next entry inside the cluster, or the root */
    bool loaded;
    struct lfn_run lfn;
};

/*
 * Read the next entry of a frame. The chain of the directory was walked and
 * claimed when the tree reached it, so a value that cannot be followed here
 * means the medium changed under the check; the directory ends and the walk
 * of the tree says nothing about it twice.
 */
static bool frame_entry(struct fat_volume *vol, struct walk_frame *frame,
                        k_u8_t entry[FAT_DIR_ENTRY], bool *end)
{
    *end = false;

    if (frame->first == 0u) {
        if (frame->offset + FAT_DIR_ENTRY > vol->root_entries * FAT_DIR_ENTRY) {
            *end = true;
            return false;
        }

        if (fat_read_volume(vol,
                            (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + frame->offset,
                            entry, FAT_DIR_ENTRY) != 0) {
            return false;
        }

        frame->offset += FAT_DIR_ENTRY;
        return true;
    }

    if (frame->offset + FAT_DIR_ENTRY > vol->cluster_bytes) {
        k_u32_t value;

        if (fat_entry_get(vol, frame->cluster, &value) != 0 || value == 0u ||
            value >= eoc_min(vol) || !cluster_in_volume(vol, value)) {
            *end = true;
            return false;
        }

        frame->cluster = value;
        frame->offset = 0;
    }

    if (fat_read_volume(vol, cluster_offset(vol, frame->cluster) + frame->offset, entry,
                        FAT_DIR_ENTRY) != 0) {
        return false;
    }

    frame->offset += FAT_DIR_ENTRY;
    return true;
}

/*
 * Check one short entry and count it. A file says how many clusters its chain
 * has to hold; a directory has no size of its own, so its chain is whatever
 * it is, as long as its cluster is inside the volume. A directory hands its
 * first cluster back to the walk, which claims the chain on the way down.
 */
static void check_entry(struct check_state *state, struct fat_volume *vol, k_u32_t dir,
                        const k_u8_t entry[FAT_DIR_ENTRY], k_u32_t *child, bool *has_child)
{
    k_u32_t cluster = entry_cluster(vol, entry);
    k_u32_t size = fat_le32(entry + 28);
    bool is_dir = (entry[11] & FAT_ATTR_DIR) != 0u;

    *has_child = false;

    if (entry[0] == '.') {
        return; /* the dot entries name the directory itself and its parent */
    }

    if (!is_dir) {
        state->report->files++;
    } else {
        state->report->directories++;
    }

    if (!is_dir && size == 0u) {
        if (cluster != 0u) {
            add_error(state, CHECK_SUBJECT_DIR, dir, "an empty file names a cluster");
        }
        return;
    }

    if (cluster < 2u) {
        add_error(state, CHECK_SUBJECT_DIR, dir,
                  is_dir ? "a directory has no cluster" : "a file with bytes has no cluster");
        return;
    }

    if (!cluster_in_volume(vol, cluster)) {
        add_error(state, CHECK_SUBJECT_DIR, dir, "an entry names a cluster outside the volume");
        return;
    }

    if (is_dir) {
        *child = cluster;
        *has_child = true;
        return;
    }

    {
        k_u32_t length = 0;
        k_u32_t needed = (k_u32_t)(((k_u64_t)size + vol->cluster_bytes - 1u) / vol->cluster_bytes);

        if (!walk_chain(state, vol, cluster, &length)) {
            return;
        }

        if (length < needed) {
            add_error(state, CHECK_SUBJECT_DIR, dir, "the chain is shorter than the file");
        } else if (length > needed) {
            add_error(state, CHECK_SUBJECT_DIR, dir, "the file keeps clusters beyond its end");
        }
    }
}

/*
 * Walk the tree from the root, one directory at a time. The frames of the
 * path live in a small array, so a damaged volume cannot take the stack of
 * the task down with a deep tree; a directory is claimed when the walk goes
 * down into it, so a loop in the tree is reported by the walk of its chain
 * and is not followed twice.
 */
static void check_tree(struct check_state *state, struct fat_volume *vol)
{
    static struct walk_frame stack[FAT_CHECK_DEPTH_MAX];
    k_size_t top = 0;
    bool depth_reported = false;

    memset(&stack[0], 0, sizeof(stack[0]));
    stack[0].first = (vol->type == 32) ? vol->root_cluster : 0u;
    stack[0].cluster = stack[0].first;
    lfn_reset(&stack[0].lfn);

    for (;;) {
        struct walk_frame *frame = &stack[top];
        k_u8_t entry[FAT_DIR_ENTRY];
        k_u32_t label = (top == 0u) ? 0u : frame->first;
        bool end = false;

        if (!frame->loaded) {
            if (frame->first != 0u) {
                k_u32_t length = 0;

                if (!walk_chain(state, vol, frame->first, &length)) {
                    if (top == 0u) {
                        break;
                    }
                    top--;
                    continue;
                }

                frame->cluster = frame->first;
            }

            frame->offset = 0;
            frame->loaded = true;
        }

        if (!frame_entry(vol, frame, entry, &end)) {
            if (!end) {
                add_error(state, CHECK_SUBJECT_DIR, label, "an entry cannot be read");
            }
            if (frame->lfn.active) {
                add_error(state, CHECK_SUBJECT_DIR, label,
                          "a long name ends outside the directory");
            }
            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        if (entry[0] == 0x00) {
            if (frame->lfn.active) {
                add_error(state, CHECK_SUBJECT_DIR, label,
                          "a long name ends outside the directory");
            }
            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        if (entry[0] == 0xE5) {
            lfn_reset(&frame->lfn);
            continue;
        }

        if (entry[11] == FAT_ATTR_LFN) {
            lfn_feed(state, label, &frame->lfn, entry);
            continue;
        }

        lfn_close(state, label, &frame->lfn, entry);

        if ((entry[11] & FAT_ATTR_VOLUME) != 0u) {
            continue;
        }

        {
            k_u32_t child = 0;
            bool has_child = false;

            check_entry(state, vol, label, entry, &child, &has_child);
            if (!has_child) {
                continue;
            }

            if (top + 1u >= FAT_CHECK_DEPTH_MAX) {
                if (!depth_reported) {
                    add_error(state, CHECK_SUBJECT_DIR, label,
                              "the tree is deeper than a path can name");
                    depth_reported = true;
                }
                continue;
            }

            top++;
            memset(&stack[top], 0, sizeof(stack[top]));
            stack[top].first = child;
            stack[top].cluster = child;
            lfn_reset(&stack[top].lfn);
        }
    }
}

/* --- the free map --- */

/*
 * A cluster that is neither free nor held by a chain is space no file will
 * ever get back: the writer only takes the zero entries of the table. The
 * count of the free clusters is what `df` shows, and the lost ones are a
 * problem of the volume.
 */
static void count_clusters(struct check_state *state, struct fat_volume *vol)
{
    k_u32_t cluster;

    for (cluster = 2u; cluster < vol->cluster_count + 2u; cluster++) {
        k_u32_t value;

        if (cluster_value(state, vol, cluster, &value) != 0) {
            return;
        }

        if (value == 0u) {
            state->report->free_clusters++;
            continue;
        }

        if (value == bad_mark(vol)) {
            continue;
        }

        if (!claimed_test(state, cluster)) {
            state->report->lost_clusters++;
            add_error(state, CHECK_SUBJECT_CLUSTER, cluster,
                      "is marked used but no chain names it");
        }
    }
}

/* --- the entry point --- */

int fat_check_volume(struct fat_volume *vol, struct fat_check_report *report)
{
    if (vol == NULL || report == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    memset(report, 0, sizeof(*report));
    memset(&s_check, 0, sizeof(s_check));
    s_check.report = report;

    report->type = vol->type;
    report->clusters = vol->cluster_count;

    if (vol->cluster_count > FAT_CHECK_CLUSTERS_MAX) {
        add_error(&s_check, CHECK_SUBJECT_VOLUME, 0,
                  "the volume has more clusters than the check can claim");
        return 0;
    }

    check_boot(&s_check, vol);
    check_tables(&s_check, vol);
    check_tree(&s_check, vol);
    count_clusters(&s_check, vol);

    return 0;
}

int fat_check_device(const struct k_blockdev *dev, struct fat_check_report *report)
{
    static struct fat_volume volume;
    int rc;

    if (dev == NULL || report == NULL) {
        return KERR_INVAL;
    }

    memset(report, 0, sizeof(*report));

    rc = fat_volume_mount(&volume, dev);
    if (rc != 0) {
        report->errors = 1;
        msg_text(report, "boot sector: ");
        msg_text(report, (rc == KERR_IO)       ? "the medium did not answer"
                         : (rc == KERR_NOTSUP) ? "the geometry is not one this reader speaks"
                                               : "the volume is not FAT12/16/32");
        return 0;
    }

    if (k_mutex_lock(&volume.lock) != 0) {
        fat_volume_unmount(&volume);
        return KERR_NOMEM;
    }

    rc = fat_check_volume(&volume, report);

    k_mutex_unlock(&volume.lock);
    fat_volume_unmount(&volume);

    return rc;
}
