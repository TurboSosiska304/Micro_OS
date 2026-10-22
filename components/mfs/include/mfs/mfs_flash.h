/*
 * Micro OS — the MicroFS on the partition of the flash.
 */
#ifndef MICRO_OS_MFS_MFS_FLASH_H
#define MICRO_OS_MFS_MFS_FLASH_H

/*
 * Mount the filesystem of the flash at the given point of the tree. The
 * volume is formatted when the flash was never written; any other error
 * keeps the data of the user.
 */
int mfs_flash_mount(const char *point);

/*
 * Erase the whole volume and make a new filesystem on it. The volume must
 * not be mounted: unmount it first.
 */
int mfs_flash_format(void);

#endif /* MICRO_OS_MFS_MFS_FLASH_H */
