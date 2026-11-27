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
 * The inventory of a GPT disk: the entries the table names and the gaps
 * between them. The installer reads it before it writes, and it never writes
 * from a guess - the gap a person confirms is the gap this inventory found.
 */
#define PARTITION_INVENTORY_MAX 128
#define PARTITION_GAPS_MAX 16
#define PARTITION_LABEL_MAX 36

/* A 1 MiB boundary in 512-byte LBAs: the start of a partition the installer
 * makes is aligned up to it, so the partition sits where the tools of a
 * person expect it. */
#define PARTITION_ALIGN_LBA 2048u

/* Flags of a partition entry the inventory reports. */
#define PARTITION_FLAG_ESP 0x01u        /* an EFI System Partition */
#define PARTITION_FLAG_FOREIGN_OS 0x02u /* a volume of another system */
#define PARTITION_FLAG_OURS 0x04u       /* a volume of this system */

/* The length of a GPT type GUID on the wire. */
#define PARTITION_GUID_SIZE 16u

/* One entry of the table, as the inventory reports it. */
struct partition_entry_info {
    k_u32_t index; /* 1-based number in the table */
    k_u64_t first_lba;
    k_u64_t last_lba;
    k_u64_t sectors;
    k_u8_t type_guid[PARTITION_GUID_SIZE]; /* raw GPT type; zero for MBR */
    k_u8_t type_mbr;                       /* MBR type byte; 0 for GPT */
    char label[PARTITION_LABEL_MAX + 1];   /* the name of the entry, UTF-8 */
    k_u32_t flags;                         /* PARTITION_FLAG_* */
};

/* A free, contiguous run of LBAs between two entries (or the usable edges of
 * the disk). `first_lba` is aligned up to PARTITION_ALIGN_LBA; `sectors` is
 * what remains after that alignment, so a gap that is too small after the
 * alignment has sectors == 0. */
struct partition_gap {
    k_u64_t first_lba;     /* aligned up to 1 MiB */
    k_u64_t last_lba;      /* inclusive, the last LBA of the gap */
    k_u64_t sectors;       /* after alignment */
    k_u64_t raw_first_lba; /* before alignment */
    k_u64_t raw_sectors;   /* before alignment */
};

/* The inventory of one disk: the entries, the gaps, and the facts the
 * installer needs to refuse a dangerous case before it writes. */
struct partition_inventory {
    bool gpt;
    bool has_esp;
    bool hybrid_mbr;      /* protective MBR carries a non-protective entry */
    bool backup_ok;       /* the backup GPT header and array are valid */
    k_u32_t sector_size;  /* bytes per device sector */
    k_u64_t disk_sectors; /* device sectors */
    k_u64_t disk_lbas;    /* 512-byte LBAs */
    k_u64_t first_usable; /* first usable LBA (GPT) */
    k_u64_t last_usable;  /* last usable LBA (GPT) */
    k_u32_t entry_count;
    struct partition_entry_info entries[PARTITION_INVENTORY_MAX];
    k_u32_t gap_count;
    struct partition_gap gaps[PARTITION_GAPS_MAX];
};

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
 * The type byte of the table entry a partition came from - the byte of the
 * MBR that says what a person meant the volume to be (0x83 Linux, 0x0B/0x0C
 * FAT32), which the boot of the root reads as a hint (ADR 0027).
 *
 * Returns -1 when the device is not a partition of this kernel (the disk
 * itself, a volume of the memory) and when the table has no such byte at all
 * (a GPT entry). A hint that is not there costs nothing: the magic of the
 * superblock is the answer either way.
 */
int partition_type_of(const struct k_blockdev *part);

/*
 * Forget the children of `disk`: the devices leave the table of the kernel
 * and their slots are free for another scan. The caller guarantees that no
 * filesystem is mounted on them and that no task has a file of them open.
 */
void partition_release(const struct k_blockdev *disk);

/*
 * Read the partition table of `disk` and report every entry and every gap.
 *
 * The inventory checks both GPT copies and the protective MBR. A table whose
 * primary CRC is broken, whose backup header or array is missing or broken,
 * or whose protective MBR carries a non-protective entry is refused with
 * KERR_IO: the installer never writes from a table it does not trust.
 *
 * Gaps are the contiguous runs of LBAs no entry claims, with `first_lba`
 * aligned up to PARTITION_ALIGN_LBA. A disk with no GPT this kernel reads is
 * KERR_NOENT; a damaged or inconsistent table is KERR_IO or KERR_INVAL.
 */
int partition_inventory(const struct k_blockdev *disk, struct partition_inventory *out);

/*
 * Append one GPT entry to both copies of the table.
 *
 * The entry covers [start_lba, start_lba + sector_count). Before any write
 * the function saves both tables in memory; after the write it re-reads and
 * verifies that the new entry is present in both copies and that every other
 * entry is byte-identical to the saved table. On a mismatch, or on a write
 * error after the first copy was touched, it restores the saved copies and
 * returns an error - the disk is left as it was found, or the restore is
 * attempted and the error is reported.
 *
 * The call refuses: a start that is not aligned to PARTITION_ALIGN_LBA, a
 * range that is too small or outside the usable area, a range that intersects
 * an existing entry, a disk whose GPT is damaged or has no valid backup, and
 * a hybrid/protective MBR that carries a foreign entry. No existing entry is
 * moved, resized, renamed or deleted; the protective MBR is not written.
 */
int partition_append(const struct k_blockdev *disk, k_u64_t start_lba, k_u64_t sector_count,
                     const k_u8_t type_guid[PARTITION_GUID_SIZE], const char *label);

#endif /* MICRO_OS_KERNEL_PARTITION_H */
