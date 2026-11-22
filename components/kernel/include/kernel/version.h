/*
 * Micro OS — kernel version / build identification.
 */
#ifndef MICRO_OS_KERNEL_VERSION_H
#define MICRO_OS_KERNEL_VERSION_H

#include "kernel/types.h"

const char *kernel_name(void);
const char *kernel_version(void);  /* "major.minor.patch" */
const char *kernel_build_id(void); /* build date and time */
const char *kernel_compiler(void);
const char *kernel_arch(void);

/*
 * Compare two versions of the "major.minor.patch[-suffix]" shape: -1 when a
 * is older than b, 0 when they are the same, 1 when a is newer. A release is
 * newer than its candidate ("0.9.0" > "0.9.0-rc1"), and the numbers of a
 * suffix are compared as numbers ("rc10" > "rc2"). The board uses it for the
 * anti-rollback policy (ADR 0020); the number is not a promise that every
 * string is a version.
 */
int kernel_version_compare(const char *a, const char *b);

#endif /* MICRO_OS_KERNEL_VERSION_H */
