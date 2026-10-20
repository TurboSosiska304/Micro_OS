/*
 * Micro OS — boot sequence and subsystem registry.
 *
 * The boot sequence mirrors the Linux model: subsystems are registered with a
 * level and initialised in level order (core -> drivers -> services -> apps).
 * Every subsystem reports success or failure, so a broken driver is visible
 * immediately in the boot log instead of causing mysterious crashes later.
 */
#ifndef MICRO_OS_KERNEL_BOOT_H
#define MICRO_OS_KERNEL_BOOT_H

#include "kernel/types.h"

/* Initialisation levels: lower value == earlier. */
#define KERNEL_SUBSYS_LEVEL_CORE 0
#define KERNEL_SUBSYS_LEVEL_DRIVER 10
#define KERNEL_SUBSYS_LEVEL_SERVICE 20
#define KERNEL_SUBSYS_LEVEL_APP 30

struct kernel_subsys {
    const char *name;  /* short identifier, e.g. "vfs" */
    int (*init)(void); /* returns 0 on success, negative on error */
    int level;         /* KERNEL_SUBSYS_LEVEL_* */
};

struct kernel_boot_config {
    const char *board;      /* board name */
    const char *board_desc; /* human readable board description */
    const struct kernel_subsys *subsystems;
    k_size_t nsubsystems;
    void (*on_ready)(void); /* optional, called after a successful boot */
};

/*
 * Boot the kernel. Returns 0 on success, negative on failure.
 * On failure the kernel panics, so the function never returns with an error.
 */
int kernel_boot(const struct kernel_boot_config *cfg);

/* Return the number of successfully initialised subsystems of the last boot. */
k_size_t kernel_boot_subsys_count(void);

#endif /* MICRO_OS_KERNEL_BOOT_H */
