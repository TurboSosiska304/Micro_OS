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

#endif /* MICRO_OS_KERNEL_VERSION_H */
