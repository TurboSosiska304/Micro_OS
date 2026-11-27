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
#include "kernel/kmm.h"

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

static void write_le32(k_u8_t *bytes, k_u32_t value)
{
    bytes[0] = (k_u8_t)(value & 0xFFu);
    bytes[1] = (k_u8_t)((value >> 8) & 0xFFu);
    bytes[2] = (k_u8_t)((value >> 16) & 0xFFu);
    bytes[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static void write_le64(k_u8_t *bytes, k_u64_t value)
{
    write_le32(bytes, (k_u32_t)(value & 0xFFFFFFFFu));
    write_le32(bytes + 4, (k_u32_t)(value >> 32));
}

static void mem_copy(void *dst, const void *src, k_size_t size)
{
    k_u8_t *d = (k_u8_t *)dst;
    const k_u8_t *s = (const k_u8_t *)src;
    k_size_t i;

    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static void mem_set(void *dst, k_u8_t value, k_size_t size)
{
    k_u8_t *d = (k_u8_t *)dst;
    k_size_t i;

    for (i = 0; i < size; i++) {
        d[i] = value;
    }
}

static bool mem_equal(const void *a, const void *b, k_size_t size)
{
    const k_u8_t *pa = (const k_u8_t *)a;
    const k_u8_t *pb = (const k_u8_t *)b;
    k_size_t i;

    for (i = 0; i < size; i++) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }

    return true;
}

/* The type GUIDs the inventory names. GPT stores them in mixed-endian on the
 * wire, so the constants are the raw 16 bytes as they appear on the disk. */
static const k_u8_t g_guid_esp[16] = {
    0x28u, 0x73u, 0x2Au, 0xC1u, 0x1Fu, 0xF8u, 0xD2u, 0x11u,
    0xBAu, 0x4Bu, 0x00u, 0xA0u, 0xC9u, 0x3Eu, 0xC9u, 0x3Bu,
};

/* Linux filesystem data: the type the plan uses for the Micro OS root until
 * the project claims a type GUID of its own. */
static const k_u8_t g_guid_linux_data[16] = {
    0xAFu, 0x3Du, 0xC6u, 0x0Fu, 0x83u, 0x84u, 0x72u, 0x47u,
    0x8Eu, 0x79u, 0x3Du, 0x69u, 0xD8u, 0x47u, 0x7Du, 0xE4u,
};

static bool guid_is_zero(const k_u8_t guid[16])
{
    k_size_t i;

    for (i = 0; i < 16u; i++) {
        if (guid[i] != 0u) {
            return false;
        }
    }

    return true;
}

static bool guid_eq(const k_u8_t a[16], const k_u8_t b[16])
{
    return mem_equal(a, b, 16u);
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
                             k_u64_t expected_lba, struct gpt_geometry *geo)
{
    k_u64_t area;
    k_u64_t entries_end_lba;

    if (read_le32(header + 8) != PARTITION_GPT_REVISION) {
        return KERR_INVAL;
    }
    if (!gpt_header_crc_ok(header)) {
        return KERR_INVAL;
    }
    if (read_le64(header + PARTITION_GPT_CURRENT_LBA) != expected_lba) {
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
    if (geo->entry_size > disk->sector_size || (disk->sector_size % geo->entry_size) != 0) {
        return KERR_INVAL;
    }
    if (geo->first_usable > geo->last_usable || geo->last_usable >= disk_lbas) {
        return KERR_INVAL;
    }

    area = (k_u64_t)geo->entries * geo->entry_size;
    entries_end_lba = geo->entries_lba + (area + PARTITION_LBA_BYTES - 1u) / PARTITION_LBA_BYTES;

    if (entries_end_lba > disk_lbas) {
        return KERR_INVAL;
    }
    if (expected_lba == 1u) {
        if (geo->entries_lba < 2u) {
            return KERR_INVAL;
        }
    } else {
        if (entries_end_lba > expected_lba) {
            return KERR_INVAL;
        }
    }

    return 0;
}

static bool gpt_entries_crc_ok(const struct k_blockdev *disk, const struct gpt_geometry *geo)
{
    k_u8_t chunk[4096];
    k_u64_t left = (k_u64_t)geo->entries * geo->entry_size;
    k_u64_t byte_off = geo->entries_lba * PARTITION_LBA_BYTES;
    k_u32_t sector = (k_u32_t)(byte_off / disk->sector_size);
    k_u32_t off = (k_u32_t)(byte_off % disk->sector_size);
    k_u32_t crc = 0xFFFFFFFFu;

    if (disk->sector_size > sizeof(chunk)) {
        return false;
    }

    while (left > 0) {
        k_size_t avail = (k_size_t)disk->sector_size - off;
        k_size_t take = left > avail ? avail : (k_size_t)left;

        if (disk->ops->read(disk->ctx, sector, chunk, 1) != 0) {
            return false;
        }

        crc = crc32_update(crc, chunk + off, take);

        sector++;
        off = 0;
        left -= take;
    }

    return (crc ^ 0xFFFFFFFFu) == geo->entries_crc;
}

static int gpt_collect(const struct k_blockdev *disk, const struct gpt_geometry *geo,
                       struct partition_found *found, k_size_t *count)
{
    k_u8_t chunk[4096];
    k_u64_t left = (k_u64_t)geo->entries * geo->entry_size;
    k_u64_t byte_off = geo->entries_lba * PARTITION_LBA_BYTES;
    k_u32_t sector = (k_u32_t)(byte_off / disk->sector_size);
    k_u32_t off = (k_u32_t)(byte_off % disk->sector_size);
    k_u32_t number = 1;

    if (disk->sector_size > sizeof(chunk)) {
        return KERR_INVAL;
    }

    while (left > 0) {
        k_size_t avail = (k_size_t)disk->sector_size - off;
        k_size_t take = left > avail ? avail : (k_size_t)left;
        k_size_t offset;

        if (disk->ops->read(disk->ctx, sector, chunk, 1) != 0) {
            return KERR_IO;
        }

        for (offset = off; offset + geo->entry_size <= off + take; offset += geo->entry_size) {
            const k_u8_t *entry = chunk + offset;
            k_u64_t first;
            k_u64_t last;
            k_size_t byte;
            bool typed = false;

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

            if (last < first || first < geo->first_usable || last > geo->last_usable) {
                return KERR_INVAL;
            }

            if (*count == K_PARTITION_MAX) {
                return KERR_NOSPC;
            }

            found[*count].first_lba = first;
            found[*count].sectors = last - first + 1u;
            found[*count].number = number;
            found[*count].type = 0;
            (*count)++;
            number++;
        }

        sector++;
        off = 0;
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

    rc = gpt_geometry_read(disk, header, disk_lbas, 1u, &geo);
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

/* --- raw LBA access --- */

/* Read one 512-byte LBA from `disk`, whatever the sector size of the device. */
static int disk_read_lba512(const struct k_blockdev *disk, k_u64_t lba, k_u8_t out[512])
{
    k_u64_t byte_off = lba * PARTITION_LBA_BYTES;
    k_u32_t dev_sector;
    k_u32_t dev_off;
    k_u8_t sector_buf[4096];

    if (disk->sector_size == 0 || disk->sector_size > sizeof(sector_buf)) {
        return KERR_INVAL;
    }

    dev_sector = (k_u32_t)(byte_off / disk->sector_size);
    dev_off = (k_u32_t)(byte_off % disk->sector_size);

    if (dev_off + PARTITION_LBA_BYTES > disk->sector_size) {
        return KERR_INVAL;
    }

    if (dev_sector >= disk->sectors) {
        return KERR_IO;
    }

    if (disk->ops->read(disk->ctx, dev_sector, sector_buf, 1) != 0) {
        return KERR_IO;
    }

    mem_copy(out, sector_buf + dev_off, PARTITION_LBA_BYTES);
    return 0;
}

/* Write one 512-byte LBA. The device sector is read-modify-written when the
 * device sector is larger than 512 bytes, so the bytes around the LBA stay. */
static int disk_write_lba512(const struct k_blockdev *disk, k_u64_t lba, const k_u8_t in[512])
{
    k_u64_t byte_off = lba * PARTITION_LBA_BYTES;
    k_u32_t dev_sector;
    k_u32_t dev_off;
    k_u8_t sector_buf[4096];

    if (disk->sector_size == 0 || disk->sector_size > sizeof(sector_buf)) {
        return KERR_INVAL;
    }

    dev_sector = (k_u32_t)(byte_off / disk->sector_size);
    dev_off = (k_u32_t)(byte_off % disk->sector_size);

    if (dev_off + PARTITION_LBA_BYTES > disk->sector_size) {
        return KERR_INVAL;
    }

    if (dev_sector >= disk->sectors) {
        return KERR_IO;
    }

    if (disk->sector_size != PARTITION_LBA_BYTES) {
        if (disk->ops->read(disk->ctx, dev_sector, sector_buf, 1) != 0) {
            return KERR_IO;
        }
    }

    mem_copy(sector_buf + dev_off, in, PARTITION_LBA_BYTES);

    if (disk->ops->write(disk->ctx, dev_sector, sector_buf, 1) != 0) {
        return KERR_IO;
    }

    return 0;
}

/* --- inventory --- */

static bool mbr_is_hybrid(const k_u8_t *mbr)
{
    k_size_t slot;
    bool has_protective = false;
    bool has_other = false;

    if (mbr[PARTITION_MBR_SIGNATURE] != 0x55u || mbr[PARTITION_MBR_SIGNATURE + 1u] != 0xAAu) {
        return false;
    }

    for (slot = 0; slot < PARTITION_MBR_ENTRIES; slot++) {
        const k_u8_t *entry = mbr + PARTITION_MBR_OFFSET + slot * PARTITION_MBR_ENTRY;
        k_u8_t type = entry[4];

        if (type == PARTITION_MBR_TYPE_EMPTY) {
            continue;
        }

        if (type == PARTITION_MBR_TYPE_PROTECTIVE) {
            has_protective = true;
        } else {
            has_other = true;
        }
    }

    return has_protective && has_other;
}

static void entry_flags_from_guid(const k_u8_t guid[16], const char *label, k_u32_t *flags)
{
    k_size_t i;
    bool microos_label = false;
    static const char prefix[] = "MICROOS";

    *flags = 0;

    if (guid_is_zero(guid)) {
        return;
    }

    if (guid_eq(guid, g_guid_esp)) {
        *flags |= PARTITION_FLAG_ESP;
        return;
    }

    /* A label that names this system is ours even when the type is the
     * generic Linux data GUID the installer uses today. */
    microos_label = true;
    for (i = 0; i < 7u; i++) {
        if (label[i] != prefix[i]) {
            microos_label = false;
            break;
        }
    }

    if (microos_label || guid_eq(guid, g_guid_linux_data)) {
        /* Linux data is a data partition; only the MICROOS label makes it
         * "ours". Other Linux data partitions are foreign data, not ours. */
        if (microos_label) {
            *flags |= PARTITION_FLAG_OURS;
        } else {
            *flags |= PARTITION_FLAG_FOREIGN_OS;
        }
        return;
    }

    /* Any other non-empty typed partition is treated as foreign: the
     * installer must not guess that an unknown type is safe to touch. */
    *flags |= PARTITION_FLAG_FOREIGN_OS;
}

static void utf16le_to_ascii(const k_u8_t *src, k_size_t src_bytes, char *out, k_size_t out_size)
{
    k_size_t i;
    k_size_t o = 0;
    k_size_t chars = src_bytes / 2u;

    if (out_size == 0) {
        return;
    }

    for (i = 0; i < chars && o + 1u < out_size; i++) {
        k_u16_t code = (k_u16_t)src[i * 2u] | ((k_u16_t)src[i * 2u + 1u] << 8);

        if (code == 0) {
            break;
        }

        out[o++] = (code >= 0x20u && code < 0x7Fu) ? (char)code : '?';
    }

    out[o] = '\0';
}

static int inventory_read_gpt_entries(const struct k_blockdev *disk, const struct gpt_geometry *geo,
                                      struct partition_inventory *inv)
{
    k_u8_t chunk[4096];
    k_u64_t left = (k_u64_t)geo->entries * geo->entry_size;
    k_u32_t sector = (k_u32_t)(geo->entries_lba * PARTITION_LBA_BYTES / disk->sector_size);
    k_u32_t number = 1;
    k_u32_t chunk_bytes;

    if (disk->sector_size > sizeof(chunk)) {
        return KERR_INVAL;
    }

    inv->entry_count = 0;

    while (left > 0) {
        k_size_t take = left > disk->sector_size ? disk->sector_size : (k_size_t)left;
        k_size_t offset;

        if (disk->ops->read(disk->ctx, sector, chunk, 1) != 0) {
            return KERR_IO;
        }

        for (offset = 0; offset + geo->entry_size <= take; offset += geo->entry_size) {
            const k_u8_t *entry = chunk + offset;
            struct partition_entry_info *info;
            k_u64_t first;
            k_u64_t last;

            if (guid_is_zero(entry + PARTITION_GPT_ENTRY_TYPE)) {
                number++;
                continue;
            }

            if (inv->entry_count >= PARTITION_INVENTORY_MAX) {
                return KERR_NOSPC;
            }

            first = read_le64(entry + PARTITION_GPT_ENTRY_FIRST);
            last = read_le64(entry + PARTITION_GPT_ENTRY_LAST);

            if (last < first || first < geo->first_usable || last > geo->last_usable) {
                return KERR_INVAL;
            }

            info = &inv->entries[inv->entry_count];
            info->index = number;
            info->first_lba = first;
            info->last_lba = last;
            info->sectors = last - first + 1u;
            mem_copy(info->type_guid, entry + PARTITION_GPT_ENTRY_TYPE, 16u);
            info->type_mbr = 0;
            utf16le_to_ascii(entry + 56u, geo->entry_size >= 128u ? 72u : geo->entry_size - 56u,
                             info->label, sizeof(info->label));
            entry_flags_from_guid(info->type_guid, info->label, &info->flags);

            if ((info->flags & PARTITION_FLAG_ESP) != 0) {
                inv->has_esp = true;
            }

            inv->entry_count++;
            number++;
        }

        sector++;
        left -= take;
        chunk_bytes = 0;
        (void)chunk_bytes;
    }

    return 0;
}

static void inventory_sort_entries(struct partition_inventory *inv)
{
    k_u32_t i;

    for (i = 1; i < inv->entry_count; i++) {
        struct partition_entry_info key = inv->entries[i];
        k_u32_t j = i;

        while (j > 0 && inv->entries[j - 1u].first_lba > key.first_lba) {
            inv->entries[j] = inv->entries[j - 1u];
            j--;
        }

        inv->entries[j] = key;
    }
}

static k_u64_t align_up_lba(k_u64_t value, k_u64_t align)
{
    k_u64_t rem = value % align;

    if (rem == 0u) {
        return value;
    }

    return value + (align - rem);
}

static void inventory_add_gap(struct partition_inventory *inv, k_u64_t raw_first, k_u64_t raw_last)
{
    struct partition_gap *gap;
    k_u64_t aligned_first;

    if (raw_last < raw_first) {
        return;
    }

    if (inv->gap_count >= PARTITION_GAPS_MAX) {
        return;
    }

    gap = &inv->gaps[inv->gap_count];
    gap->raw_first_lba = raw_first;
    gap->raw_sectors = raw_last - raw_first + 1u;
    aligned_first = align_up_lba(raw_first, PARTITION_ALIGN_LBA);
    gap->first_lba = aligned_first;

    if (aligned_first > raw_last) {
        gap->last_lba = raw_last;
        gap->sectors = 0;
    } else {
        gap->last_lba = raw_last;
        gap->sectors = raw_last - aligned_first + 1u;
    }

    inv->gap_count++;
}

static void inventory_compute_gaps(struct partition_inventory *inv)
{
    k_u64_t cursor;
    k_u32_t i;

    inv->gap_count = 0;
    cursor = inv->first_usable;

    for (i = 0; i < inv->entry_count; i++) {
        const struct partition_entry_info *e = &inv->entries[i];

        if (e->first_lba > cursor) {
            inventory_add_gap(inv, cursor, e->first_lba - 1u);
        }

        if (e->last_lba + 1u > cursor) {
            cursor = e->last_lba + 1u;
        }
    }

    if (cursor <= inv->last_usable) {
        inventory_add_gap(inv, cursor, inv->last_usable);
    }
}

int partition_inventory(const struct k_blockdev *disk, struct partition_inventory *out)
{
    k_u8_t mbr[512];
    k_u8_t header[512];
    k_u8_t backup_header[512];
    struct gpt_geometry geo;
    struct gpt_geometry backup_geo;
    k_u64_t disk_lbas;
    int rc;

    if (disk == NULL || out == NULL || disk->ops == NULL || disk->ops->read == NULL) {
        return KERR_INVAL;
    }

    mem_set(out, 0, sizeof(*out));
    out->sector_size = disk->sector_size;
    out->disk_sectors = disk->sectors;
    disk_lbas = (k_u64_t)disk->sectors * (disk->sector_size / PARTITION_LBA_BYTES);
    out->disk_lbas = disk_lbas;

    if (disk->sector_size == 0 || (PARTITION_LBA_BYTES % disk->sector_size != 0 &&
                                   disk->sector_size % PARTITION_LBA_BYTES != 0)) {
        return KERR_INVAL;
    }

    rc = disk_read_lba512(disk, 0, mbr);
    if (rc != 0) {
        return KERR_IO;
    }
    rc = disk_read_lba512(disk, 1, header);
    if (rc != 0) {
        return KERR_IO;
    }

    if (!gpt_signature_ok(header)) {
        return KERR_NOENT;
    }

    out->gpt = true;
    out->hybrid_mbr = mbr_is_hybrid(mbr);
    if (out->hybrid_mbr) {
        return KERR_IO;
    }

    rc = gpt_geometry_read(disk, header, disk_lbas, 1u, &geo);
    if (rc != 0) {
        return rc == KERR_INVAL ? KERR_IO : rc;
    }
    if (!gpt_entries_crc_ok(disk, &geo)) {
        return KERR_IO;
    }

    /* The backup GPT is the other half of the table. A missing or broken
     * backup is a refusal: the installer never writes one copy alone. */
    if (disk_lbas < 2u) {
        return KERR_IO;
    }

    rc = disk_read_lba512(disk, disk_lbas - 1u, backup_header);
    if (rc != 0) {
        return KERR_IO;
    }
    if (!gpt_signature_ok(backup_header)) {
        return KERR_IO;
    }
    rc = gpt_geometry_read(disk, backup_header, disk_lbas, disk_lbas - 1u, &backup_geo);
    if (rc != 0) {
        return KERR_IO;
    }
    if (!gpt_entries_crc_ok(disk, &backup_geo)) {
        return KERR_IO;
    }
    if (geo.entries != backup_geo.entries || geo.entry_size != backup_geo.entry_size ||
        geo.first_usable != backup_geo.first_usable || geo.last_usable != backup_geo.last_usable ||
        geo.entries_crc != backup_geo.entries_crc) {
        return KERR_IO;
    }

    out->backup_ok = true;
    out->first_usable = geo.first_usable;
    out->last_usable = geo.last_usable;

    rc = inventory_read_gpt_entries(disk, &geo, out);
    if (rc != 0) {
        return rc;
    }

    inventory_sort_entries(out);
    inventory_compute_gaps(out);

    return 0;
}

/* --- GPT append --- */

static int gpt_read_array(const struct k_blockdev *disk, k_u64_t lba, k_u32_t bytes, k_u8_t *buf)
{
    k_u64_t byte_off = lba * PARTITION_LBA_BYTES;
    k_u32_t done = 0;

    while (done < bytes) {
        k_u8_t sector[4096];
        k_u32_t dev_sector = (k_u32_t)((byte_off + done) / disk->sector_size);
        k_u32_t sec_off = (k_u32_t)((byte_off + done) % disk->sector_size);
        k_u32_t avail = disk->sector_size - sec_off;
        k_u32_t take = bytes - done < avail ? bytes - done : avail;

        if (disk->sector_size > sizeof(sector)) {
            return KERR_INVAL;
        }

        if (dev_sector >= disk->sectors) {
            return KERR_IO;
        }

        if (disk->ops->read(disk->ctx, dev_sector, sector, 1) != 0) {
            return KERR_IO;
        }

        mem_copy(buf + done, sector + sec_off, take);
        done += take;
    }

    return 0;
}

static int gpt_write_array(const struct k_blockdev *disk, k_u64_t lba, k_u32_t bytes,
                           const k_u8_t *buf)
{
    k_u64_t byte_off = lba * PARTITION_LBA_BYTES;
    k_u32_t done = 0;

    while (done < bytes) {
        k_u8_t sector[4096];
        k_u32_t dev_sector = (k_u32_t)((byte_off + done) / disk->sector_size);
        k_u32_t sec_off = (k_u32_t)((byte_off + done) % disk->sector_size);
        k_u32_t avail = disk->sector_size - sec_off;
        k_u32_t take = bytes - done < avail ? bytes - done : avail;

        if (disk->sector_size > sizeof(sector)) {
            return KERR_INVAL;
        }

        if (dev_sector >= disk->sectors) {
            return KERR_IO;
        }

        if (take < disk->sector_size) {
            if (disk->ops->read(disk->ctx, dev_sector, sector, 1) != 0) {
                return KERR_IO;
            }
        }

        mem_copy(sector + sec_off, buf + done, take);

        if (disk->ops->write(disk->ctx, dev_sector, sector, 1) != 0) {
            return KERR_IO;
        }

        done += take;
    }

    return 0;
}

static int disk_sync(const struct k_blockdev *disk)
{
    if (disk->ops->sync != NULL) {
        return disk->ops->sync(disk->ctx);
    }

    return 0;
}

static void gpt_header_update_crc(k_u8_t *header)
{
    k_size_t size = (k_size_t)read_le32(header + PARTITION_GPT_HEADER_SIZE);

    if (size < PARTITION_GPT_HEADER_MIN || size > PARTITION_LBA_BYTES) {
        return;
    }

    write_le32(header + PARTITION_GPT_HEADER_CRC, 0u);
    write_le32(header + PARTITION_GPT_HEADER_CRC, crc32(header, size));
}

static void gpt_make_partition_guid(k_u8_t guid[16], k_u64_t start_lba, k_u64_t sector_count)
{
    /* A deterministic, unique-enough GUID: not random, but stable for the
     * same place on the disk. The installer makes one partition at a time
     * and the GUID only has to not collide with the ones already there. */
    guid[0] = 'M';
    guid[1] = 'O';
    guid[2] = 'S';
    guid[3] = 0x01u;
    write_le64(guid + 4, start_lba);
    write_le32(guid + 12, (k_u32_t)(sector_count & 0xFFFFFFFFu));
}

static int gpt_entry_slot(const k_u8_t *entries, k_u32_t count, k_u32_t entry_size)
{
    k_u32_t i;

    for (i = 0; i < count; i++) {
        if (guid_is_zero(entries + (k_size_t)i * entry_size + PARTITION_GPT_ENTRY_TYPE)) {
            return (int)i;
        }
    }

    return -1;
}

static bool gpt_range_intersects(const k_u8_t *entries, k_u32_t count, k_u32_t entry_size,
                                 k_u64_t start, k_u64_t end)
{
    k_u32_t i;

    for (i = 0; i < count; i++) {
        const k_u8_t *e = entries + (k_size_t)i * entry_size;
        k_u64_t first;
        k_u64_t last;

        if (guid_is_zero(e + PARTITION_GPT_ENTRY_TYPE)) {
            continue;
        }

        first = read_le64(e + PARTITION_GPT_ENTRY_FIRST);
        last = read_le64(e + PARTITION_GPT_ENTRY_LAST);

        if (start <= last && first <= end) {
            return true;
        }
    }

    return false;
}

static bool gpt_entries_other_same(const k_u8_t *before, const k_u8_t *after, k_u32_t bytes,
                                   k_u32_t skip_off, k_u32_t skip_len)
{
    k_u32_t i;

    for (i = 0; i < bytes; i++) {
        if (i >= skip_off && i < skip_off + skip_len) {
            continue;
        }

        if (before[i] != after[i]) {
            return false;
        }
    }

    return true;
}

static bool gpt_entry_matches(const k_u8_t *entry, k_u64_t start, k_u64_t end,
                              const k_u8_t type_guid[16], const char *label)
{
    k_u8_t want_label[72];
    k_size_t i;
    k_size_t len = 0;

    if (!mem_equal(entry + PARTITION_GPT_ENTRY_TYPE, type_guid, 16u)) {
        return false;
    }

    if (read_le64(entry + PARTITION_GPT_ENTRY_FIRST) != start) {
        return false;
    }

    if (read_le64(entry + PARTITION_GPT_ENTRY_LAST) != end) {
        return false;
    }

    mem_set(want_label, 0, sizeof(want_label));
    if (label != NULL) {
        while (label[len] != '\0' && len < 36u) {
            want_label[len * 2u] = (k_u8_t)label[len];
            want_label[len * 2u + 1u] = 0;
            len++;
        }
    }

    for (i = 0; i < 72u; i++) {
        if (entry[56u + i] != want_label[i]) {
            return false;
        }
    }

    return true;
}

int partition_append(const struct k_blockdev *disk, k_u64_t start_lba, k_u64_t sector_count,
                     const k_u8_t type_guid[PARTITION_GUID_SIZE], const char *label)
{
    k_u8_t mbr[512];
    k_u8_t primary_header[512];
    k_u8_t backup_header[512];
    k_u8_t saved_ph[512];
    k_u8_t saved_bh[512];
    struct gpt_geometry geo;
    struct gpt_geometry backup_geo;
    k_u8_t *primary = NULL;
    k_u8_t *backup = NULL;
    k_u8_t *saved_primary = NULL;
    k_u8_t *saved_backup = NULL;
    k_u8_t *verify_primary = NULL;
    k_u8_t *verify_backup = NULL;
    k_u32_t array_bytes;
    k_u64_t disk_lbas;
    k_u64_t end_lba;
    int slot;
    int rc;

    if (disk == NULL || disk->ops == NULL || disk->ops->read == NULL || disk->ops->write == NULL ||
        type_guid == NULL) {
        return KERR_INVAL;
    }

    if (guid_is_zero(type_guid)) {
        return KERR_INVAL;
    }

    if (sector_count == 0) {
        return KERR_INVAL;
    }

    if ((start_lba % PARTITION_ALIGN_LBA) != 0u) {
        return KERR_INVAL;
    }

    disk_lbas = (k_u64_t)disk->sectors * (disk->sector_size / PARTITION_LBA_BYTES);
    if (disk_lbas < 34u) {
        return KERR_INVAL;
    }

    end_lba = start_lba + sector_count - 1u;
    if (end_lba < start_lba) {
        return KERR_INVAL;
    }

    rc = disk_read_lba512(disk, 0, mbr);
    if (rc != 0) {
        return KERR_IO;
    }

    rc = disk_read_lba512(disk, 1, primary_header);
    if (rc != 0) {
        return KERR_IO;
    }

    if (!gpt_signature_ok(primary_header)) {
        return KERR_NOENT;
    }

    if (mbr_is_hybrid(mbr)) {
        return KERR_IO;
    }

    rc = gpt_geometry_read(disk, primary_header, disk_lbas, 1u, &geo);
    if (rc != 0) {
        return rc == KERR_INVAL ? KERR_IO : rc;
    }

    if (!gpt_entries_crc_ok(disk, &geo)) {
        return KERR_IO;
    }

    rc = disk_read_lba512(disk, disk_lbas - 1u, backup_header);
    if (rc != 0) {
        return KERR_IO;
    }

    if (!gpt_signature_ok(backup_header)) {
        return KERR_IO;
    }

    rc = gpt_geometry_read(disk, backup_header, disk_lbas, disk_lbas - 1u, &backup_geo);
    if (rc != 0) {
        return KERR_IO;
    }

    if (!gpt_entries_crc_ok(disk, &backup_geo)) {
        return KERR_IO;
    }

    if (geo.entries != backup_geo.entries || geo.entry_size != backup_geo.entry_size ||
        geo.first_usable != backup_geo.first_usable || geo.last_usable != backup_geo.last_usable ||
        geo.entries_crc != backup_geo.entries_crc) {
        return KERR_IO;
    }

    if (start_lba < geo.first_usable || end_lba > geo.last_usable) {
        return KERR_INVAL;
    }

    mem_copy(saved_ph, primary_header, 512);
    mem_copy(saved_bh, backup_header, 512);

    array_bytes = geo.entries * geo.entry_size;

    primary = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);
    backup = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);
    saved_primary = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);
    saved_backup = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);
    verify_primary = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);
    verify_backup = (k_u8_t *)kmm_alloc(array_bytes, KMM_PRIORITY_KERNEL);

    if (primary == NULL || backup == NULL || saved_primary == NULL || saved_backup == NULL ||
        verify_primary == NULL || verify_backup == NULL) {
        rc = KERR_NOMEM;
        goto out;
    }

    rc = gpt_read_array(disk, geo.entries_lba, array_bytes, primary);
    if (rc != 0) {
        goto out;
    }

    rc = gpt_read_array(disk, backup_geo.entries_lba, array_bytes, backup);
    if (rc != 0) {
        goto out;
    }

    if (!mem_equal(primary, backup, array_bytes)) {
        rc = KERR_IO;
        goto out;
    }

    if (gpt_range_intersects(primary, geo.entries, geo.entry_size, start_lba, end_lba)) {
        rc = KERR_INVAL;
        goto out;
    }

    slot = gpt_entry_slot(primary, geo.entries, geo.entry_size);
    if (slot < 0) {
        rc = KERR_NOSPC;
        goto out;
    }

    mem_copy(saved_primary, primary, array_bytes);
    mem_copy(saved_backup, backup, array_bytes);

    /* Write the new entry into both arrays. The partition GUID is
     * deterministic: the same place on the disk gives the same GUID. */
    {
        k_u8_t *pe = primary + (k_size_t)slot * geo.entry_size;
        k_u8_t *be = backup + (k_size_t)slot * geo.entry_size;
        k_u8_t pguid[16];
        k_size_t i;
        k_size_t len = 0;

        mem_set(pe, 0, geo.entry_size);
        mem_set(be, 0, geo.entry_size);

        mem_copy(pe + PARTITION_GPT_ENTRY_TYPE, type_guid, 16u);
        mem_copy(be + PARTITION_GPT_ENTRY_TYPE, type_guid, 16u);

        gpt_make_partition_guid(pguid, start_lba, sector_count);
        mem_copy(pe + 16u, pguid, 16u);
        mem_copy(be + 16u, pguid, 16u);

        write_le64(pe + PARTITION_GPT_ENTRY_FIRST, start_lba);
        write_le64(pe + PARTITION_GPT_ENTRY_LAST, end_lba);
        write_le64(be + PARTITION_GPT_ENTRY_FIRST, start_lba);
        write_le64(be + PARTITION_GPT_ENTRY_LAST, end_lba);

        /* Attributes: zero. */
        write_le64(pe + 48u, 0u);
        write_le64(be + 48u, 0u);

        if (label != NULL) {
            while (label[len] != '\0' && len < 36u) {
                len++;
            }

            for (i = 0; i < len; i++) {
                pe[56u + i * 2u] = (k_u8_t)label[i];
                pe[56u + i * 2u + 1u] = 0;
                be[56u + i * 2u] = (k_u8_t)label[i];
                be[56u + i * 2u + 1u] = 0;
            }
        }
    }

    /* Recompute CRCs: entries first, then header (header CRC covers the
     * entries CRC field). Primary and backup get the same entries CRC. */
    {
        k_u32_t entries_crc = crc32(primary, array_bytes);

        write_le32(primary_header + PARTITION_GPT_ENTRIES_CRC, entries_crc);
        write_le32(backup_header + PARTITION_GPT_ENTRIES_CRC, entries_crc);
        gpt_header_update_crc(primary_header);
        gpt_header_update_crc(backup_header);
    }

    /* Write backup first, then primary: after a crash the primary still
     * points at the old table until its header is written last. */
    rc = gpt_write_array(disk, backup_geo.entries_lba, array_bytes, backup);
    if (rc == 0) {
        rc = disk_write_lba512(disk, disk_lbas - 1u, backup_header);
    }

    if (rc == 0) {
        rc = disk_sync(disk);
    }

    if (rc == 0) {
        rc = gpt_write_array(disk, geo.entries_lba, array_bytes, primary);
    }

    if (rc == 0) {
        rc = disk_write_lba512(disk, 1u, primary_header);
    }

    if (rc == 0) {
        rc = disk_sync(disk);
    }

    /* Verify: re-read both arrays and headers, check the new entry is
     * present and every other entry is byte-identical to the saved copy. */
    if (rc == 0) {
        k_u8_t vph[512];
        k_u8_t vbh[512];

        rc = gpt_read_array(disk, geo.entries_lba, array_bytes, verify_primary);
        if (rc == 0) {
            rc = gpt_read_array(disk, backup_geo.entries_lba, array_bytes, verify_backup);
        }

        if (rc == 0) {
            rc = disk_read_lba512(disk, 1u, vph);
        }

        if (rc == 0) {
            rc = disk_read_lba512(disk, disk_lbas - 1u, vbh);
        }

        if (rc == 0) {
            k_u32_t slot_off = (k_u32_t)slot * geo.entry_size;
            k_u32_t want_crc = crc32(primary, array_bytes);

            if (!gpt_entry_matches(verify_primary + slot_off, start_lba, end_lba, type_guid,
                                   label)) {
                rc = KERR_IO;
            } else if (!gpt_entry_matches(verify_backup + slot_off, start_lba, end_lba, type_guid,
                                          label)) {
                rc = KERR_IO;
            } else if (!gpt_entries_other_same(saved_primary, verify_primary, array_bytes, slot_off,
                                               geo.entry_size)) {
                rc = KERR_IO;
            } else if (!gpt_entries_other_same(saved_backup, verify_backup, array_bytes, slot_off,
                                               geo.entry_size)) {
                rc = KERR_IO;
            } else if (!mem_equal(verify_primary, verify_backup, array_bytes)) {
                rc = KERR_IO;
            } else if (read_le32(vph + PARTITION_GPT_ENTRIES_CRC) != want_crc ||
                       read_le32(vbh + PARTITION_GPT_ENTRIES_CRC) != want_crc) {
                rc = KERR_IO;
            } else if (!gpt_header_crc_ok(vph) || !gpt_header_crc_ok(vbh)) {
                rc = KERR_IO;
            }
        }
    }

    if (rc != 0) {
        /* Restore the saved copies. A restore that itself fails is still
         * reported as the original error: the caller sees that the write did
         * not complete. The saved headers are the originals read before any
         * mutation, so the disk returns to the state it had before this call. */
        (void)gpt_write_array(disk, backup_geo.entries_lba, array_bytes, saved_backup);
        (void)disk_write_lba512(disk, disk_lbas - 1u, saved_bh);
        (void)gpt_write_array(disk, geo.entries_lba, array_bytes, saved_primary);
        (void)disk_write_lba512(disk, 1u, saved_ph);
        (void)disk_sync(disk);
    }

out:
    if (verify_backup != NULL) {
        kmm_free(verify_backup);
    }
    if (verify_primary != NULL) {
        kmm_free(verify_primary);
    }
    if (saved_backup != NULL) {
        kmm_free(saved_backup);
    }
    if (saved_primary != NULL) {
        kmm_free(saved_primary);
    }
    if (backup != NULL) {
        kmm_free(backup);
    }
    if (primary != NULL) {
        kmm_free(primary);
    }

    return rc;
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
