/*
 * Micro OS — reading the tables of the partitions of a disk.
 *
 * The file knows two formats, both older than this kernel and both written
 * in the first sectors of the disk: the MBR of 1983 (four entries of sixteen
 * bytes at the end of LBA 0) and the GPT of the machines with an EFI (a
 * header in LBA 1, an array of entries after it, and a CRC32 over each).
 *
 * The parser is deliberately suspicious. Everything the table claims is
 * held against the size of the disk: an entry that ends after the last
 * sector, two entries that claim the same sectors, an offset that the
 * sectors of the device cannot address - each one is a refusal of the whole
 * table, because a filesystem mounted on the wrong bytes is worse than a
 * disk that says why it cannot be used. The medium is never written: this is
 * a reader, and the repair of a table belongs to a tool of the person who
 * owns the disk, not to a boot.
 *
 * The children live in a small table of static slots: the registry of the
 * kernel keeps the pointer of every registered device for the life of the
 * system (kernel/blockdev.c), so the structure of a child cannot be an
 * object of a stack frame.
 */
#include "kernel/partition.h"

#include "kernel/errno.h"

/* The MBR and the header of the GPT are the first two LBAs of the disk. */
#define PARTITION_HEAD_BYTES 1024u

/* The unit of both tables, whatever the medium of the disk calls a sector. */
#define PARTITION_LBA_BYTES 512u

/* --- the MBR of 1983 --- */

#define PARTITION_MBR_OFFSET 446u
#define PARTITION_MBR_ENTRIES 4u
#define PARTITION_MBR_ENTRY 16u
#define PARTITION_MBR_SIGNATURE 510u
#define PARTITION_MBR_TYPE_EMPTY 0x00u
#define PARTITION_MBR_TYPE_EXTENDED 0x05u
#define PARTITION_MBR_TYPE_EXTENDED_LBA 0x0Fu
#define PARTITION_MBR_TYPE_EXTENDED_LINUX 0x85u
#define PARTITION_MBR_TYPE_PROTECTIVE 0xEEu

/* --- the GPT --- */

#define PARTITION_GPT_REVISION 0x00010000u
#define PARTITION_GPT_HEADER_MIN 92u
#define PARTITION_GPT_ENTRY_MIN 128u
#define PARTITION_GPT_ENTRIES_MAX 4096u

/* The offsets inside the header of the GPT. */
#define PARTITION_GPT_HEADER_SIZE 12u
#define PARTITION_GPT_HEADER_CRC 16u
#define PARTITION_GPT_CURRENT_LBA 24u
#define PARTITION_GPT_FIRST_USABLE 40u
#define PARTITION_GPT_LAST_USABLE 48u
#define PARTITION_GPT_ENTRIES_LBA 72u
#define PARTITION_GPT_ENTRIES 80u
#define PARTITION_GPT_ENTRY_SIZE 84u
#define PARTITION_GPT_ENTRIES_CRC 88u

/* The offsets inside an entry of the GPT. */
#define PARTITION_GPT_ENTRY_TYPE 0u
#define PARTITION_GPT_ENTRY_FIRST 32u
#define PARTITION_GPT_ENTRY_LAST 40u

/* --- what a scan found, before anything is registered --- */

struct partition_found {
    k_u64_t first_lba; /* the first sector of the partition */
    k_u64_t sectors;   /* its length, both in PARTITION_LBA_BYTES */
    k_u32_t number;    /* the number of the entry, for the name of the child */
    /*
     * The type byte of the entry (0x83 Linux, 0x0B/0x0C FAT32, ...), which
     * the boot reads as a hint about the filesystem inside (ADR 0027). A
     * GPT entry has no such byte - its type is a GUID - and the byte is
     * zero there, which means "the table says nothing".
     */
    k_u8_t type;
};

/* The geometry of the GPT, as the header of the disk describes it. */
struct gpt_geometry {
    k_u64_t entries_lba;
    k_u64_t first_usable;
    k_u64_t last_usable;
    k_u32_t entries;
    k_u32_t entry_size;
    k_u32_t entries_crc;
};

/* --- the children --- */

struct partition_ctx {
    const struct k_blockdev *parent;
    k_u32_t first_sector; /* where the partition starts, in the parent */
    k_u32_t sectors;      /* its length, in the sectors of the parent */
};

struct partition_slot {
    bool used;
    struct partition_ctx ctx;
    struct k_blockdev dev;
    char name[K_PARTITION_NAME_MAX];
    k_u8_t type; /* the type byte of the entry, 0 for a GPT entry */
};

