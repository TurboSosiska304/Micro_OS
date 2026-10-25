/*
 * Micro OS — module registry and health check.
 *
 * Every driver registers a module descriptor. The registry runs the
 * initialisation of the module and, later, a health check ("doctor") that
 * tells whether the module actually works. The init process walks through
 * the modules and prints a report, so a broken piece of hardware is
 * visible right after the boot.
 */
#ifndef MICRO_OS_KERNEL_KMOD_H
#define MICRO_OS_KERNEL_KMOD_H

#include "kernel/types.h"

#define KMOD_MAX 24
#define KMOD_INFO_MAX 48

typedef enum {
    KMOD_UNKNOWN = 0,
    KMOD_OK,       /* everything works */
    KMOD_DEGRADED, /* works, but with limitations */
    KMOD_FAIL,     /* does not work */
    KMOD_SKIP,     /* not implemented or not present */
} kmod_state_t;

struct kmod {
    const char *name;            /* short identifier, e.g. "display" */
    const char *desc;            /* human readable description */
    int (*init)(void);           /* hardware initialisation, called on registration */
    int (*check)(void);          /* health check, called by the doctor */
    const char *(*detail)(void); /* optional detail string of the last check */
    kmod_state_t state;
    char info[KMOD_INFO_MAX];
};

/*
 * Register a module and initialise it. Returns 0 on success.
 * A module whose init fails is registered anyway, in the KMOD_FAIL state,
 * so that the doctor can report it.
 */
int kmod_register(const struct kmod *mod);

/* Run the health check of every registered module. Returns the number of problems. */
int kmod_doctor(void);

/* Access to the registry. */
k_size_t kmod_count(void);
const struct kmod *kmod_get(k_size_t index);
const char *kmod_state_name(kmod_state_t state);

/* The problems of the last run of the doctor (0 before the first run). */
int kmod_problems(void);

/* Print the report of the last doctor run. */
void kmod_report(void);

#endif /* MICRO_OS_KERNEL_KMOD_H */
