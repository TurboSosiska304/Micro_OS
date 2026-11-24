/*
 * Micro OS — the rule of the disks: what a volume is, and what may be written.
 *
 * A disk that arrives in a machine is somebody else's life: a table of
 * partitions, a boot area, a volume of a system this kernel has never seen.
 * The one sentence of ADR 0041 is that such a disk is read-only, and that a
 * volume is writable only when the superblock carries the identifier of this
 * system and the volume is named in `/etc/volumes` of the medium.
 *
 * This file is the rule, portable and without a driver in it: it reads the
 * first sectors of a device, says what the medium carries, and answers
 * whether a write at a sector may happen. A driver that is handed a new
 * controller asks this file, prints the answer (the first boot of a machine
 * with a new controller lists what it found and writes nothing), and refuses
 * the writes the answer refuses.
 *
 * Nothing here writes: every call is a reader of the medium, and a device
 * that cannot be read is a device that is not understood, not one that is
 * formatted.
 */
#ifndef MICRO_OS_KERNEL_VOLUME_H
#define MICRO_OS_KERNEL_VOLUME_H

#include "kernel/blockdev.h"
#include "kernel/types.h"

/* Where the list of the volumes of this system lives on a medium. */
#define K_VOLUME_LIST_PATH "/etc/volumes"

/* The longest line of /etc/volumes a reader keeps, and the file it reads. */
#define K_VOLUME_NAME_MAX 32
#define K_VOLUME_LIST_MAX 1024

/* What the bytes of a medium say it is. */
enum k_volume_kind {
    /* The volume of this system: the magic of MicroFS in the header block. */
    K_VOLUME_OURS = 0,
    /* The superblock of another system (ext2/ext4, btrfs, NTFS, XFS, FAT). */
    K_VOLUME_FOREIGN,
    /* Nothing was ever written: all ones (a flash) or all zeros (a disk). */
    K_VOLUME_BLANK,
    /* Neither a superblock nor a blank medium: the bytes say nothing. */
    K_VOLUME_UNKNOWN,
};

struct k_volume_id {
    enum k_volume_kind kind;
    /* The name of the format the superblock belongs to ("mfs", "ext4",
     * "btrfs", "ntfs", "xfs", "fat32"), NULL when nothing was recognised. */
    const char *format;
    /*
     * The first sector of the medium a write may name, in the sectors of the
     * device: the first partition of its table, in other words the end of
     * the boot area and of the table itself. Zero when the medium has no
     * table this reader understands, in which case the rule has no boundary
     * to draw and the whole medium is the volume.
     */
    k_u32_t table_sectors;
};

/*
 * Read the head of `dev` and say what it carries: the partition table (the
 * boundary of the writes) and the superblock (whose volume it is).
 *
 * Returns 0 with `out` filled, or a KERR_* code when the medium could not be
 * read at all (KERR_INVAL for a device without the calls, KERR_IO for one
 * that did not answer). The kind of a medium that was read but not
 * recognised is K_VOLUME_UNKNOWN, which is not an error of the call: the
 * driver has a disk and has to say what it found on it.
 */
int k_volume_identify(const struct k_blockdev *dev, struct k_volume_id *out);

/*
 * The parser of `/etc/volumes`: one device name per line, `#` starts a
 * comment, whitespace around a name is not part of it. `list` is the text of
 * the file (NUL terminated) and `name` is a device name ("sd0p1").
 *
 * A name matches a whole line and nothing else: "sd0p1" is not "sd0p1x",
 * and a line of a longer name does not name a shorter one.
 */
bool k_volume_list_has(const char *list, const char *name);

/*
 * Whether `/etc/volumes` of the mounted root names `dev`. False when the
 * file is not there, cannot be read, or does not name the device - all three
 * are the same answer to the rule, and the sentence of the refusal names the
 * file either way.
 */
bool k_volume_named(const struct k_blockdev *dev);

/*
 * The rule as one call: may a write to `dev` happen at all? True only for a
 * volume of this system that `/etc/volumes` names; `*why` is a sentence for
 * the log otherwise (a pointer to a constant, never NULL when the answer is
 * false).
 *
 * `id` is the answer of k_volume_identify() for the same device, which the
 * caller has read already: a write path should not read the head of the disk
 * again for every sector.
 */
bool k_volume_may_write(const struct k_blockdev *dev, const struct k_volume_id *id,
                        const char **why);

/*
 * The second half of the rule: a write of `count` sectors at `sector` must
 * not touch the boot area or the partition table of the medium. True when
 * the request is above the boundary of `id` (or when the medium has no
 * boundary), false when any of its sectors is the table's.
 *
 * The caller is a driver of a whole disk: a partition is a child device that
 * adds the offset of its first sector to every request, so the request has
 * already left the table behind by the time it reaches the medium.
 */
bool k_volume_write_above_table(const struct k_volume_id *id, k_u32_t sector, k_size_t count);

#endif /* MICRO_OS_KERNEL_VOLUME_H */
