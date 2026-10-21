/*
 * Micro OS — the procfs.
 */
#ifndef MICRO_OS_PROCFS_PROCFS_H
#define MICRO_OS_PROCFS_PROCFS_H

#include "kernel/types.h"

/* Mount the procfs at the given point of the tree. */
int procfs_register(const char *point);

#endif /* MICRO_OS_PROCFS_PROCFS_H */
