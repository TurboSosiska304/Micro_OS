/*
 * Micro OS — the wall clock of the system.
 *
 * One function pointer, set when the driver of the chip comes up during
 * the boot, read long after the tasks are running. The registration and
 * the reads never overlap in time, so no lock is needed; the pointer is
 * written once and only read afterwards.
 */
#include "kernel/clock.h"

static k_clock_set_fn g_set;

void k_clock_register(k_clock_set_fn set)
{
    g_set = set;
}

int k_clock_set_unix(k_u64_t unix_seconds)
{
    if (g_set == NULL) {
        return -1;
    }

    return g_set(unix_seconds);
}

bool k_clock_present(void)
{
    return g_set != NULL;
}
