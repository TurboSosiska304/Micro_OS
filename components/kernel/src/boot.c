#include "kernel/boot.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/port.h"
#include "kernel/version.h"

#define KERNEL_MAX_SUBSYS 32

static k_size_t g_booted_subsys;

static void print_banner(const struct kernel_boot_config *cfg) {
    klog(K_LOG_LEVEL_INFO, "%s %s (build %s)", kernel_name(), kernel_version(), kernel_build_id());
    klog(K_LOG_LEVEL_INFO, "board: %s — %s", cfg->board ? cfg->board : KERNEL_BOARD_NAME,
         cfg->board_desc ? cfg->board_desc : KERNEL_BOARD_DESC);
    klog(K_LOG_LEVEL_INFO, "arch: %s, compiler: %s", kernel_arch(), kernel_compiler());
}

/*
 * Stable insertion sort by level: subsystems with the same level keep the
 * order in which they were declared.
 */
static void sort_subsystems(const struct kernel_subsys *in, k_size_t n,
                            const struct kernel_subsys **out) {
    k_size_t i;

    for (i = 0; i < n; i++) {
        k_size_t j = i;
        while (j > 0 && out[j - 1]->level > in[i].level) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = &in[i];
    }
}

int kernel_boot(const struct kernel_boot_config *cfg) {
    const struct kernel_subsys *sorted[KERNEL_MAX_SUBSYS];
    k_size_t n = 0;
    k_size_t i;

    if (cfg == NULL) {
        KERNEL_PANIC("kernel_boot called with NULL config");
    }

    klog_init(KERNEL_LOG_DEFAULT_LEVEL);
    g_booted_subsys = 0;

    print_banner(cfg);

    if (cfg->subsystems != NULL && cfg->nsubsystems > 0) {
        n = cfg->nsubsystems > KERNEL_MAX_SUBSYS ? KERNEL_MAX_SUBSYS : cfg->nsubsystems;
        sort_subsystems(cfg->subsystems, n, sorted);
    }

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

k_size_t kernel_boot_subsys_count(void) {
    return g_booted_subsys;
}
