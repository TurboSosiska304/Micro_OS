/*
 * Micro OS — the type of the volume of the root, and the mount of it.
 *
 * The root of a machine is a volume of some filesystem, and which one is a
 * fact of the medium (ADR 0027). The port says where the boot looks - a
 * device and, when the disk has a table, the partition of it - and this file
 * asks the bytes: the type of the partition is a hint, the magic of the
 * superblock is the volume, and the registry of the VFS says which types the
 * image was built with and in which order they get to claim a volume.
 *
 * The kernel knows the three magics of the tree as numbers, not as calls of
 * the components: a probe of a type is "read these bytes of this device", and
 * a filesystem that is not in the image costs a line of the table and no
 * link. The formats themselves are untouched - a probe is a reader of the
 * first block, and the mount that follows is the filesystem's own.
 */
#include "kernel/root.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/partition.h"
#include "kernel/volume.h"
#include "kernel/vfs.h"

/*
 * The widest sector the probe reads at once. The volumes of this system speak
 * 512 bytes (a partition of a disk) or 1024 (a block of MicroFS, the unit of
 * the flash and of the port); a device of a wider unit is not probed, and the
 * magic that would have been found says nothing instead of being read from
 * the wrong offset.
 */
#define ROOT_SECTOR_MAX 1024u

/* The header block of MicroFS: the last block of the volume, and the word
 * "MFS1" at its start (components/mfs, the layout of docs/memory.md). */
#define ROOT_MFS_BLOCK_SIZE 1024u
#define ROOT_MFS_MAGIC 0x4D465331u

/* The superblock of ext2 lives at the second kilobyte of the volume, and its
 * magic is the word at 56 bytes into it (components/fs_ext2). */
#define ROOT_EXT2_MAGIC_OFFSET 1080u
#define ROOT_EXT2_MAGIC 0xEF53u

/* The BPB of a FAT volume: the label of the format at 82 bytes of the boot
 * sector, and the signature every boot sector of a PC ends with. */
#define ROOT_FAT_LABEL_OFFSET 82u
#define ROOT_FAT_SIGNATURE_OFFSET 510u

/* The MBR type bytes the boot knows: the hint, not the answer. */
#define ROOT_MBR_TYPE_LINUX 0x83
#define ROOT_MBR_TYPE_FAT32_CHS 0x0B
#define ROOT_MBR_TYPE_FAT32_LBA 0x0C

/* The bytes of the medium read at the head of a volume that is not known. */
#define ROOT_HEAD_BYTES 4u

/* The number of the bytes a blank medium is made of. */
#define ROOT_BLANK_CHECK 512u

struct root_probe {
    const char *type; /* the name of the type in the registry of the VFS */
    bool (*mine)(const struct k_blockdev *dev);
};

static bool probe_mfs(const struct k_blockdev *dev);
static bool probe_ext2(const struct k_blockdev *dev);
static bool probe_fat32(const struct k_blockdev *dev);

/* The probes of the image, in the order the ambiguity of ADR 0027 resolves
 * it: the registry of the VFS is walked, and this table only says what to
 * read for a name. A type that is not here has no probe and is never the
 * root of a medium it did not claim by a call of the VFS. */
static const struct root_probe g_probes[] = {
    {"mfs", probe_mfs},
    {"ext2", probe_ext2},
    {"fat32", probe_fat32},
};

static k_u16_t read_le16(const k_u8_t *bytes)
{
    return (k_u16_t)((k_u16_t)bytes[0] | ((k_u16_t)bytes[1] << 8));
}

static k_u32_t read_le32(const k_u8_t *bytes)
{
    return (k_u32_t)bytes[0] | ((k_u32_t)bytes[1] << 8) | ((k_u32_t)bytes[2] << 16) |
           ((k_u32_t)bytes[3] << 24);
}

/*
 * Read a run of bytes of the medium. A sector is the unit of the device, so
 * an offset is placed in its sector and the sector is read once; the run of
 * the caller may cross two of them. A device of a wider sector than the
 * buffer, or a read past the end of the volume, is a refusal: the probe of a
 * type says no rather than reading the bytes at an offset that is not there.
 */
