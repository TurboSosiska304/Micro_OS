/*
 * Micro OS — the directory of the `progs` partition (ADR 0022).
 *
 * The programs of the board are files of a partition of the flash, not blobs
 * of the firmware (ADR 0018 §2 asked for the move and 1.1 does it). The
 * partition is small and never written by the board, so the simplest format
 * that can be read without an allocation is a fixed header and a table of
 * entries:
 *
 *     end - 0x1000  magic     8 bytes, "MOPROGS1"
 *     end - 0x0FF8  version   u32, 1
 *     end - 0x0FF4  count     u32, at most PROGS_MAX_FILES
 *     end - 0x0FF0  slot      u32, the size of one slot (0x10000)
 *     end - 0x0FEC  reserved  u32, 0
 *     end - 0x0FE8  entries   PROGS_MAX_FILES records of 36 bytes:
 *                               name[24], offset u32, size u32, reserved u32
 *
 * The directory is the last 4 KiB of the partition, and the files are the
 * two 64 KiB slots before it: offset 0 is the first program and 0x10000 the
 * second. The directory is at the end for two reasons: every file
 * is then on a 64 KiB boundary of the flash, which is what
 * `esp_partition_mmap` maps, and the first page of the partition carries a
 * program and nothing else. With the directory in the first slot, a 64 KiB
 * partition would hold no program at all and the smallest one with a program
 * would waste 60 KiB. A later program of 1.x is a new image of the partition,
 * not a reserve slot.
 *
 * The loader of 1.1 copies the bytes into the windows of the port and does
 * not map them (ADR 0022 says why), but the alignment is the one a mapping
 * needs and the table does not have to change for a later loader.
 *
 * The parser is here and not in the VFS file so that the host tests can read
 * a table without a board: it has no ESP-IDF in it and no allocation. A
 * partition whose table does not parse is not a reason to stop the boot; the
 * port mounts an empty /bin and says so, and the shell still works.
 */
#ifndef MICRO_OS_PROGS_FORMAT_H
#define MICRO_OS_PROGS_FORMAT_H

#include "kernel/types.h"

/* The magic and the version of the table. */
#define PROGS_MAGIC "MOPROGS1"
#define PROGS_MAGIC_SIZE 8u
#define PROGS_VERSION 1u

/* The directory at the end of the partition and the slot of a file. */
#define PROGS_DIRECTORY_SIZE 0x1000u
#define PROGS_SLOT_SIZE 0x10000u

/*
 * The name of a file, without its terminating zero, and the records of one
 * partition: 0x21000 bytes are two 64 KiB slots and the 4 KiB directory.
 */
#define PROGS_NAME_MAX 23u
#define PROGS_MAX_FILES 2u
#define PROGS_ENTRY_SIZE 36u
#define PROGS_TABLE_HEADER_SIZE 24u
#define PROGS_PARTITION_SIZE 0x21000u

/* One record of the table, as the parser hands it out. */
struct progs_entry {
    char name[PROGS_NAME_MAX + 1u];
    k_u32_t offset; /* from the start of the partition, a multiple of the slot */
    k_u32_t size;   /* bytes of the file */
};

/*
 * Read the table at `data` (`size` bytes of the last 4 KiB of the partition)
 * and check every record against `partition_size`.
 *
 * Returns 0 when the table is consistent and fills `out` up to `max` entries
 * (`count` receives the number of them, also when the table is refused it is
 * set to zero). The refusals are a wrong magic or version, a count above
 * PROGS_MAX_FILES, a slot that is not a multiple of PROGS_SLOT_SIZE, an
 * offset that is not on a slot boundary, a file that does not fit the area
 * before the directory (or runs past the partition), a name that is empty,
 * too long or carries a character a path cannot, and a duplicate name.
 *
 * The bytes of the file are not read: the checksum of a program is the
 * checksum of the partition that the flash writes, and ADR 0020 keeps the
 * signature of the system image for the image of the system.
 */
int progs_directory_parse(const k_u8_t *data, k_size_t size, k_u32_t partition_size,
                          struct progs_entry *out, k_size_t max, k_size_t *count);

#endif /* MICRO_OS_PROGS_FORMAT_H */
