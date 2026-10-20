#include "kernel/kmod.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"

static struct kmod g_modules[KMOD_MAX];
static k_size_t g_count;
static int g_problems;

static void kmod_set_info(struct kmod *mod, const char *text)
{
    if (text == NULL) {
        mod->info[0] = '\0';
        return;
    }

    strncpy(mod->info, text, KMOD_INFO_MAX - 1);
    mod->info[KMOD_INFO_MAX - 1] = '\0';
}

int kmod_register(const struct kmod *mod)
{
    struct kmod *slot;

    if (mod == NULL || mod->name == NULL) {
        return -1;
    }

    if (g_count >= KMOD_MAX) {
        kerror("kmod: the registry is full (%u modules)", (unsigned)KMOD_MAX);
        return -1;
    }

    slot = &g_modules[g_count];
    memset(slot, 0, sizeof(*slot));

    slot->name = mod->name;
    slot->desc = mod->desc;
    slot->init = mod->init;
    slot->check = mod->check;
    slot->detail = mod->detail;
    slot->state = KMOD_UNKNOWN;

    g_count++;

    if (mod->init != NULL) {
        int rc = mod->init();

        if (rc == 0) {
            slot->state = KMOD_OK;
        } else {
            slot->state = KMOD_FAIL;
            kerror("kmod: '%s' failed to initialise (rc=%d)", mod->name, rc);
        }
    }

    return 0;
}

int kmod_doctor(void)
{
    k_size_t i;

    g_problems = 0;

    for (i = 0; i < g_count; i++) {
        struct kmod *mod = &g_modules[i];

        if (mod->check == NULL) {
            if (mod->state == KMOD_UNKNOWN) {
                mod->state = KMOD_SKIP;
            }
            kmod_set_info(mod, NULL);
            continue;
        }

        if (mod->state == KMOD_FAIL) {
            /* The module did not even initialise, there is nothing to check. */
            g_problems++;
            kmod_set_info(mod, "init failed");
            continue;
        }

        {
            int rc = mod->check();

            if (rc == 0) {
                mod->state = KMOD_OK;
            } else if (rc == 1) {
                mod->state = KMOD_DEGRADED;
            } else {
                mod->state = KMOD_FAIL;
                g_problems++;
            }
        }

        if (mod->detail != NULL) {
            kmod_set_info(mod, mod->detail());
        } else {
            kmod_set_info(mod, NULL);
        }
    }

    return g_problems;
}

k_size_t kmod_count(void)
{
    return g_count;
}

const struct kmod *kmod_get(k_size_t index)
{
    if (index >= g_count) {
        return NULL;
    }

    return &g_modules[index];
}

const char *kmod_state_name(kmod_state_t state)
{
    switch (state) {
    case KMOD_OK:
        return "ok";
    case KMOD_DEGRADED:
        return "degraded";
    case KMOD_FAIL:
        return "fail";
    case KMOD_SKIP:
        return "skip";
    case KMOD_UNKNOWN:
    default:
        return "?";
    }
}

void kmod_report(void)
{
    k_size_t i;

    klog(K_LOG_LEVEL_INFO, "module health report:");
    klog(K_LOG_LEVEL_INFO, "  %-10s %-9s %s", "module", "state", "detail");

    for (i = 0; i < g_count; i++) {
        const struct kmod *mod = &g_modules[i];

        klog(K_LOG_LEVEL_INFO, "  %-10s %-9s %s", mod->name, kmod_state_name(mod->state),
             mod->info[0] != '\0' ? mod->info : mod->desc);
    }

    klog(K_LOG_LEVEL_INFO, "%u module(s), %d problem(s)", (unsigned)g_count, g_problems);
}
