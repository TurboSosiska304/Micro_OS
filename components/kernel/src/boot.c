#include "kernel/boot.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/version.h"

#define KERNEL_MAX_SUBSYS 32

static k_size_t g_booted_subsys;

static int sched_subsys_init(void)
{
    k_sched_init();
    return 0;
}

/*
 * Subsystems the kernel itself needs, initialised before anything the
 * platform provides.
 */
static const struct kernel_subsys g_builtin_subsystems[] = {
    {"sched", sched_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
};

static void print_banner(const struct kernel_boot_config *cfg)
{
    klog(K_LOG_LEVEL_INFO, "%s %s (build %s)", kernel_name(), kernel_version(), kernel_build_id());
    klog(K_LOG_LEVEL_INFO, "board: %s — %s", cfg->board ? cfg->board : KERNEL_BOARD_NAME,
         cfg->board_desc ? cfg->board_desc : KERNEL_BOARD_DESC);
    klog(K_LOG_LEVEL_INFO, "arch: %s, compiler: %s", kernel_arch(), kernel_compiler());
}

/*
 * Stable insertion sort by level: subsystems with the same level keep the
 * order in which they were declared. The list is sorted in place.
 */
static void sort_subsys_list(const struct kernel_subsys **list, k_size_t n)
{
    k_size_t i;

    for (i = 0; i < n; i++) {
        const struct kernel_subsys *item = list[i];
        k_size_t j = i;

        while (j > 0 && list[j - 1]->level > item->level) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = item;
    }
}

int kernel_boot(const struct kernel_boot_config *cfg)
{
    const struct kernel_subsys *sorted[KERNEL_MAX_SUBSYS];
    k_size_t n = 0;
    k_size_t i;

    if (cfg == NULL) {
        KERNEL_PANIC("kernel_boot called with NULL config");
    }

    klog_init(KERNEL_LOG_DEFAULT_LEVEL);
    g_booted_subsys = 0;

    print_banner(cfg);

    /* The built-in subsystems come first, they have the lowest level. */
    for (i = 0; i < K_ARRAY_SIZE(g_builtin_subsystems) && n < KERNEL_MAX_SUBSYS; i++) {
        sorted[n++] = &g_builtin_subsystems[i];
    }

    /* Then the subsystems provided by the platform. */
    if (cfg->subsystems != NULL && cfg->nsubsystems > 0) {
        k_size_t count = cfg->nsubsystems;

        if (count > KERNEL_MAX_SUBSYS - n) {
            count = KERNEL_MAX_SUBSYS - n;
        }
        for (i = 0; i < count; i++) {
            sorted[n++] = &cfg->subsystems[i];
        }
    }

    sort_subsys_list(sorted, n);

    for (i = 0; i < n; i++) {
        int rc = 0;

        if (sorted[i]->init != NULL) {
            rc = sorted[i]->init();
        }

        if (rc != 0) {
            klog(K_LOG_LEVEL_ERROR, "subsystem '%s' failed to init (rc=%d)", sorted[i]->name, rc);
            KERNEL_PANIC("cannot continue without subsystem '%s'", sorted[i]->name);
        }

        klog(K_LOG_LEVEL_INFO, "[ ok ] %s", sorted[i]->name);
        g_booted_subsys++;
    }

    klog(K_LOG_LEVEL_INFO, "%u subsystem(s) ready", (unsigned)g_booted_subsys);

    if (cfg->on_ready != NULL) {
        cfg->on_ready();
    }

    return 0;
}

k_size_t kernel_boot_subsys_count(void)
{
    return g_booted_subsys;
}
