/*
 * Micro OS — the check and the repair of a FAT volume.
 *
 * A FAT volume has no journal and no second copy of anything but its table
 * of the clusters, so a power cut in the middle of a write, or another
 * system that did not finish, leaves whatever it leaves. The check answers
 * the question of a person who is about to trust a card with a file: is the
 * boot sector, the table, the directories and the free map still one story?
 *
 * Without `write` nothing here writes. The volume is only read, and the
 * report names the first problem and counts the rest. A volume that is
 * reported as damaged is left exactly the way it was found, so that it can
 * be copied off the medium before anything touches it.
 *
 * With `write` the repair pass fixes the damage the medium itself explains:
 * a cluster no chain names goes back to the free map, a chain is cut where
 * it loops or leaves the volume, the second of two names of one cluster
 * gives it up, a broken run of long names is removed, and a size that does
 * not match its chain is put right. It does not guess: a boot sector that
 * disagrees with the mount, two table copies that differ and a directory
 * whose head another chain owns are left for a person. The check runs once
 * more after the pass, so a repaired volume is proven whole and not
 * assumed whole.
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
    k_u8_t next;        /* the sequence number the next entry has to carry */
    bool broken;        /* the repair's view: the run cannot be kept as it is */
    k_u32_t first_slot; /* the slot of the first entry of the run */
    k_u32_t count;      /* the entries of the run, for the repair to remove */
};