static struct partition_slot g_slots[K_PARTITION_MAX];

static int child_read(void *ctx_ptr, k_u32_t sector, void *buf, k_size_t count);
static int child_write(void *ctx_ptr, k_u32_t sector, const void *buf, k_size_t count);
static int child_erase(void *ctx_ptr, k_u32_t sector, k_size_t count);
static int child_sync(void *ctx_ptr);

/*
 * A child refuses what its parent would accept past its end: the registry of
 * the kernel hands the request to the parent, and the parent only knows the
 * whole disk. The check is here, not there.
 */
static bool child_range_ok(const struct partition_ctx *ctx, k_u32_t sector, k_size_t count)
{
    if (count == 0 || sector >= ctx->sectors) {
        return false;
    }

    return count <= (k_size_t)ctx->sectors - sector;
}

static int child_read(void *ctx_ptr, k_u32_t sector, void *buf, k_size_t count)
{
    const struct partition_ctx *ctx = (const struct partition_ctx *)ctx_ptr;

    if (buf == NULL || !child_range_ok(ctx, sector, count)) {
        return -1;
    }

    return ctx->parent->ops->read(ctx->parent->ctx, ctx->first_sector + sector, buf, count);
}

static int child_write(void *ctx_ptr, k_u32_t sector, const void *buf, k_size_t count)
{
    const struct partition_ctx *ctx = (const struct partition_ctx *)ctx_ptr;

    if (buf == NULL || !child_range_ok(ctx, sector, count)) {
        return -1;
    }

    return ctx->parent->ops->write(ctx->parent->ctx, ctx->first_sector + sector, buf, count);
}

static int child_erase(void *ctx_ptr, k_u32_t sector, k_size_t count)
{
    const struct partition_ctx *ctx = (const struct partition_ctx *)ctx_ptr;

    if (!child_range_ok(ctx, sector, count)) {
        return -1;
    }

    return ctx->parent->ops->erase(ctx->parent->ctx, ctx->first_sector + sector, count);
}

static int child_sync(void *ctx_ptr)
{
    const struct partition_ctx *ctx = (const struct partition_ctx *)ctx_ptr;

    return ctx->parent->ops->sync(ctx->parent->ctx);
}

/* The door of the writes belongs to the whole disk and a partition is a way
 * of naming its sectors: the mount of a partition opens the writes of the
 * controller below it (ADR 0041 §2). The child ops carry the call only when
 * the parent's do, so k_blockdev_gated() answers for the pair. */
static int child_writes(void *ctx_ptr, bool open)
{
    const struct partition_ctx *ctx = (const struct partition_ctx *)ctx_ptr;

    return ctx->parent->ops->writes(ctx->parent->ctx, open);
}

/* A disk needs no erase, and a partition of it neither. */
static const struct k_blockdev_ops g_child_ops = {
    .read = child_read,
    .write = child_write,
    .erase = NULL,
    .sync = child_sync,
};

/* The same for a medium that has one - the flash of a board, a card. */
static const struct k_blockdev_ops g_child_ops_erase = {
    .read = child_read,
    .write = child_write,
    .erase = child_erase,
    .sync = child_sync,
};

/* The same two when the disk below them has the door of the writes. */
static const struct k_blockdev_ops g_child_ops_gated = {
    .read = child_read,
    .write = child_write,
    .erase = NULL,
    .sync = child_sync,
    .writes = child_writes,
};

static const struct k_blockdev_ops g_child_ops_erase_gated = {
    .read = child_read,
    .write = child_write,
    .erase = child_erase,
    .sync = child_sync,
    .writes = child_writes,
};

/* The pair of operations a child of `disk` needs: the erase of the parent and
 * the door of the writes of the parent, each only when the parent has it. */
static const struct k_blockdev_ops *child_ops_of(const struct k_blockdev *disk)
{
    bool erase = disk->ops->erase != NULL;
    bool gated = disk->ops->writes != NULL;

    if (erase) {
        return gated ? &g_child_ops_erase_gated : &g_child_ops_erase;
    }

    return gated ? &g_child_ops_gated : &g_child_ops;
}

/* --- the little numbers of the tables --- */

static k_u32_t read_le32(const k_u8_t *bytes)
{
    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

static k_u64_t read_le64(const k_u8_t *bytes)
{
    return (k_u64_t)read_le32(bytes) | ((k_u64_t)read_le32(bytes + 4) << 32);
}

/* The checksum of the GPT: the polynomial of IEEE 802.3, as in MicroFS. */
static k_u32_t crc32_update(k_u32_t crc, const void *data, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_size_t i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }

    return crc;
}

