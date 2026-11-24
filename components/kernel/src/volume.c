/*
 * Micro OS — the rule of ADR 0041, ported from the plan into one reader.
 *
 * The disk of a person is not the disk of this system unless this system
 * made it. The bytes say which one it is: the header block of MicroFS at the
 * end of the volume is the identifier of this system, and the magic of an
 * ext2, btrfs, NTFS or XFS superblock is the identifier of somebody else's.
 * A medium that carries neither has no volume this kernel may write, and a
 * medium of this system is writable only when `/etc/volumes` names it.
 *
 * The second protection is geometric: the table of the partitions and the
 * boot area in front of the first partition belong to the medium, not to a
 * filesystem, and a write below the first sector of the first partition is
 * refused before it reaches a driver.
 *
 * Nothing in this file writes. The probes read a sector or two, the list is
 * read through the VFS, and every answer is an answer about bytes that are
 * already there.
 */
#include "kernel/volume.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

/*
 * The widest sector the probes read at once, and the widest unit they can
 * work in: a volume of this system is made of blocks of a kilobyte, and a
 * partition of a disk is a run of sectors of 512 bytes. A device of a wider
 * unit is not probed - four bytes read at the wrong offset say nothing.
 */
#define VOLUME_SECTOR_MAX 1024u

/* The unit of the two tables of a PC disk, whatever the medium calls a sector. */
#define VOLUME_LBA 512u

/* --- the table of the partitions --- */

#define VOLUME_MBR_ENTRIES 446u
#define VOLUME_MBR_COUNT 4u
#define VOLUME_MBR_ENTRY 16u
#define VOLUME_MBR_TYPE 4u
#define VOLUME_MBR_START 8u
#define VOLUME_MBR_SIGNATURE 510u
#define VOLUME_MBR_TYPE_PROTECTIVE 0xEEu

/* The header of the GPT lives in the second LBA, and the first sector it
 * lets a partition start at is what the writes may not cross. */
#define VOLUME_GPT_HEADER 512u
#define VOLUME_GPT_FIRST_USABLE 40u

/* --- the superblocks the rule knows --- */

#define VOLUME_MFS_BLOCK 1024u
#define VOLUME_MFS_MAGIC 0x4D465331u /* "MFS1", the header block of MicroFS */

/* ext2 and ext4 share one magic at the second kilobyte of the volume; the
 * feature word says which of the two the volume is (the extents, the 64-bit
 * block numbers and the flex groups are the three ext4 added). */
#define VOLUME_EXT2_MAGIC_OFFSET 1080u
#define VOLUME_EXT2_MAGIC 0xEF53u
#define VOLUME_EXT2_INCOMPAT 1120u /* 1024 + 96 */
#define VOLUME_EXT4_INCOMPAT (0x0040u | 0x0080u | 0x0200u)

#define VOLUME_BTRFS_MAGIC_OFFSET 0x10040u
#define VOLUME_NTFS_LABEL_OFFSET 3u
#define VOLUME_FAT_LABEL_OFFSET 82u

static k_u16_t read_le16(const k_u8_t *bytes)
{
    return (k_u16_t)((k_u16_t)bytes[0] | ((k_u16_t)bytes[1] << 8));
}