struct check_state {
    struct fat_check_report *report;
    bool write;      /* the repair pass: a write that fails stops the pass */
    bool incomplete; /* the repair could not walk the whole tree */
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

/*
 * The characters an 8.3 name may not hold. A name with one of them is not
 * a name another system wrote; it is a slot of a directory that a crash or
 * a wild writer left behind, and the reader would present it as a file
 * whose name cannot be typed. The check names it and the repair removes the
 * entry, because there is no name to put back.
 */
static bool short_name_valid(const k_u8_t entry[FAT_DIR_ENTRY])
{
    static const char forbidden[] = "\"*+,/:;<=>?[\\]|";
    k_size_t i;

    if (entry[0] == 0x20u) {
        return false;
    }

    for (i = 0; i < 11u; i++) {
        /* 0x05 in the first byte is the escape of a name that begins with
         * 0xE5, which is how FAT writes a byte a deleted marker would hide. */
        if (i == 0u && entry[i] == 0x05u) {
            continue;
        }
        if (entry[i] < 0x20u || strchr(forbidden, (char)entry[i]) != NULL) {
            return false;
        }
    }

    return true;
}

static void lfn_reset(struct lfn_run *run)
{
    run->active = false;
    run->checksum = 0;
    run->next = 0;
    run->broken = false;
    run->first_slot = 0;
    run->count = 0;
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
    k_u32_t slot;    /* the index of the next entry in the directory */
    bool loaded;
    struct lfn_run lfn;
};

/*
 * Read the next entry of a frame. The chain of the directory was walked and
 * claimed when the tree reached it, so a value that cannot be followed here
 * means the medium changed under the check; the directory ends and the walk
 * of the tree says nothing about it twice. `at` is the byte of the volume
 * the entry was read from, which is where a repair writes it back.
 */
static bool frame_entry(struct fat_volume *vol, struct walk_frame *frame,
                        k_u8_t entry[FAT_DIR_ENTRY], k_u64_t *at, bool *end)
{
    *end = false;

    if (frame->first == 0u) {
        if (frame->offset + FAT_DIR_ENTRY > vol->root_entries * FAT_DIR_ENTRY) {
            *end = true;
            return false;
        }

        if (at != NULL) {
            *at = (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + frame->offset;
        }

        if (fat_read_volume(vol,
                            (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + frame->offset,
                            entry, FAT_DIR_ENTRY) != 0) {
            return false;
        }

        frame->offset += FAT_DIR_ENTRY;
        frame->slot++;
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

    if (at != NULL) {
        *at = cluster_offset(vol, frame->cluster) + frame->offset;
    }

    if (fat_read_volume(vol, cluster_offset(vol, frame->cluster) + frame->offset, entry,
                        FAT_DIR_ENTRY) != 0) {
        return false;
    }

    frame->offset += FAT_DIR_ENTRY;
    frame->slot++;
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

    if (!short_name_valid(entry)) {
        add_error(state, CHECK_SUBJECT_DIR, dir,
                  "a short name holds a character FAT does not allow");
        return;
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

        if (!frame_entry(vol, frame, entry, NULL, &end)) {
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

/* --- the repair pass --- */

/*
 * The repair reads the volume exactly the way the check does, but where the
 * check stops at the first problem of a chain the repair writes the change
 * that the medium itself explains. The two walks share the entry reader and
 * the map of claimed clusters; the rules of the write are in ADR 0009.
 */

static void claimed_release(struct check_state *state, k_u32_t cluster)
{
    k_u32_t index = cluster - 2u;

    state->claimed[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));
}

/* The mark the repair leaves at the end of a chain: the one the writer of
 * the filesystem leaves, not the smallest value a reader would accept. */
static k_u32_t end_mark(const struct fat_volume *vol)
{
    switch (vol->type) {
    case 12:
        return 0x0FFFu;
    case 16:
        return 0xFFFFu;
    default:
        return 0x0FFFFFFFu;
    }
}

static k_u32_t end_value(const struct fat_volume *vol)
{
    return end_mark(vol);
}

/* Write one entry back. A medium that does not answer stops the pass. */
static int entry_write(struct check_state *state, struct fat_volume *vol, k_u64_t at,
                       const k_u8_t entry[FAT_DIR_ENTRY])
{
    int rc = fat_write_volume(vol, at, entry, FAT_DIR_ENTRY);

    if (rc != 0) {
        add_error(state, CHECK_SUBJECT_VOLUME, 0, "the medium did not take a repair");
    }

    return rc;
}

/* Remove one entry: the marker E5 is the only thing a removal writes. */
static int entry_delete(struct check_state *state, struct fat_volume *vol, k_u64_t at)
{
    k_u8_t entry[FAT_DIR_ENTRY];
    int rc = fat_read_volume(vol, at, entry, FAT_DIR_ENTRY);

    if (rc != 0) {
        add_error(state, CHECK_SUBJECT_VOLUME, 0, "the medium did not answer");
        return rc;
    }

    if (entry[0] == 0xE5u) {
        return 0;
    }

    entry[0] = 0xE5u;
    rc = entry_write(state, vol, at, entry);
    if (rc == 0) {
        state->report->fixed++;
    }

    return rc;
}

/* The length of the chain from `first` to `target` inclusive, or 0 when
 * `target` is not met before `stop`. The walk is bounded by the volume. */
static k_u32_t chain_length_to(struct fat_volume *vol, k_u32_t first, k_u32_t target, k_u32_t stop)
{
    k_u32_t cluster = first;
    k_u32_t index = 0;
    k_u32_t steps = 0;

    while (steps <= vol->cluster_count) {
        k_u32_t value;

        if (cluster == stop) {
            return 0;
        }

        if (cluster == target) {
            return index + 1u;
        }

        if (fat_entry_get(vol, cluster, &value) != 0) {
            return 0;
        }

        if (!cluster_in_volume(vol, value)) {
            return 0;
        }

        cluster = value;
        index++;
        steps++;
    }

    return 0;
}

/* The clusters of a cut tail are nobody's any more. They are not written
 * here: the pass of the lost clusters gives them back, and counts them. */
static void unclaim_tail(struct check_state *state, struct fat_volume *vol, k_u32_t start,
                         k_u32_t stop)
{
    k_u32_t cluster = start;
    k_u32_t steps = 0;

    while (cluster_in_volume(vol, cluster) && cluster != stop && steps <= vol->cluster_count) {
        k_u32_t next;

        if (fat_entry_get(vol, cluster, &next) != 0) {
            return;
        }

        claimed_release(state, cluster);
        cluster = next;
        steps++;
    }
}

/*
 * Claim a chain and make it whole: a cluster that is free, retired or
 * outside the volume cuts the chain before it, a loop cuts it at the first
 * time the repeated cluster was named, and a cluster another chain already
 * holds cuts it before that cluster. The clusters of a cut tail are given
 * back to the lost pass. The value left in *length is the length of the
 * chain after the repair, which is what the size of a file is measured
 * against.
 *
 * KERR_BUSY is a head that another chain holds: only the caller knows
 * whether an entry can give its claim up. Every other error is a medium
 * that did not answer, and the pass stops.
 */
static int repair_chain(struct check_state *state, struct fat_volume *vol, k_u32_t first,
                        k_u32_t *length)
{
    k_u32_t cluster = first;
    k_u32_t count = 1;
    int rc;

    *length = 0;

    if (!cluster_in_volume(vol, first)) {
        return KERR_INVAL;
    }

    if (!claimed_take(state, first)) {
        return KERR_BUSY;
    }

    for (;;) {
        k_u32_t value;

        rc = fat_entry_get(vol, cluster, &value);
        if (rc != 0) {
            return rc;
        }

        if (value >= eoc_min(vol)) {
            break; /* the chain ended where it should */
        }

        if (value == 0u || value == bad_mark(vol)) {
            rc = fat_entry_set(vol, cluster, end_value(vol));
            if (rc != 0) {
                return rc;
            }
            state->report->fixed++;
            break;
        }

        if (!cluster_in_volume(vol, value)) {
            rc = fat_entry_set(vol, cluster, end_value(vol));
            if (rc != 0) {
                return rc;
            }
            state->report->fixed++;
            break;
        }

        if (value == cluster) {
            rc = fat_entry_set(vol, cluster, end_value(vol));
            if (rc != 0) {
                return rc;
            }
            state->report->fixed++;
            break;
        }

        if (claimed_test(state, value)) {
            k_u32_t kept = chain_length_to(vol, first, value, cluster);

            if (kept != 0u) {
                /* The chain loops: end it at the first time it named
                 * `value` and give the rest of the loop back. */
                k_u32_t tail;

                rc = fat_entry_get(vol, value, &tail);
                if (rc != 0) {
                    return rc;
                }

                rc = fat_entry_set(vol, value, end_value(vol));
                if (rc != 0) {
                    return rc;
                }

                unclaim_tail(state, vol, tail, value);
                state->report->fixed++;
                *length = kept;
                return 0;
            }

            /* Another chain is first: this one stops before that cluster,
             * so the name that came first keeps the data whole. */
            rc = fat_entry_set(vol, cluster, end_value(vol));
            if (rc != 0) {
                return rc;
            }
            state->report->fixed++;
            *length = count;
            return 0;
        }

        if (!claimed_take(state, value)) {
            return KERR_BUSY; /* the map cannot change here; refuse to guess */
        }

        cluster = value;
        count++;
    }

    *length = count;
    return 0;
}

/* The byte of the volume of one slot of a directory, by its index. */
static int dir_slot_offset(struct fat_volume *vol, k_u32_t first, k_u32_t slot, k_u64_t *at)
{
    k_u32_t per_cluster = vol->cluster_bytes / FAT_DIR_ENTRY;
    k_u32_t cluster;
    k_u32_t steps = 0;

    if (per_cluster == 0u) {
        return KERR_INVAL;
    }

    if (first == 0u) {
        if ((k_u64_t)(slot + 1u) * FAT_DIR_ENTRY > (k_u64_t)vol->root_entries * FAT_DIR_ENTRY) {
            return KERR_INVAL;
        }

        *at = (k_u64_t)vol->root_dir_start * vol->bytes_per_sector + (k_u64_t)slot * FAT_DIR_ENTRY;
        return 0;
    }

    cluster = first;

    while (slot >= per_cluster) {
        k_u32_t value;
        int rc;

        if (++steps > vol->cluster_count) {
            return KERR_INVAL; /* a chain that does not end is not a path */
        }

        rc = fat_entry_get(vol, cluster, &value);
        if (rc != 0) {
            return rc;
        }

        if (!cluster_in_volume(vol, value)) {
            return KERR_INVAL;
        }

        cluster = value;
        slot -= per_cluster;
    }

    *at = cluster_offset(vol, cluster) + (k_u64_t)slot * FAT_DIR_ENTRY;
    return 0;
}

/* Whether the repair of this entry will remove it. A run of long-name
 * entries in front of a removed entry has nothing left to name, so it goes
 * with the entry; the decision is taken before the entry is written, while
 * the run is still in the scanner. */
static bool entry_will_be_removed(const struct fat_volume *vol, const k_u8_t entry[FAT_DIR_ENTRY])
{
    bool is_dir = (entry[11] & FAT_ATTR_DIR) != 0u;
    k_u32_t cluster;
    k_u32_t size;

    if (entry[0] == '.') {
        return false;
    }

    if (!short_name_valid(entry)) {
        return !is_dir;
    }

    cluster = entry_cluster(vol, entry);
    size = fat_le32(entry + 28);

    if (!is_dir && size == 0u) {
        return false; /* the entry stays, empty or with a chain to give back */
    }

    return !cluster_in_volume(vol, cluster);
}

/* Collect one long-name entry for the repair. The run is whole when it
 * starts with the LAST marker, the sequence numbers fall by one and every
 * entry carries the checksum of the short entry that follows. */
static void repair_lfn_feed(struct walk_frame *frame, const k_u8_t entry[FAT_DIR_ENTRY])
{
    struct lfn_run *run = &frame->lfn;
    k_u8_t sequence = (k_u8_t)(entry[0] & 0x1Fu);
    bool last = (entry[0] & 0x40u) != 0u;

    if (!run->active) {
        run->active = true;
        run->first_slot = frame->slot - 1u;
        run->count = 1;
        run->checksum = entry[13];
        run->broken = !last || sequence == 0u || sequence > FAT_LFN_ENTRIES;
        run->next = (sequence == 0u) ? 0u : (k_u8_t)(sequence - 1u);
        return;
    }

    run->count++;

    if (last || sequence == 0u || sequence > FAT_LFN_ENTRIES || sequence != run->next ||
        entry[13] != run->checksum) {
        run->broken = true;
    }

    run->next = (sequence == 0u) ? 0u : (k_u8_t)(sequence - 1u);
}

/* Remove every entry of a run. The 8.3 entry that follows stays. */
static int repair_lfn_delete(struct check_state *state, struct fat_volume *vol,
                             struct walk_frame *frame)
{
    k_u32_t i;

    for (i = 0; i < frame->lfn.count; i++) {
        k_u8_t entry[FAT_DIR_ENTRY];
        k_u64_t at = 0;
        int rc = dir_slot_offset(vol, frame->first, frame->lfn.first_slot + i, &at);

        if (rc != 0) {
            return rc;
        }

        rc = fat_read_volume(vol, at, entry, FAT_DIR_ENTRY);
        if (rc != 0) {
            return rc;
        }

        if (entry[0] == 0xE5u) {
            continue;
        }

        entry[0] = 0xE5u;
        rc = entry_write(state, vol, at, entry);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/* A run with no short entry in front of it is the wake of a removed entry,
 * and it goes the same way. */
static int repair_lfn_orphan(struct check_state *state, struct fat_volume *vol,
                             struct walk_frame *frame)
{
    int rc;

    if (!frame->lfn.active) {
        return 0;
    }

    rc = repair_lfn_delete(state, vol, frame);
    if (rc == 0) {
        state->report->fixed++;
    }

    lfn_reset(&frame->lfn);

    return rc;
}

/* The size of a file against the chain the repair left: a file that promised
 * more than the medium holds is cut down, and a chain that reaches past the
 * end of its file gives the tail back. */
static int repair_file_size(struct check_state *state, struct fat_volume *vol, k_u64_t at,
                            k_u8_t entry[FAT_DIR_ENTRY], k_u32_t first, k_u32_t size,
                            k_u32_t length)
{
    k_u32_t needed = (k_u32_t)(((k_u64_t)size + vol->cluster_bytes - 1u) / vol->cluster_bytes);
    int rc;

    if (length < needed) {
        fat_put32(entry + 28, length * vol->cluster_bytes);
        rc = entry_write(state, vol, at, entry);
        if (rc == 0) {
            state->report->fixed++;
        }
        return rc;
    }

    if (length > needed) {
        k_u32_t cluster = first;
        k_u32_t tail;
        k_u32_t i;

        for (i = 1u; i < needed; i++) {
            k_u32_t next;

            rc = fat_entry_get(vol, cluster, &next);
            if (rc != 0) {
                return rc;
            }
            cluster = next;
        }

        rc = fat_entry_get(vol, cluster, &tail);
        if (rc != 0) {
            return rc;
        }

        rc = fat_entry_set(vol, cluster, end_value(vol));
        if (rc != 0) {
            return rc;
        }

        unclaim_tail(state, vol, tail, 0u);
        state->report->fixed++;
    }

    return 0;
}

/*
 * Repair one short entry. The chain of a directory is not claimed here: the
 * walk claims it when it goes down into the directory, so that a directory
 * whose head is shared with a file is left for a person instead of having
 * its subtree thrown away. A file gives a shared head up: the data stays
 * with the name that came first.
 */
static int repair_entry(struct check_state *state, struct fat_volume *vol, k_u64_t at,
                        k_u8_t entry[FAT_DIR_ENTRY], k_u32_t *child, bool *has_child)
{
    k_u32_t cluster = entry_cluster(vol, entry);
    k_u32_t size = fat_le32(entry + 28);
    bool is_dir = (entry[11] & FAT_ATTR_DIR) != 0u;
    k_u32_t length = 0;
    int rc;

    *has_child = false;

    if (entry[0] == '.') {
        return 0; /* the dot entries name the directory itself and its parent */
    }

    if (!short_name_valid(entry)) {
        /* A file is only a name here, so the entry goes. A directory keeps
         * its name and its subtree and is named as left by the report. */
        return is_dir ? 0 : entry_delete(state, vol, at);
    }

    if (!is_dir && size == 0u) {
        /* An empty file that names a cluster is a chain nobody measures.
         * It cannot be told from the chain of a directory the walk has not
         * reached yet, so the pass does not free it: the report names it
         * and a person decides. (A chain a crash left with no entry at all
         * is a lost cluster, and the pass does free those.) */
        return 0;
    }

    if (!cluster_in_volume(vol, cluster)) {
        /* Nothing on the medium says where the data is. The name is all
         * that is left, and an entry nobody can read is not a name. */
        return entry_delete(state, vol, at);
    }

    if (is_dir) {
        *child = cluster;
        *has_child = true;
        return 0;
    }

    if (claimed_test(state, cluster)) {
        /* Two names, one chain: the first keeps it and this file becomes
         * empty. The name stays, so a person sees what happened. */
        fat_put16(entry + 26, 0);
        if (vol->type == 32) {
            fat_put16(entry + 20, 0);
        }
        fat_put32(entry + 28, 0);

        rc = entry_write(state, vol, at, entry);
        if (rc == 0) {
            state->report->fixed++;
        }
        return rc;
    }

    rc = repair_chain(state, vol, cluster, &length);
    if (rc != 0) {
        return rc;
    }

    return repair_file_size(state, vol, at, entry, cluster, size, length);
}

/* Walk the tree the way the check does, and repair what the walk meets. */
static int repair_tree(struct check_state *state, struct fat_volume *vol)
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
        k_u64_t at = 0;
        k_u32_t label = (top == 0u) ? 0u : frame->first;
        bool end = false;
        int rc;

        if (!frame->loaded) {
            if (frame->first != 0u) {
                k_u32_t length = 0;

                if (claimed_test(state, frame->first)) {
                    /* A directory that names a chain a file already holds:
                     * the subtree is not this name's to give away. The
                     * tree is then not walked whole, so the pass cannot
                     * tell a lost cluster from a cluster of this subtree
                     * and does not free any. */
                    state->incomplete = true;
                    if (top == 0u) {
                        return 0;
                    }
                    top--;
                    continue;
                }

                rc = repair_chain(state, vol, frame->first, &length);
                if (rc != 0) {
                    add_error(state, CHECK_SUBJECT_DIR, label,
                              "the directory chain could not be repaired");
                    return rc;
                }

                frame->cluster = frame->first;
            }

            frame->offset = 0;
            frame->slot = 0;
            frame->loaded = true;
        }

        if (!frame_entry(vol, frame, entry, &at, &end)) {
            if (!end) {
                add_error(state, CHECK_SUBJECT_DIR, label, "an entry cannot be read");
                return KERR_IO;
            }

            rc = repair_lfn_orphan(state, vol, frame);
            if (rc != 0) {
                return rc;
            }

            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        if (entry[0] == 0x00u) {
            rc = repair_lfn_orphan(state, vol, frame);
            if (rc != 0) {
                return rc;
            }

            if (top == 0u) {
                break;
            }
            top--;
            continue;
        }

        if (entry[0] == 0xE5u) {
            /* A removed entry takes its long name with it in a sound
             * volume; what is left of the run is an orphan. */
            rc = repair_lfn_orphan(state, vol, frame);
            if (rc != 0) {
                return rc;
            }
            continue;
        }

        if (entry[11] == FAT_ATTR_LFN) {
            repair_lfn_feed(frame, entry);
            continue;
        }

        if (frame->lfn.active) {
            bool whole = !frame->lfn.broken && frame->lfn.next == 0u && frame->lfn.count != 0u &&
                         frame->lfn.checksum == short_checksum(entry);

            /* A run that cannot be read is removed whole; the 8.3 name
             * stays. A run in front of an entry that is not a name, or of
             * an entry about to be removed, goes with it. */
            if (!whole || !short_name_valid(entry) || entry_will_be_removed(vol, entry)) {
                rc = repair_lfn_delete(state, vol, frame);
                if (rc != 0) {
                    return rc;
                }
                state->report->fixed++;
            }

            lfn_reset(&frame->lfn);
        }

        if ((entry[11] & FAT_ATTR_VOLUME) != 0u) {
            continue;
        }

        {
            k_u32_t child = 0;
            bool has_child = false;

            rc = repair_entry(state, vol, at, entry, &child, &has_child);
            if (rc != 0) {
                return rc;
            }

            if (!has_child) {
                continue;
            }

            if (top + 1u >= FAT_CHECK_DEPTH_MAX) {
                if (!depth_reported) {
                    add_error(state, CHECK_SUBJECT_DIR, label,
                              "the tree is deeper than a path can name");
                    depth_reported = true;
                }
                /* The subtree below the limit was not walked, so the pass
                 * cannot tell its clusters from lost ones. */
                state->incomplete = true;
                continue;
            }

            top++;
            memset(&stack[top], 0, sizeof(stack[top]));
            stack[top].first = child;
            stack[top].cluster = child;
            lfn_reset(&stack[top].lfn);
        }
    }

    return 0;
}

/* Every cluster that is marked used and that no repaired chain names is
 * space no file will ever get back: the writer only takes the zero entries
 * of the table. It goes back to the free map, in both copies of the table. */
static int repair_lost(struct check_state *state, struct fat_volume *vol)
{
    k_u32_t cluster;

    for (cluster = 2u; cluster < vol->cluster_count + 2u; cluster++) {
        k_u32_t value;
        int rc = fat_entry_get(vol, cluster, &value);

        if (rc != 0) {
            return rc;
        }

        if (value == 0u || value == bad_mark(vol) || claimed_test(state, cluster)) {
            continue;
        }

        state->report->lost_found++;

        rc = fat_entry_set(vol, cluster, 0);
        if (rc != 0) {
            return rc;
        }

        state->report->lost_freed++;
        state->report->fixed++;
    }

    return 0;
}

static int repair_pass(struct fat_volume *vol, struct check_state *state)
{
    int rc = repair_tree(state, vol);

    /* The lost clusters are freed only when the whole tree was walked:
     * otherwise the pass cannot tell a lost cluster from one of a subtree
     * it refused to enter, and giving that one back would destroy data a
     * person could still recover by fixing the entry by hand. */
    if (rc == 0 && !state->incomplete) {
        rc = repair_lost(state, vol);
    }

    if (rc == 0) {
        rc = fat_flush(vol);
    }

    if (rc != 0) {
        add_error(state, CHECK_SUBJECT_VOLUME, 0,
                  "the repair stopped at a medium that did not answer");
    }

    return rc;
}

/* --- the entry point --- */

int fat_check_volume(struct fat_volume *vol, struct fat_check_report *report, bool write)
{
    if (vol == NULL || report == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    memset(report, 0, sizeof(*report));
    report->type = vol->type;
    report->clusters = vol->cluster_count;

    if (vol->cluster_count > FAT_CHECK_CLUSTERS_MAX) {
        memset(&s_check, 0, sizeof(s_check));
        s_check.report = report;
        add_error(&s_check, CHECK_SUBJECT_VOLUME, 0,
                  "the volume has more clusters than the check can claim");
        return 0;
    }

    if (write) {
        struct fat_check_report probe;
        bool safe;

        /* A repair writes with the geometry of the mount. If the medium
         * has moved away from that geometry, or the two copies of the table
         * no longer agree, the medium does not say which of the two stories
         * is the volume: the pass does not guess. */
        memset(&probe, 0, sizeof(probe));
        memset(&s_check, 0, sizeof(s_check));
        s_check.report = &probe;
        check_boot(&s_check, vol);
        if (probe.errors == 0) {
            check_tables(&s_check, vol);
        }
        safe = (probe.errors == 0);

        if (safe) {
            memset(&s_check, 0, sizeof(s_check));
            s_check.report = report;
            s_check.write = true;
            (void)repair_pass(vol, &s_check);
        }
    }

    /* The check runs once more, with a map of its own, so the answer is
     * about the volume as the repair left it and not about the plan. */
    memset(&s_check, 0, sizeof(s_check));
    s_check.report = report;

    check_boot(&s_check, vol);
    check_tables(&s_check, vol);
    check_tree(&s_check, vol);
    count_clusters(&s_check, vol);

    if (write) {
        report->unfixed = report->errors;
    }

    return 0;
}

int fat_check_device(const struct k_blockdev *dev, struct fat_check_report *report, bool write)
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

    rc = fat_check_volume(&volume, report, write);

    k_mutex_unlock(&volume.lock);
    fat_volume_unmount(&volume);

    return rc;
}
