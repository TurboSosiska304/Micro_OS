/*
 * Micro OS — the adapter between the filesystem and the VFS.
 */
#ifndef MICRO_OS_MFS_MFS_VFS_H
#define MICRO_OS_MFS_MFS_VFS_H

#include "kernel/types.h"

/* The number of the entries that the list operation returns at most. */
#define MFS_MAX_ENTRIES 64

/* Mount the filesystem at the given point of the tree. */
int mfs_vfs_register(const char *point);

/* The place for the data of an open file. */
void *mfs_vfs_file_data(k_size_t index);

#endif /* MICRO_OS_MFS_MFS_VFS_H */
