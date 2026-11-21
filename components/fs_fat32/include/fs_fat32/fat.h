/*
 * Micro OS — FAT12/16/32, read and write.
 *
 * The filesystem of the cards and of the disks the rest of the world
 * writes: a camera, a laptop, a phone. The kernel reads it and now writes
 * it, but with no journal to replay and no cache of its own: every change
 * reaches the medium before the call returns, and a power cut in the middle
 * of one is a risk the volume knows about (ADR 0009).
 *
 * A FAT record has no mode, no owner and no time, so `stat` answers zero for
 * the first two and nothing reports the third; `chmod` and `chown` stay
 * KERR_NOTSUP.
 */
#ifndef MICRO_OS_FS_FAT32_FAT_H
#define MICRO_OS_FS_FAT32_FAT_H

#include "kernel/types.h"

/*
 * Mount the volume of the block device at a point of the tree. The device
 * is looked up by name in the registry of the kernel, the way MicroFS does
 * it, so this call has no platform in it.
 *
 * Returns 0, or a KERR_* code: KERR_NOENT for a name the registry does not
 * know, KERR_INVAL for a boot sector that is not a FAT volume, KERR_IO for
 * a medium that did not answer, KERR_NOTSUP for a geometry this reader does
 * not speak. The mount does not ask whether the medium can be written: a
 * card with its switch on "read only" mounts and refuses the writes.
 */
int fat_vfs_mount(const char *point, const char *device);

/*
 * Put the type in the registry of the VFS, so that vfs_mount_type(point,
 * "fat32", device) finds it. An image that includes this filesystem calls
 * it at the boot, before anything asks for a FAT volume; the host tests
 * call it themselves.
 */
int fat_vfs_register_type(void);

/* The text of an error of this filesystem, for the shell and the tests. */
const char *fat_strerror(int error);

/* The room for the first problem that fat_check() finds. */
#define FAT_CHECK_MESSAGE_MAX 96

/*
 * What fat_check() found. The counters describe the volume as it is on the
 * medium: the clusters of the data, the ones no chain names, the entries of
 * the tree and the free clusters of the table. A repair adds what it
 * changed: `fixed` and `unfixed` are the operations it applied and the
 * problems it left, and `lost_found`/`lost_freed` are the clusters that
 * were marked used while no chain named them, before and after the pass.
 */
struct fat_check_report {
    k_u32_t type;          /* 12, 16 or 32 */
    k_u32_t clusters;      /* the clusters of the data section */
    k_u32_t free_clusters; /* the entries of the table that are zero */
    k_u32_t lost_clusters; /* used in the table, named by no chain */
    k_u32_t files;
    k_u32_t directories;
    k_u32_t errors;     /* 0 for a sound volume; after a repair, what is left */
    k_u32_t fixed;      /* the changes a repair applied, 0 for a check */
    k_u32_t unfixed;    /* the problems a repair could not make safe, 0 for a check */
    k_u32_t lost_found; /* the lost clusters the repair pass met */
    k_u32_t lost_freed; /* the lost clusters it gave back to the free map */
    /* The first problem in English, an empty string for a sound volume. */
    char first[FAT_CHECK_MESSAGE_MAX];
};

/*
 * Check the volume of a mount point or of a block device, read-only.
 *
 * `what` is either the point the volume of this reader is mounted at
 * ("/mnt/fat") or the name of a block device ("sd0"). A device that is not
 * mounted is mounted for the length of the check and forgotten again; a
 * volume that is already mounted is checked where it is, under its lock.
 *
 * Returns 0 when the report was made, even when it names problems; the
 * caller reads report.errors for the answer. KERR_INVAL is a point that is
 * not the volume of this reader, KERR_NOENT a device the kernel does not
 * know, and KERR_NOMEM a check that could not take the lock.
 *
 * The check never writes: a damaged volume is left exactly as it was found.
 * It covers the boot sector and the BPB, the copies of the table, the
 * chains (a loop, a cross-link, a cluster outside the volume), the entries
 * of the root and of the subdirectories, the names and their long-name
 * runs, and the clusters that no chain names. The volumes up to 65536
 * clusters are checked whole; a bigger one is refused with a message
 * instead of being walked with a map that would hide a cross-link.
 */
int fat_check(const char *what, struct fat_check_report *report);

/*
 * Check the volume, and with `write` repair the damage that can be repaired
 * without guessing what the data was. Without `write` this is fat_check().
 *
 * The safe repairs are the ones where the medium itself says what the
 * volume meant: a cluster no chain names goes back to the free map, a chain
 * that loops or runs outside the volume is cut at the last cluster that is
 * inside it, a cluster two chains name stays with the first and the second
 * is cut before it (an entry whose own first cluster is the shared one
 * becomes an empty file), a broken run of long-name entries is removed and
 * the 8.3 name kept, a size larger than its chain is cut down and a chain
 * longer than its file gives the tail back, and an entry whose first
 * cluster is not in the volume is removed. A file whose 8.3 name holds a
 * character FAT does not allow is removed too; a directory with such a name
 * is left and named, because its subtree would go with it. Every write goes
 * to both copies of the table, and the device is synced when it has a sync.
 *
 * What it refuses to do is guess: a boot sector that disagrees with the
 * mount, two copies of the table that differ (which of them is right is not
 * on the medium), an empty file that still names a chain (it could be a
 * directory the walk has not reached), a directory whose head cluster
 * another chain holds (the subtree below it would be lost), and a medium
 * that does not answer. Those stay in `errors`, `unfixed` counts them, and
 * `first` names the first one.
 * A tree that could not be walked whole (a shared directory head, a depth
 * beyond a path) also keeps the pass from freeing the lost clusters: a
 * cluster of a subtree it did not enter looks exactly like a lost one.
 *
 * After the repair pass the check runs again with a map of its own, so a
 * volume the repair made whole reports 0 errors, and the counters files,
 * directories and free clusters describe the volume as it is now.
 */
int fat_repair(const char *what, struct fat_check_report *report, bool write);

#endif /* MICRO_OS_FS_FAT32_FAT_H */
