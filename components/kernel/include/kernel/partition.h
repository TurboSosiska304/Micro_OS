/*
 * Micro OS — the tables of the partitions of a disk.
 *
 * A disk is a block device (kernel/blockdev.h), and a partition on it is a
 * block device too: the parser reads the table, holds what it says against
 * the size of the disk and registers a child device for every partition that
 * fits. A child is the same struct k_blockdev with the first sector of the
 * partition added to every request, so a filesystem mounted on "vd0p1" has no
 * idea that it lives inside a disk rather than on one.
 *
 * Two tables are understood: the MBR (four primary entries) and the GPT (a
 * header, an array of entries and the CRC of both). The disk is only read: a
 * table that contradicts itself is a refusal, not a repair, and the bytes of
 * the disk are the same after a scan as they were before it.
 */
#ifndef MICRO_OS_KERNEL_PARTITION_H
#define MICRO_OS_KERNEL_PARTITION_H

#include "kernel/blockdev.h"
#include "kernel/types.h"

/* The most partitions one disk adds to the table of the kernel. */
#define K_PARTITION_MAX 4

/* The room for a name: the name of the disk, "p" and a number. */
#define K_PARTITION_NAME_MAX 16

/*
 * Read the table of `disk` and register a child for every partition in it.
 *
 * The children take the sector size of their parent and the name of the disk
 * with "p" and the number of the entry appended ("vd0p1", "mmcblk0p3"). A
 * scan of a disk that already has children replaces them, so a table that
 * was read once is not in the registry twice. The calls do not write to the
 * medium.
 *
 * Returns the number of the children registered (at least one), or a KERR_*
 * code:
 *   KERR_NOENT  the disk has no table this kernel reads;
 *   KERR_INVAL  the table contradicts itself or this layer (a partition
 *               outside the disk, two that overlap, an offset the sectors of
 *               the device cannot address, a checksum that does not match);
 *   KERR_NOSPC  the table has more partitions than the kernel keeps;
 *   KERR_IO     the disk did not answer.
 *
 * A scan happens at the boot, when the port knows its disk and no task is
 * running yet: the slots of the children are not locked.
 */
int partition_scan(const struct k_blockdev *disk);

/*
 * The name of the `index`-th child of `disk`, in the order the scan
 * registered them; NULL when the disk has no such child. The name lives as
 * long as the child does.
 */
const char *partition_name(const struct k_blockdev *disk, k_size_t index);

/*
 * Forget the children of `disk`: the devices leave the table of the kernel
 * and their slots are free for another scan. The caller guarantees that no
 * filesystem is mounted on them and that no task has a file of them open.
 */
void partition_release(const struct k_blockdev *disk);

#endif /* MICRO_OS_KERNEL_PARTITION_H */
