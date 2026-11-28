/*
 * Micro OS — the record and the tree of an installed volume.
 *
 * The installer formats a partition and then writes the small tree a boot
 * expects: `/etc/volumes` names the installed volume, `/etc/install` records
 * what was installed, and `/dev`, `/proc`, `/tmp` and `/mnt` are the empty
 * mount points the boot fills. The same calls are used by the host tests, so
 * the tree a test checks is the tree the installer writes.
 */
#ifndef MICRO_OS_KERNEL_INSTALL_H
#define MICRO_OS_KERNEL_INSTALL_H

#include "kernel/types.h"

#define INSTALL_RECORD_PATH "/etc/install"
#define INSTALL_VOLUMES_PATH "/etc/volumes"

/* The fields of the record, in the order a person reads them. */
struct install_record {
    const char *volume;    /* the partition as the live system names it */
    const char *fs;        /* "ext2" or "mfs" */
    const char *boot_mode; /* "uefi" in this release */
    k_u64_t start_lba;     /* the place of the partition in the table */
    k_u64_t sector_count;  /* its length, for the deterministic UUID */
};

/*
 * Create the tree under `point`, which has to be the mount point of the
 * freshly formatted volume. Existing directories are kept; a file that is
 * already there is overwritten, because the volume was just formatted and
 * there is no user data on it to protect.
 *
 * Returns 0, or a KERR_* code from the VFS.
 */
int install_tree_create(const char *point, const struct install_record *rec);

/*
 * Format `unix_seconds` as "YYYY-MM-DD" (UTC) in `out`. A clock that was not
 * set gives a small number, which is printed as the first days of 1970 - an
 * honest date of the machine rather than a guess about a battery.
 */
void install_date(char *out, k_size_t size, k_u64_t unix_seconds);

#endif /* MICRO_OS_KERNEL_INSTALL_H */