static k_u32_t read_le32(const k_u8_t *bytes)
{
    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

static k_u64_t read_le64(const k_u8_t *bytes)
{
    return (k_u64_t)read_le32(bytes) | ((k_u64_t)read_le32(bytes + 4) << 32);
}

/*
 * Read a run of bytes of the medium, whatever the offset and the unit of the
 * device: the sector that holds the first byte is read, the bytes are copied
 * out of it, and the next sector follows. A device of a wider unit than the
 * scratch, or a read past the end, is a refusal: the caller says "the bytes
 * are not there" instead of reading the wrong ones.
 */
static int volume_read(const struct k_blockdev *dev, k_u64_t offset, void *buf, k_size_t size)
{
    k_u8_t sector[VOLUME_SECTOR_MAX];
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t unit = dev->sector_size;

    if (unit == 0u || unit > sizeof(sector)) {
        return KERR_NOTSUP;
    }

    while (size > 0) {
        k_u64_t index = offset / unit;
        k_u32_t inside = (k_u32_t)(offset % unit);
        k_size_t chunk = (k_size_t)unit - inside;

        if (index >= dev->sectors) {
            return KERR_IO;
        }

        if (chunk > size) {
            chunk = size;
        }

        if (dev->ops->read(dev->ctx, (k_u32_t)index, sector, 1) != 0) {
            return KERR_IO;
        }

        memcpy(dst, sector + inside, chunk);
        dst += chunk;
        offset += chunk;
        size -= chunk;
    }

    return 0;
}

/* A boundary of the writes is a number of the sectors of the device: the
 * tables of a PC are written in units of 512 bytes, and a medium of a wider
 * sector holds several of them in one. */
static k_u32_t volume_lba_to_sectors(const struct k_blockdev *dev, k_u64_t lba)
{
    k_u64_t per_sector = dev->sector_size / VOLUME_LBA;
    k_u64_t sectors;

    if (per_sector == 0u) {
        return 0;
    }

    sectors = (lba + per_sector - 1u) / per_sector;

    return sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (k_u32_t)sectors;
}

/*
 * The first sector a write may name: the start of the first partition of the
 * table, which is the end of the boot area and of the table itself. A medium
 * without a table this reader understands answers zero, and then the rule
 * has no line to draw.
 */
static k_u32_t volume_table_end(const struct k_blockdev *dev)
{
    k_u8_t head[VOLUME_LBA];
    k_u64_t first = 0;
    bool gpt = false;
    k_size_t i;

    if (volume_read(dev, 0, head, sizeof(head)) != 0) {
        return 0;
    }

    if (head[VOLUME_MBR_SIGNATURE] != 0x55u || head[VOLUME_MBR_SIGNATURE + 1u] != 0xAAu) {
        return 0;
    }

    for (i = 0; i < VOLUME_MBR_COUNT; i++) {
        const k_u8_t *entry = head + VOLUME_MBR_ENTRIES + i * VOLUME_MBR_ENTRY;
        k_u8_t type = entry[VOLUME_MBR_TYPE];
        k_u32_t start = read_le32(entry + VOLUME_MBR_START);

        if (type == VOLUME_MBR_TYPE_PROTECTIVE) {
            gpt = true;
        } else if (type != 0u && start != 0u && (first == 0u || start < first)) {
            first = start;
        }
    }

    if (gpt) {
        k_u8_t header[VOLUME_GPT_HEADER];

        if (volume_read(dev, VOLUME_GPT_HEADER, header, sizeof(header)) == 0) {
            k_u64_t usable = read_le64(header + VOLUME_GPT_FIRST_USABLE);

            if (usable != 0u) {
                return volume_lba_to_sectors(dev, usable);
            }
        }

        /* A protective entry whose header does not read is still a table of
         * a GPT: the first sectors of the disk are the table's either way. */
        return volume_lba_to_sectors(dev, 34u);
    }

    return first != 0u ? volume_lba_to_sectors(dev, first) : 0u;
}

static bool bytes_are(const k_u8_t *bytes, const char *text, k_size_t size)
{
    return memcmp(bytes, text, size) == 0;
}

/* A medium nothing was ever written to: all ones, like a flash chip from the
 * factory, or all zeros, like a disk image nobody formatted. Neither is a
 * volume of anything, and both are a refusal of the writes: the rule does
 * not format a disk it did not make. */
static bool volume_is_blank(const struct k_blockdev *dev)
{
    k_u8_t sector[VOLUME_SECTOR_MAX];
    k_u32_t unit = dev->sector_size;
    k_size_t i;
    bool ones = true;
    bool zeros = true;

    if (unit == 0u || unit > sizeof(sector)) {
        return false;
    }

    /* The head first, then the same buffer for the tail: the rule runs on the
     * stack of a boot, and a board of a hundred kilobytes of RAM pays for
     * every kilobyte of it. */
    if (volume_read(dev, 0, sector, unit) != 0) {
        return false;
    }

    for (i = 0; i < unit; i++) {
        if (sector[i] != 0xFFu) {
            ones = false;
        }
        if (sector[i] != 0x00u) {
            zeros = false;
        }
    }

    if (!ones && !zeros) {
        return false;
    }

    if (volume_read(dev, (k_u64_t)(dev->sectors - 1u) * unit, sector, unit) != 0) {
        return false;
    }

    for (i = 0; i < unit; i++) {
        if (ones && sector[i] != 0xFFu) {
            ones = false;
        }
        if (zeros && sector[i] != 0x00u) {
            zeros = false;
        }
    }

    return ones || zeros;
}

/*
 * The superblock of the volume, as a name: "mfs" for the header block of
 * this system, the name of the other system for everybody else, NULL when
 * the bytes are no superblock this reader knows.
 *
 * The probes are the smallest read that decides the question: the magic of
 * MicroFS and the two words of the header that say it is a volume of this
 * release, one word of the ext2 family, the eight bytes of btrfs, the label
 * of NTFS and XFS, and the name of FAT32.
 */
static const char *volume_superblock(const struct k_blockdev *dev)
{
    k_u8_t bytes[8];
    k_u32_t magic;

    if (dev->sector_size == VOLUME_MFS_BLOCK && dev->sectors > 0u) {
        if (volume_read(dev, (k_u64_t)(dev->sectors - 1u) * VOLUME_MFS_BLOCK, bytes,
                        sizeof(bytes)) == 0) {
            magic = read_le32(bytes);

            if (magic == VOLUME_MFS_MAGIC) {
                k_u8_t block_size[4];

                if (volume_read(dev, (k_u64_t)(dev->sectors - 1u) * VOLUME_MFS_BLOCK + 8u,
                                block_size, sizeof(block_size)) == 0) {
                    if (read_le32(block_size) != VOLUME_MFS_BLOCK) {
                        kerror("volume: the header of '%s' says %u bytes of a block, not %u",
                               dev->name != NULL ? dev->name : "?", (unsigned)read_le32(block_size),
                               (unsigned)VOLUME_MFS_BLOCK);
                        return NULL;
                    }

                    return "mfs";
                }
            }
        }
    }

    if (volume_read(dev, VOLUME_BTRFS_MAGIC_OFFSET, bytes, sizeof(bytes)) == 0 &&
        bytes_are(bytes, "_BHRfS_M", sizeof(bytes))) {
        return "btrfs";
    }

    {
        k_u8_t boot[VOLUME_LBA];

        if (volume_read(dev, 0, boot, sizeof(boot)) == 0) {
            if (bytes_are(boot + VOLUME_NTFS_LABEL_OFFSET, "NTFS    ", 8u)) {
                return "ntfs";
            }

            if (bytes_are(boot, "XFSB", 4u)) {
                return "xfs";
            }

            if (bytes_are(boot + VOLUME_FAT_LABEL_OFFSET, "FAT32", 5u)) {
                return "fat32";
            }
        }
    }

    {
        k_u8_t word[2];
        k_u8_t incompat[4];

        if (volume_read(dev, VOLUME_EXT2_MAGIC_OFFSET, word, sizeof(word)) == 0 &&
            read_le16(word) == VOLUME_EXT2_MAGIC) {
            if (volume_read(dev, VOLUME_EXT2_INCOMPAT, incompat, sizeof(incompat)) == 0 &&
                (read_le32(incompat) & VOLUME_EXT4_INCOMPAT) != 0u) {
                return "ext4";
            }

            return "ext2";
        }
    }

    return NULL;
}

int k_volume_identify(const struct k_blockdev *dev, struct k_volume_id *out)
{
    const char *format;

    if (out == NULL) {
        return KERR_INVAL;
    }

    memset(out, 0, sizeof(*out));
    out->kind = K_VOLUME_UNKNOWN;

    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL || dev->sector_size == 0u ||
        dev->sectors == 0u) {
        return KERR_INVAL;
    }

    out->table_sectors = volume_table_end(dev);

    /*
     * The superblock comes first: a medium whose head and tail are blank but
     * whose second kilobyte carries the magic of ext2 is an ext2 volume of a
     * small disk, not a medium nothing was written to.
     */
    format = volume_superblock(dev);

    if (format == NULL) {
        out->kind = volume_is_blank(dev) ? K_VOLUME_BLANK : K_VOLUME_UNKNOWN;
        return 0;
    }

    out->format = format;
    out->kind = strcmp(format, "mfs") == 0 ? K_VOLUME_OURS : K_VOLUME_FOREIGN;

    return 0;
}

