/*
 * Micro OS — the hang watchdog.
 *
 * The dump is a diagnostic, not a log line: it must come out when the system
 * is already broken, so it takes no lock, reads state best-effort, and writes
 * through k_port_write() directly. One dump per incident (latch) keeps a
 * spinning tick from flooding the only channel the operator has left.
 */
#include "kernel/watchdog.h"

#include <stdarg.h>
#include <stdio.h>

#include "kernel/cpu.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/cpu.h"
#include "kernel/sched.h"
#include "kernel/task.h"

#define WDG_LINE 256

static volatile bool g_triggered;
static volatile bool g_dumping;
static k_u32_t g_last_check_ms;

static void wdg_write(const char *text)
{
    k_size_t len = 0;

    if (text == NULL) {
        return;
    }

    while (text[len] != '\0') {
        len++;
    }

    k_port_write(text, len);
}

static void wdg_printf(const char *fmt, ...) K_FORMAT(__printf__, 1, 2);

static void wdg_printf(const char *fmt, ...)
{
    char buf[WDG_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }

    if ((k_size_t)n >= sizeof(buf)) {
        buf[sizeof(buf) - 1] = '\0';
    }

    wdg_write(buf);
}

static const char *state_name(k_u8_t state)
{
    return task_state_name((task_state_t)state);
}

/* Snapshot comes from kernel/lock.h (lock-free, best effort). */

void k_watchdog_init(void)
{
    g_triggered = false;
    g_dumping = false;
    g_last_check_ms = 0u;
}

bool k_watchdog_triggered(void)
{
    return g_triggered;
}

void k_watchdog_reset(void)
{
    __atomic_store_n(&g_triggered, false, __ATOMIC_RELEASE);
}

static void dump_mutexes(void)
{
    struct k_mutex_debug_info infos[32];
    k_size_t count;
    k_size_t i;
    k_size_t held = 0;

    count = k_mutex_debug_snapshot(infos, K_ARRAY_SIZE(infos));

    for (i = 0; i < count; i++) {
        if (infos[i].depth > 0u) {
            held++;
        }
    }

    wdg_printf("[WDG] mutexes: %u known, %u held\n", (unsigned)count, (unsigned)held);

    for (i = 0; i < count; i++) {
        if (infos[i].depth == 0u) {
            continue;
        }
        wdg_printf("[WDG]   mutex %s owner=%u depth=%u\n",
                   infos[i].name != NULL ? infos[i].name : "?", (unsigned)infos[i].owner_pid,
                   (unsigned)infos[i].depth);
    }
}

static void dump_sleepers(k_u32_t now_ticks)
{
    struct k_sched_sleeper sleepers[TASK_MAX_TASKS];
    k_size_t count;
    k_size_t i;

    count = k_sched_sleepers_raw(sleepers, K_ARRAY_SIZE(sleepers));
    wdg_printf("[WDG] sleepers: %u\n", (unsigned)count);

    for (i = 0; i < count; i++) {
        k_u32_t wake = sleepers[i].wake_tick;
        k_u32_t overdue = (k_i32_t)(now_ticks - wake) >= 0 ? (now_ticks - wake) : 0u;

        wdg_printf("[WDG]   pid=%u cpu=%u state=%s wake=%u now=%u overdue=%u ms\n",
                   (unsigned)sleepers[i].pid, (unsigned)sleepers[i].cpu,
                   state_name(sleepers[i].state), (unsigned)wake, (unsigned)now_ticks,
                   (unsigned)overdue);
    }
}

static void dump_cpus(k_u32_t now_ticks)
{
    k_u32_t cpus = k_cpu_count();
    k_u32_t cpu;

    wdg_printf("[WDG] cpus: %u ticks=%u tsc=%llu uptime_us=%llu\n", (unsigned)cpus,
               (unsigned)now_ticks, (unsigned long long)k_port_debug_tsc(),
               (unsigned long long)k_port_uptime_us());

    for (cpu = 0; cpu < cpus && cpu < KERNEL_CPUS_MAX; cpu++) {
        struct k_cpu *record = k_cpu_by_id(cpu);
        struct task *cur = (record != NULL) ? record->current : NULL;
        k_u32_t depth = k_port_cpu_critical_depth(cpu);
        k_u64_t rip = k_port_cpu_saved_rip(cpu);

        wdg_printf("[WDG]   cpu=%u ticks=%u crit=%u klock=%u rip=0x%016llx cur=%u:%s/%s\n",
                   (unsigned)cpu, (record != NULL) ? (unsigned)record->ticks : 0u, (unsigned)depth,
                   (record != NULL && record->interrupt_depth > 0u) ? 1u : 0u,
                   (unsigned long long)rip, (cur != NULL) ? (unsigned)cur->pid : 0u,
                   (cur != NULL) ? cur->name : "<idle>",
                   (cur != NULL) ? task_state_name(cur->state) : "-");
    }
}

void k_watchdog_trigger(const char *reason)
{
    if (__atomic_exchange_n(&g_triggered, true, __ATOMIC_ACQ_REL)) {
        return;
    }

    /* Best-effort single dump: if another core is already dumping, skip. */
    if (__atomic_exchange_n(&g_dumping, true, __ATOMIC_ACQ_REL)) {
        return;
    }

    wdg_write("\n[WDG] === HANG WATCHDOG ===\n");
    wdg_printf("[WDG] reason: %s\n", reason != NULL ? reason : "unknown");
    dump_cpus(k_sched_ticks_raw());
    dump_sleepers(k_sched_ticks_raw());
    dump_mutexes();
    wdg_write("[WDG] === END ===\n");

    __atomic_store_n(&g_dumping, false, __ATOMIC_RELEASE);
}

void k_watchdog_check(void)
{
    k_u64_t now_us;
    k_u32_t now_ms;
    k_u32_t now_ticks;
    k_size_t i;

    if (g_triggered) {
        return;
    }

    now_us = k_port_uptime_us();
    now_ms = (k_u32_t)(now_us / 1000u);

    /* Check at most once per second. */
    if ((k_u32_t)(now_ms - g_last_check_ms) < 1000u) {
        return;
    }
    g_last_check_ms = now_ms;

    now_ticks = k_sched_ticks_raw();

    /* Overdue sleepers: a sleeper more than threshold past its deadline means
     * the scheduler stopped making progress. */
    {
        struct k_sched_sleeper sleepers[TASK_MAX_TASKS];
        k_size_t count = k_sched_sleepers_raw(sleepers, K_ARRAY_SIZE(sleepers));

        for (i = 0; i < count; i++) {
            k_u32_t overdue = now_ticks - sleepers[i].wake_tick;
            if ((k_i32_t)overdue >= (k_i32_t)K_WATCHDOG_THRESHOLD_MS) {
                wdg_printf("[WDG] overdue sleeper pid=%u overdue=%u ms\n",
                           (unsigned)sleepers[i].pid, (unsigned)overdue);
                k_watchdog_trigger("sleeper overdue");
                return;
            }
        }
    }

    /* Ready tasks that have not run for the threshold. */
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index_unlocked(i);

        if (t == NULL || !t->in_use) {
            continue;
        }

        if (t->state == TASK_STATE_READY) {
            k_u32_t waited = now_ticks - t->last_run_tick;
            if (t->last_run_tick != 0u && (k_i32_t)waited >= (k_i32_t)K_WATCHDOG_THRESHOLD_MS) {
                wdg_printf("[WDG] starved pid=%u name=%s waited=%u ms\n", (unsigned)t->pid, t->name,
                           (unsigned)waited);
                k_watchdog_trigger("ready task starved");
                return;
            }
        }
    }
}