static k_u32_t crc32(const void *data, k_size_t size)
{
    return crc32_update(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

static k_size_t text_len(const char *text)
{
    k_size_t len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

/* --- the table of the children --- */

static k_size_t children_of(const struct k_blockdev *disk)
{
    k_size_t i;
    k_size_t count = 0;

    for (i = 0; i < K_PARTITION_MAX; i++) {
        if (g_slots[i].used && g_slots[i].ctx.parent == disk) {
            count++;
        }
    }

    return count;
}

/* Children take sectors of the parent: a scan of the same disk replaces them. */
void partition_release(const struct k_blockdev *disk)
{
    k_size_t i;

    for (i = 0; i < K_PARTITION_MAX; i++) {
        if (!g_slots[i].used || g_slots[i].ctx.parent != disk) {
            continue;
        }

        (void)k_blockdev_unregister(&g_slots[i].dev);
        g_slots[i].used = false;
    }
}

/*
 * The name of a child: the name of the disk, "p" and the number of the entry.
 * The number is the one of the table, not the place in the scan, so a disk
 * with an empty second entry still has "sd0p1" and "sd0p3".
 */
static bool child_name(const char *disk, k_u32_t number, char *out, k_size_t size)
{
    k_size_t len = text_len(disk);
    k_size_t digits = 1;
    k_size_t i;
    k_u32_t rest = number;

    while (rest >= 10u) {
        rest /= 10u;
        digits++;
    }

    if (len + 1u + digits + 1u > size) {
        return false;
    }

    for (i = 0; i < len; i++) {
        out[i] = disk[i];
    }

    out[len] = 'p';

    for (i = 0; i < digits; i++) {
        out[len + 1u + digits - 1u - i] = (char)('0' + (int)(number % 10u));
        number /= 10u;
    }

    out[len + 1u + digits] = '\0';

    return true;
}

const char *partition_name(const struct k_blockdev *disk, k_size_t index)
{
    k_size_t i;
    k_size_t seen = 0;

    if (disk == NULL) {
        return NULL;
    }

    for (i = 0; i < K_PARTITION_MAX; i++) {
        if (!g_slots[i].used || g_slots[i].ctx.parent != disk) {
            continue;
        }

        if (seen == index) {
            return g_slots[i].name;
        }

        seen++;
    }

    return NULL;
}

int partition_type_of(const struct k_blockdev *part)
{
    k_size_t i;

    if (part == NULL) {
        return -1;
    }

    for (i = 0; i < K_PARTITION_MAX; i++) {
        if (!g_slots[i].used || &g_slots[i].dev != part) {
            continue;
        }

        /* A zero is a GPT entry, whose type is a GUID this layer does not
         * translate: the bytes of the medium answer for it, not the table. */
        return g_slots[i].type != 0 ? (int)g_slots[i].type : -1;
    }

    return -1;
}

static struct partition_slot *slot_free(void)
{
    k_size_t i;

    for (i = 0; i < K_PARTITION_MAX; i++) {
        if (!g_slots[i].used) {
            return &g_slots[i];
        }
    }

    return NULL;
}

/* --- reading the disk --- */

static bool head_readable(const struct k_blockdev *disk)
{
    return disk->sector_size != 0 && (PARTITION_HEAD_BYTES % disk->sector_size) == 0 &&
           PARTITION_HEAD_BYTES / disk->sector_size <= disk->sectors;
}

static bool head_read(const struct k_blockdev *disk, k_u8_t *head)
{
    k_size_t sectors = PARTITION_HEAD_BYTES / disk->sector_size;

    return disk->ops->read(disk->ctx, 0, head, sectors) == 0;
}

/*
 * Everything the table says is held against the disk: a partition that ends
 * past the last sector is not a partition, and one that does not start and
 * end on a sector of the device cannot be addressed by the calls of the
 * device at all.
 */
static int found_check(const struct k_blockdev *disk, const struct partition_found *found,
                       k_size_t count)
{
    k_u64_t limit = (k_u64_t)disk->sectors * disk->sector_size;
    k_size_t i;

    for (i = 0; i < count; i++) {
        k_u64_t first = found[i].first_lba * PARTITION_LBA_BYTES;
        k_u64_t length = found[i].sectors * PARTITION_LBA_BYTES;

        if (length == 0 || first + length > limit) {
            return KERR_INVAL;
        }

        if ((first % disk->sector_size) != 0 || (length % disk->sector_size) != 0) {
            return KERR_INVAL;
        }
    }

    return 0;
}

/* --- the MBR --- */

static int scan_mbr(const k_u8_t *head, k_u64_t disk_lbas, struct partition_found *found,
                    k_size_t *count)
{
    k_size_t slot;
    bool protective = false;

    if (head[PARTITION_MBR_SIGNATURE] != 0x55u || head[PARTITION_MBR_SIGNATURE + 1u] != 0xAAu) {
        return KERR_NOENT;
    }

    for (slot = 0; slot < PARTITION_MBR_ENTRIES; slot++) {
        const k_u8_t *entry = head + PARTITION_MBR_OFFSET + slot * PARTITION_MBR_ENTRY;
        k_u8_t type = entry[4];
        k_u32_t first;
        k_u32_t sectors;
        k_size_t i;

        if (type == PARTITION_MBR_TYPE_EMPTY) {
            continue;
        }

        /*
         * The protective entry says "there is a GPT here" and covers the
         * whole disk; the logical partitions of an extended one are a chain
         * this kernel does not walk yet (ADR 0008).
         */
        if (type == PARTITION_MBR_TYPE_PROTECTIVE) {
            protective = true;
            continue;
        }

        if (type == PARTITION_MBR_TYPE_EXTENDED || type == PARTITION_MBR_TYPE_EXTENDED_LBA ||
            type == PARTITION_MBR_TYPE_EXTENDED_LINUX) {
            continue;
        }

        first = read_le32(entry + 8);
        sectors = read_le32(entry + 12);

        if (first == 0 || sectors == 0) {
            return KERR_INVAL;
        }

        if ((k_u64_t)first + sectors > disk_lbas) {
            return KERR_INVAL;
        }

        for (i = 0; i < *count; i++) {
            if ((k_u64_t)first < found[i].first_lba + found[i].sectors &&
                found[i].first_lba < (k_u64_t)first + sectors) {
                return KERR_INVAL;
            }
        }

        if (*count == K_PARTITION_MAX) {
            return KERR_NOSPC;
        }

        found[*count].first_lba = first;
        found[*count].sectors = sectors;
        found[*count].number = (k_u32_t)slot + 1u;
        found[*count].type = type;
        (*count)++;
    }

    if (*count == 0) {
        /* An MBR that only protects a GPT without one is a damaged table,
         * not an empty disk. */
        return protective ? KERR_INVAL : KERR_NOENT;
    }

    return 0;
}

/* --- the GPT --- */

static bool gpt_header_crc_ok(const k_u8_t *header)
{
    k_u8_t copy[PARTITION_LBA_BYTES];
    k_size_t size = (k_size_t)read_le32(header + PARTITION_GPT_HEADER_SIZE);
    k_u32_t stored = read_le32(header + PARTITION_GPT_HEADER_CRC);
    k_size_t i;

    if (size < PARTITION_GPT_HEADER_MIN || size > PARTITION_LBA_BYTES) {
        return false;
    }

    for (i = 0; i < size; i++) {
        copy[i] = header[i];
    }

    /* The checksum was taken with its own field zero. */
    copy[PARTITION_GPT_HEADER_CRC + 0u] = 0;
    copy[PARTITION_GPT_HEADER_CRC + 1u] = 0;
    copy[PARTITION_GPT_HEADER_CRC + 2u] = 0;
    copy[PARTITION_GPT_HEADER_CRC + 3u] = 0;

    return crc32(copy, size) == stored;
}

static bool gpt_signature_ok(const k_u8_t *header)
{
    static const char magic[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    k_size_t i;

    for (i = 0; i < sizeof(magic); i++) {
        if (header[i] != magic[i]) {
            return false;
        }
    }

    return true;
}

static int gpt_geometry_read(const struct k_blockdev *disk, const k_u8_t *header, k_u64_t disk_lbas,
                             struct gpt_geometry *geo)
{
    k_u64_t area;

    if (read_le32(header + 8) != PARTITION_GPT_REVISION) {
        return KERR_INVAL;
    }

    if (!gpt_header_crc_ok(header)) {
        return KERR_INVAL;
    }

    /* The header of the disk itself is the only one this boot can use. */
    if (read_le64(header + PARTITION_GPT_CURRENT_LBA) != 1u) {
        return KERR_INVAL;
    }

    geo->entries_lba = read_le64(header + PARTITION_GPT_ENTRIES_LBA);
    geo->first_usable = read_le64(header + PARTITION_GPT_FIRST_USABLE);
    geo->last_usable = read_le64(header + PARTITION_GPT_LAST_USABLE);
    geo->entries = read_le32(header + PARTITION_GPT_ENTRIES);
    geo->entry_size = read_le32(header + PARTITION_GPT_ENTRY_SIZE);
    geo->entries_crc = read_le32(header + PARTITION_GPT_ENTRIES_CRC);

    if (geo->entries == 0 || geo->entries > PARTITION_GPT_ENTRIES_MAX) {
        return KERR_INVAL;
    }

    if (geo->entry_size < PARTITION_GPT_ENTRY_MIN || (geo->entry_size % 8u) != 0) {
        return KERR_INVAL;
    }

    /*
     * The entries are read in sectors of the disk, so an entry and an offset
     * the device cannot express are refused here instead of half way through
     * the array.
     */
    if (geo->entry_size > disk->sector_size || (disk->sector_size % geo->entry_size) != 0) {
        return KERR_INVAL;
    }

    if ((geo->entries_lba * PARTITION_LBA_BYTES) % disk->sector_size != 0) {
        return KERR_INVAL;
    }

    if (geo->first_usable > geo->last_usable || geo->last_usable >= disk_lbas) {
        return KERR_INVAL;
    }

    area = (k_u64_t)geo->entries * geo->entry_size;

    if (geo->entries_lba < 2u ||
        geo->entries_lba * PARTITION_LBA_BYTES + area > disk_lbas * PARTITION_LBA_BYTES) {
        return KERR_INVAL;
    }

    return 0;
}

static bool gpt_entries_crc_ok(const struct k_blockdev *disk, const struct gpt_geometry *geo)
{
    k_u8_t chunk[PARTITION_HEAD_BYTES];
    k_u64_t left = (k_u64_t)geo->entries * geo->entry_size;
    k_u32_t sector = (k_u32_t)(geo->entries_lba * PARTITION_LBA_BYTES / disk->sector_size);
    k_u32_t crc = 0xFFFFFFFFu;

    while (left > 0) {
        k_size_t take = left > disk->sector_size ? disk->sector_size : (k_size_t)left;

        if (disk->ops->read(disk->ctx, sector, chunk, 1) != 0) {
            return false;
        }

        crc = crc32_update(crc, chunk, take);

        sector++;
        left -= take;
    }

    return (crc ^ 0xFFFFFFFFu) == geo->entries_crc;
}

static int gpt_collect(const struct k_blockdev *disk, const struct gpt_geometry *geo,
                       struct partition_found *found, k_size_t *count)
{
    k_u8_t chunk[PARTITION_HEAD_BYTES];
    k_u64_t left = (k_u64_t)geo->entries * geo->entry_size;
    k_u32_t sector = (k_u32_t)(geo->entries_lba * PARTITION_LBA_BYTES / disk->sector_size);
    k_u32_t number = 1;

    while (left > 0) {
        k_size_t take = left > disk->sector_size ? disk->sector_size : (k_size_t)left;
        k_size_t offset;

        if (disk->ops->read(disk->ctx, sector, chunk, 1) != 0) {
            return KERR_IO;
        }

        for (offset = 0; offset + geo->entry_size <= take; offset += geo->entry_size) {
            const k_u8_t *entry = chunk + offset;
            k_u64_t first;
            k_u64_t last;
            k_size_t byte;
            bool typed = false;

            /* An entry with the empty type is a hole in the table. */
            for (byte = 0; byte < 16u; byte++) {
                if (entry[PARTITION_GPT_ENTRY_TYPE + byte] != 0) {
                    typed = true;
                    break;
                }
            }

            if (!typed) {
                number++;
                continue;
            }

            first = read_le64(entry + PARTITION_GPT_ENTRY_FIRST);
            last = read_le64(entry + PARTITION_GPT_ENTRY_LAST);

            if (last < first || first < geo->first_usable) {
                return KERR_INVAL;
            }

            /*
             * last_usable is the range the GPT reserves for partitions, but a
             * real disk may carry a partition that ends past it and still
             * inside the disk (a recovery partition, a table a tool wrote with
             * a stale field) - sgdisk -v accepts it. The bound that matters is
             * the disk itself, which found_check enforces after the scan.
             */

            if (*count == K_PARTITION_MAX) {
                return KERR_NOSPC;
            }

            found[*count].first_lba = first;
            found[*count].sectors = last - first + 1u;
            found[*count].number = number;
            found[*count].type = 0; /* a GUID, not the byte of an MBR entry */
            (*count)++;
            number++;
        }

        sector++;
        left -= take;
    }

    if (*count == 0) {
        return KERR_NOENT;
    }

    return 0;
}

static int scan_gpt(const struct k_blockdev *disk, const k_u8_t *header, k_u64_t disk_lbas,
                    struct partition_found *found, k_size_t *count)
{
    struct gpt_geometry geo;
    int rc;

    rc = gpt_geometry_read(disk, header, disk_lbas, &geo);
    if (rc != 0) {
        return rc;
    }

    /*
     * Both checksums are verified before a single entry is believed: a table
     * whose array was rewritten by a machine that crashed is not a table
     * with one wrong entry, it is a table without an owner.
     */
    if (!gpt_entries_crc_ok(disk, &geo)) {
        return KERR_INVAL;
    }

    return gpt_collect(disk, &geo, found, count);
}

/* --- the scan --- */

static int scan_table(const struct k_blockdev *disk, const k_u8_t *head,
                      struct partition_found *found, k_size_t *count)
{
    k_u64_t disk_lbas = (k_u64_t)disk->sectors * (disk->sector_size / PARTITION_LBA_BYTES);
    int rc;

    if (gpt_signature_ok(head + PARTITION_LBA_BYTES)) {
        rc = scan_gpt(disk, head + PARTITION_LBA_BYTES, disk_lbas, found, count);
    } else {
        rc = scan_mbr(head, disk_lbas, found, count);
    }

    if (rc != 0) {
        return rc;
    }

    return found_check(disk, found, *count);
}

static int register_children(const struct k_blockdev *disk, const struct partition_found *found,
                             k_size_t count)
{
    k_size_t i;

    for (i = 0; i < count; i++) {
        struct partition_slot *slot = slot_free();
        k_u64_t first = found[i].first_lba * PARTITION_LBA_BYTES;
        k_u64_t length = found[i].sectors * PARTITION_LBA_BYTES;

        if (slot == NULL) {
            partition_release(disk);
            return KERR_NOSPC;
        }

        if (!child_name(disk->name, found[i].number, slot->name, sizeof(slot->name))) {
            partition_release(disk);
            return KERR_INVAL;
        }

        slot->used = true;
        slot->ctx.parent = disk;
        slot->ctx.first_sector = (k_u32_t)(first / disk->sector_size);
        slot->ctx.sectors = (k_u32_t)(length / disk->sector_size);
        slot->type = found[i].type;

        slot->dev.name = slot->name;
        slot->dev.sector_size = disk->sector_size;
        slot->dev.sectors = slot->ctx.sectors;
        slot->dev.ops = child_ops_of(disk);
        slot->dev.ctx = &slot->ctx;

        if (k_blockdev_register(&slot->dev) != 0) {
            slot->used = false;
            partition_release(disk);
            return KERR_NOSPC;
        }
    }

    return (int)count;
}

int partition_scan(const struct k_blockdev *disk)
{
    k_u8_t head[PARTITION_HEAD_BYTES];
    struct partition_found found[K_PARTITION_MAX];
    k_size_t i;
    k_size_t count = 0;
    k_size_t owned;
    k_size_t room;
    int rc;

    if (disk == NULL || disk->ops == NULL || disk->ops->read == NULL || disk->name == NULL) {
        return KERR_INVAL;
    }

    if (!head_readable(disk)) {
        return KERR_INVAL;
    }

    /*
     * A device that answers "read" without filling every byte of the buffer
     * would leave the signature of the GPT, which lives in the second half of
     * the head, to the stack. The zeros make such a device an MBR without a
     * signature instead of a table read from nowhere.
     */
    for (i = 0; i < sizeof(head); i++) {
        head[i] = 0;
    }

    if (!head_read(disk, head)) {
        return KERR_IO;
    }

    rc = scan_table(disk, head, found, &count);
    if (rc != 0) {
        return rc;
    }

    /* The children of this disk are the slots the scan is about to replace. */
    owned = children_of(disk);
    room = k_blockdev_count() - owned;
    room = room >= K_BLOCKDEV_MAX ? 0 : K_BLOCKDEV_MAX - room;

    if (room < count) {
        return KERR_NOSPC;
    }

    partition_release(disk);

    return register_children(disk, found, count);
}