bool k_volume_list_has(const char *list, const char *name)
{
    k_size_t at = 0;
    k_size_t length;

    if (list == NULL || name == NULL || name[0] == '\0') {
        return false;
    }

    length = strlen(name);

    /* One line at a time: the loop always moves past the newline, so an
     * empty line, a comment and the last line without a newline all end. */
    while (list[at] != '\0') {
        k_size_t start = at;
        k_size_t end;
        k_size_t comment;

        while (list[at] != '\0' && list[at] != '\n') {
            at++;
        }

        end = at;

        if (list[at] == '\n') {
            at++;
        }

        while (start < end && (list[start] == ' ' || list[start] == '\t')) {
            start++;
        }

        while (end > start &&
               (list[end - 1u] == ' ' || list[end - 1u] == '\t' || list[end - 1u] == '\r')) {
            end--;
        }

        /* The rest of a line after a `#` is a comment and names nothing. */
        comment = start;

        while (comment < end && list[comment] != '#') {
            comment++;
        }

        end = comment;

        while (end > start && (list[end - 1u] == ' ' || list[end - 1u] == '\t')) {
            end--;
        }

        if (end - start == length && memcmp(list + start, name, length) == 0) {
            return true;
        }
    }

    return false;
}

bool k_volume_named(const struct k_blockdev *dev)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char list[K_VOLUME_LIST_MAX + 1u];
    int size;

    if (dev == NULL || dev->name == NULL) {
        return false;
    }

    if (vfs_open(K_VOLUME_LIST_PATH, VFS_O_RDONLY, &file, priv) != 0) {
        return false;
    }

    size = vfs_read(&file, list, K_VOLUME_LIST_MAX);
    (void)vfs_close(&file);

    if (size <= 0) {
        return false;
    }

    list[size] = '\0';

    return k_volume_list_has(list, dev->name);
}

bool k_volume_may_write(const struct k_blockdev *dev, const struct k_volume_id *id,
                        const char **why)
{
    const char *reason = NULL;

    if (id == NULL) {
        reason = "the medium was not read";
    } else if (id->kind == K_VOLUME_FOREIGN) {
        reason = "the medium carries a volume of another system";
    } else if (id->kind == K_VOLUME_BLANK) {
        reason = "the medium carries no volume";
    } else if (id->kind != K_VOLUME_OURS || id->format == NULL) {
        reason = "the medium carries no volume of this system";
    } else if (!k_volume_named(dev)) {
        reason = "the volume is not named in " K_VOLUME_LIST_PATH;
    }

    if (why != NULL) {
        *why = reason;
    }

    return reason == NULL;
}

bool k_volume_write_above_table(const struct k_volume_id *id, k_u32_t sector, k_size_t count)
{
    if (id == NULL || id->table_sectors == 0u) {
        return true;
    }

    if (count == 0) {
        return false;
    }

    return sector >= id->table_sectors;
}
