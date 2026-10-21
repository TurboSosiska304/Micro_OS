/*
 * Micro OS — the MicroFS on the partition of the flash.
 */
#ifndef MICRO_OS_MFS_MFS_FLASH_H
#define MICRO_OS_MFS_MFS_FLASH_H

/*
 * Mount the filesystem of the flash at the given point of the tree. The
 * volume is formatted when it is empty.
 */
int mfs_flash_mount(const char *point);

#endif /* MICRO_OS_MFS_MFS_FLASH_H */
