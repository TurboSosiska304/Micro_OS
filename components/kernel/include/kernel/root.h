/*
 * Micro OS — the root of the machine: the type of a volume, and its mount.
 *
 * The root of a machine is a volume of some filesystem, and the filesystem is
 * a fact of the medium: the type of the partition in the table is a hint a
 * person may have written by hand, the magic of the superblock is the volume
 * itself, and the types the image was built with are the ones that may claim
 * the bytes (ADR 0027).
 *
 * The port says which device the root is on, not what kind of volume it is
 * (kernel/port.h has no word about a filesystem); this file answers the
 * second question and mounts the root with the answer, so that a machine
 * whose disk was made by `mke2fs` boots to the same shell as one whose disk
 * was made by `tools/mkfs_mfs`.
 *
 * The board is the exception the contract allows: its root is the `progs`
 * partition of the flash and no partition table names it, so a port that has
 * no table keeps mounting MicroFS by itself (ADR 0027 §2).
 */
#ifndef MICRO_OS_KERNEL_ROOT_H
#define MICRO_OS_KERNEL_ROOT_H

#include "kernel/types.h"

/* The point of the root of the tree. */
#define K_ROOT_POINT "/"

/*
 * The type of the volume of `device`, as a name of the registry of the VFS
 * ("mfs", "ext2", "fat32"), or NULL when no type of the image recognises the
 * bytes.
 *
 * The order of the questions is the order of ADR 0027:
 *
 *   1. the type byte of the MBR entry the device came from (0x83 Linux,
 *      0x0B/0x0C FAT32) puts one type first, and only first: the byte is a
 *      hint, and a volume whose magic says otherwise is not that type;
 *   2. the magic of the superblock - MicroFS in the header block of the
 *      volume, the 0xEF53 of ext2 at the second kilobyte, the BPB of FAT32;
 *   3. the registry of the types of the image, in the order they were
 *      registered: the first type whose probe says yes owns the volume.
 *
 * A medium that was never written - all ones, like the flash of a board, or
 * all zeros, like the array of the port - is not a volume of anything and
 * answers "mfs": MicroFS is where the machine formats a fresh medium, and the
 * caller that mounts the answer gets a formatted volume instead of a refusal.
 *
 * The bytes of a volume that no type recognises are in the log, once, with
 * the first four bytes of the medium: a machine that cannot boot says what it
 * found where a volume was expected.
 */
const char *k_root_type(const char *device);

/*
 * Mount the root of the machine at "/" with the type of k_root_type().
 *
 * Returns 0 with the type name in `type` (when `type` is not NULL), or a
 * KERR_* code: KERR_NOENT when the device is not there, KERR_INVAL when no
 * type of the image recognises it, and the error of the mount of the volume
 * when the type was found and the volume still does not mount.
 *
 * The line of the boot - "the root is ext2 on ata0p1" - is written here, so
 * that every port says the same sentence and no port has to know the order of
 * the questions. A caller that needs the type for a log line of its own keeps
 * the answer through `type`.
 */
int k_root_mount(const char *device, const char **type);

#endif /* MICRO_OS_KERNEL_ROOT_H */
