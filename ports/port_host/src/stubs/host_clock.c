/*
 * Micro OS — the wall clock of the host port.
 *
 * The board keeps the time in the BM8563; the host keeps it in the clock of
 * the process. Both must be settable through kernel/clock.h: `date set` and
 * `ntp` write the moment, and nobody above the port should know which chip
 * (or which process) holds it.
 *
 * The offset is not the time itself: the process clock keeps running while
 * the offset changes, so a second read a second later is still a second
 * later. `date set` moves the offset to the requested moment.
 */
#include "host_stubs.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "kernel/clock.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static k_i64_t s_offset_s;

k_i64_t host_clock_now_unix(void)
{
    time_t now = time(NULL);
    k_i64_t offset;

    if (now == (time_t)-1) {
        now = 0;
    }

    pthread_mutex_lock(&s_lock);
    offset = s_offset_s;
    pthread_mutex_unlock(&s_lock);

    return (k_i64_t)now + offset;
}

int host_clock_set_unix(k_u64_t unix_seconds)
{
    time_t now = time(NULL);

    if (now == (time_t)-1) {
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    s_offset_s = (k_i64_t)unix_seconds - (k_i64_t)now;
    pthread_mutex_unlock(&s_lock);

    return 0;
}

/* The moment as one line, in the shape the RTC of the board prints. */
void host_clock_text(char *buf, size_t size)
{
    k_i64_t now = host_clock_now_unix();
    time_t seconds = (time_t)now;
    struct tm moment;

    if (buf == NULL || size == 0) {
        return;
    }

    if (now < 0 || gmtime_r(&seconds, &moment) == NULL) {
        snprintf(buf, size, "1970-01-01 00:00:00");
        return;
    }

    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d", moment.tm_year + 1900, moment.tm_mon + 1,
             moment.tm_mday, moment.tm_hour, moment.tm_min, moment.tm_sec);
}

void host_clock_register(void)
{
    k_clock_register(host_clock_set_unix);
}
