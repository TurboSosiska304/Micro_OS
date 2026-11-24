/*
 * Micro OS — the wall clock of the system.
 *
 * One function pointer, set when the driver of the chip comes up during
 * the boot, read long after the tasks are running. The pointer is written
 * once at the boot and only read afterwards.
 *
 * The moment of a file, however, needs a number even when nobody ever set
 * the clock: the clock therefore keeps the moment of the last set and the
 * uptime of that moment: the two give a wall clock that keeps running, and a
 * machine whose chip is dead and whose network is down falls back to the
 * seconds since the boot. The base is behind a mutex because NTP may move it
 * while a task stamps a record.
 *
 * Who sets it: the source of the machine at the boot (the BM8563 of the board,
 * the CMOS of a laptop, the process clock of the host), and then `ntp` or
 * `date set`, which write the same number back into the chip through the
 * setter the driver registered.
 */
#include "kernel/clock.h"

#include "kernel/lock.h"
#include "kernel/port/time.h"

static k_clock_set_fn g_set;
static k_mutex_t s_lock = K_MUTEX_INITIALIZER;
static k_u64_t s_base_unix;
static k_u64_t s_base_uptime_ms;
static bool s_base_valid;

void k_clock_register(k_clock_set_fn set)
{
    g_set = set;

    if (set == NULL) {
        /*
         * The clock is gone: the next moment is the one of the boot again,
         * not the stale base of a chip that is no longer there.
         */
        (void)k_mutex_lock(&s_lock);
        s_base_valid = false;
        (void)k_mutex_unlock(&s_lock);
    }
}

int k_clock_set_unix(k_u64_t unix_seconds)
{
    k_u64_t uptime_ms;
    int rc;

    if (g_set == NULL) {
        return -1;
    }

    rc = g_set(unix_seconds);
    if (rc != 0) {
        return -1;
    }

    uptime_ms = k_port_uptime_ms();

    (void)k_mutex_lock(&s_lock);
    s_base_unix = unix_seconds;
    s_base_uptime_ms = uptime_ms;
    s_base_valid = true;
    (void)k_mutex_unlock(&s_lock);

    return 0;
}

void k_clock_now(struct k_clock_moment *out)
{
    k_u64_t uptime_ms = k_port_uptime_ms();
    k_u64_t base;
    k_u64_t base_uptime;
    bool valid;

    if (out == NULL) {
        return;
    }

    if (k_mutex_lock(&s_lock) != 0) {
        /*
         * No lock to read the base under: the boot time is still a clock,
         * and its fraction is the fraction of the millisecond count.
         */
        out->sec = uptime_ms / 1000u;
        out->nsec = (k_u32_t)((uptime_ms % 1000u) * 1000000u);
        return;
    }

    valid = s_base_valid;
    base = s_base_unix;
    base_uptime = s_base_uptime_ms;

    (void)k_mutex_unlock(&s_lock);

    if (!valid) {
        out->sec = uptime_ms / 1000u;
        out->nsec = (k_u32_t)((uptime_ms % 1000u) * 1000000u);
        return;
    }

    if (uptime_ms < base_uptime) {
        /* The counter of the port went back; the base is the best answer. */
        out->sec = base;
        out->nsec = 0;
        return;
    }

    out->sec = base + (uptime_ms - base_uptime) / 1000u;
    out->nsec = (k_u32_t)(((uptime_ms - base_uptime) % 1000u) * 1000000u);
}

k_u64_t k_clock_now_unix(void)
{
    struct k_clock_moment moment;

    k_clock_now(&moment);

    return moment.sec;
}

bool k_clock_present(void)
{
    return g_set != NULL;
}