static int read_bytes(const struct k_blockdev *dev, k_u64_t offset, void *buf, k_size_t size)
{
    k_u8_t sector[ROOT_SECTOR_MAX];
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t unit = dev->sector_size;

    if (dev->ops == NULL || dev->ops->read == NULL || unit == 0 || unit > sizeof(sector)) {
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

static bool probe_mfs(const struct k_blockdev *dev)
{
    k_u8_t header[4];

    /*
     * The volume is a run of blocks of one kilobyte (the contract of the
     * filesystem), and its header is the last block: a device of another
     * unit could not hold the volume even if these four bytes matched.
     */
    if (dev->sector_size != ROOT_MFS_BLOCK_SIZE || dev->sectors == 0) {
        return false;
    }

    if (read_bytes(dev, (k_u64_t)(dev->sectors - 1u) * ROOT_MFS_BLOCK_SIZE, header,
                   sizeof(header)) != 0) {
        return false;
    }

    return read_le32(header) == ROOT_MFS_MAGIC;
}

static bool probe_ext2(const struct k_blockdev *dev)
{
    k_u8_t magic[2];

    if (read_bytes(dev, ROOT_EXT2_MAGIC_OFFSET, magic, sizeof(magic)) != 0) {
        return false;
    }

    return read_le16(magic) == ROOT_EXT2_MAGIC;
}

static bool probe_fat32(const struct k_blockdev *dev)
{
    k_u8_t boot[512];

    if (read_bytes(dev, 0, boot, sizeof(boot)) != 0) {
        return false;
    }

    if (boot[ROOT_FAT_SIGNATURE_OFFSET] != 0x55u || boot[ROOT_FAT_SIGNATURE_OFFSET + 1u] != 0xAAu) {
        return false;
    }

    return memcmp(boot + ROOT_FAT_LABEL_OFFSET, "FAT32", 5u) == 0;
}

/*
 * A medium nothing was ever written to: the first and the last sector are all
 * ones, the way a flash chip comes from the factory. It is not a volume of any
 * type and it is not a damaged one either - it is where a format may put a
 * filesystem, and the caller does that. The test is the one MicroFS makes
 * before it formats a volume (components/mfs): a medium that reads as zeros is
 * a device, not a blank flash, and a format of it would be a guess.
 */
static bool medium_is_blank(const struct k_blockdev *dev)
{
    k_u8_t head[ROOT_BLANK_CHECK];
    k_u8_t tail[ROOT_BLANK_CHECK];
    k_u64_t end;
    k_size_t i;

    if (dev->sectors == 0) {
        return false;
    }

    if (read_bytes(dev, 0, head, sizeof(head)) != 0) {
        return false;
    }

    end = (k_u64_t)dev->sectors * dev->sector_size - sizeof(tail);

    if (read_bytes(dev, end, tail, sizeof(tail)) != 0) {
        return false;
    }

    for (i = 0; i < sizeof(head); i++) {
        if (head[i] != 0xFFu || tail[i] != 0xFFu) {
            return false;
        }
    }

    return true;
}

/* What the partition table of the device says the volume is, when it says
 * anything this boot knows how to use. */
static const char *hint_of(int type)
{
    switch (type) {
    case ROOT_MBR_TYPE_LINUX:
        return "ext2"; /* the volume `mkfs.ext2` writes, and the default of Linux */
    case ROOT_MBR_TYPE_FAT32_CHS:
    case ROOT_MBR_TYPE_FAT32_LBA:
        return "fat32";
    default:
        return NULL;
    }
}

static const struct root_probe *probe_of(const char *type)
{
    k_size_t i;

    if (type == NULL) {
        return NULL;
    }

    for (i = 0; i < sizeof(g_probes) / sizeof(g_probes[0]); i++) {
        if (strcmp(g_probes[i].type, type) == 0) {
            return &g_probes[i];
        }
    }

    return NULL;
}

const char *k_root_type(const char *device)
{
    const struct k_blockdev *dev;
    const struct root_probe *probe;
    const struct root_probe *found = NULL;
    const char *hint;
    const char *name;
    k_u8_t head[ROOT_HEAD_BYTES];
    k_size_t i;

    if (device == NULL) {
        return NULL;
    }

    dev = k_blockdev_find(device);
    if (dev == NULL) {
        kerror("root: there is no block device '%s'", device);
        return NULL;
    }

    /*
     * The hint is asked first and only first: a volume whose bytes disagree
     * with the table is the bytes. A disk of a person may carry a MicroFS
     * volume in a partition somebody labelled "Linux" by hand, and this is
     * the line that keeps that disk booting.
     */
    hint = hint_of(partition_type_of(dev));
    if (hint != NULL && vfs_type_find(hint) >= 0) {
        probe = probe_of(hint);
        if (probe != NULL && probe->mine(dev)) {
            found = probe;
        }
    }

    /*
     * The registry of the types, in the order the image registered them:
     * the first type whose probe says "mine" is the root. The order is the
     * contract of ADR 0027 - two types that recognise the same bytes are
     * resolved by it - and the shell's `abi` and this list are the same.
     */
    for (i = 0; found == NULL && i < vfs_type_count(); i++) {
        name = vfs_type_name(i);

        if (hint != NULL && name != NULL && strcmp(name, hint) == 0) {
            continue; /* asked already, with the same answer */
        }

        probe = probe_of(name);
        if (probe != NULL && probe->mine(dev)) {
            found = probe;
        }
    }

    if (found != NULL) {
        if (hint != NULL && strcmp(hint, found->type) != 0) {
            kdebug("root: the table of %s says %s, the bytes say %s", device, hint, found->type);
        }

        return found->type;
    }

    if (medium_is_blank(dev) && vfs_type_find("mfs") >= 0) {
        /* A fresh medium is where MicroFS makes a volume, and a machine of
         * this system with a blank disk has always formatted it and booted. */
        kdebug("root: the medium of %s is blank, MicroFS is made on it", device);
        return "mfs";
    }

    /*
     * The sentence of a volume no type of the image knows: the partition and
     * the bytes, because a person can tell from the four bytes whether the
     * volume is of a format this machine does not have or of one it failed
     * to read. A medium of another system is named for what it is - the rule
     * of ADR 0041 refuses it at the mount - and a medium the rule does not
     * know is named by its first bytes.
     */
    {
        struct k_volume_id volume;

        if (k_volume_identify(dev, &volume) == 0 && volume.kind == K_VOLUME_FOREIGN &&
            volume.format != NULL) {
            kerror("root: %s carries the volume of another system (%s), this system did not "
                   "make it and does not mount it",
                   device, volume.format);
            return NULL;
        }
    }

    if (read_bytes(dev, 0, head, sizeof(head)) != 0) {
        kerror("root: the volume of %s cannot be read", device);
    } else {
        kerror(
            "root: no type of this image recognises %s (the first bytes are %02x %02x %02x %02x)",
            device, head[0], head[1], head[2], head[3]);
    }

    return NULL;
}

int k_root_mount(const char *device, const char **type)
{
    const char *found;
    int rc;

    found = k_root_type(device);

    if (type != NULL) {
        *type = found;
    }

    if (found == NULL) {
        return KERR_INVAL;
    }

    rc = vfs_mount_type(K_ROOT_POINT, found, device);
    if (rc != 0) {
        kerror("root: the volume of %s is %s and it does not mount (%s)", device, found,
               vfs_strerror(rc));
        return rc;
    }

    kinfo("root: the root is %s on %s", found, device);

    /*
     * The writes are a decision of the mount and of the rule (ADR 0041 §2):
     * the root is in the tree now, so its own /etc/volumes can be read, and
     * a volume of this system that the list names opens the door of the
     * controller. A refusal leaves the root mounted and read-only - the
     * state of an ext2 root, which this image can read and this rule does
     * not write - and the sentence of the refusal is in the log.
     */
    (void)vfs_mount_root_writes(K_ROOT_POINT, device);

    return 0;
}
