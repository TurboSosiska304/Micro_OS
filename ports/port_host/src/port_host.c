#include "port_host.h"
#include "port_host_psram.h"

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"

/* --- output sink --- */

static FILE *s_sink;

void port_host_set_sink(FILE *sink)
{
    s_sink = sink;
}

FILE *port_host_get_sink(void)
{
    return s_sink != NULL ? s_sink : stdout;
}

/* --- locking --- */

/*
 * The host port uses a recursive mutex: the kernel core is allowed to call
 * locked helpers from within a locked section.
 */
static pthread_mutex_t s_lock;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static pthread_key_t s_task_key;

static void host_once_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_key_create(&s_task_key, NULL);
}

static void host_lock(void)
{
    pthread_once(&s_once, host_once_init);
    pthread_mutex_lock(&s_lock);
}

static void host_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

void k_port_enter_critical(void)
{
    host_lock();
}

void k_port_exit_critical(void)
{
    host_unlock();
}

/* --- basic port API --- */

void k_port_write(const char *data, k_size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    fwrite(data, 1, len, port_host_get_sink());
    fflush(port_host_get_sink());
}

k_u64_t k_port_uptime_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (k_u64_t)ts.tv_sec * 1000u + (k_u64_t)(ts.tv_nsec / 1000000);
}

void k_port_halt(void)
{
    for (;;) {
        sleep(1);
    }
}

void k_port_reset(void)
{
    exit(0);
}

const char *k_port_name(void)
{
    return "host";
}

const char *k_port_arch(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "host";
#endif
}

/* --- console --- */

int k_port_console_readline(char *buf, k_size_t size)
{
    if (buf == NULL || size == 0) {
        return -1;
    }

    if (fgets(buf, (int)size, stdin) == NULL) {
        return -1;
    }

    {
        k_size_t len = strlen(buf);

        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }

        return (int)len;
    }
}

/* --- memory --- */

k_size_t k_port_heap_free(void)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 info = mallinfo2();

    return (k_size_t)(info.arena - info.uordblks);
#else
    return 0;
#endif
}

k_size_t k_port_heap_total(void)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 info = mallinfo2();

    return (k_size_t)info.arena;
#else
    return 0;
#endif
}

k_size_t k_port_psram_free(void)
{
    return host_psram_available();
}

k_size_t k_port_psram_total(void)
{
    return host_psram_total();
}

void *k_port_alloc(k_size_t size, bool psram)
{
    if (size == 0) {
        size = 4096;
    }

    if (psram) {
        return host_psram_alloc(size);
    }

    return malloc(size);
}

void k_port_free(void *ptr)
{
    if (host_psram_contains(ptr)) {
        host_psram_release(ptr);
        return;
    }

    free(ptr);
}

k_u64_t k_port_uptime_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (k_u64_t)ts.tv_sec * 1000000u + (k_u64_t)(ts.tv_nsec / 1000);
}

/* --- tasks (pthreads) --- */

typedef struct {
    struct task *task;
    k_port_task_entry_fn entry;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started;
} host_task_ctl;

k_size_t k_port_task_stack_size(void)
{
    return 64 * 1024;
}

static void *host_task_thread(void *arg)
{
    host_task_ctl *ctl = (host_task_ctl *)arg;

    pthread_once(&s_once, host_once_init);
    pthread_setspecific(s_task_key, ctl->task);

    pthread_mutex_lock(&ctl->lock);
    while (!ctl->started) {
        pthread_cond_wait(&ctl->cond, &ctl->lock);
    }
    pthread_mutex_unlock(&ctl->lock);

    ctl->entry(ctl->task);

    return NULL;
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg)
{
    host_task_ctl *ctl;
    pthread_attr_t attr;
    pthread_t thread;

    K_UNUSED(arg);

    ctl = calloc(1, sizeof(*ctl));
    if (ctl == NULL) {
        return -1;
    }

    ctl->task = t;
    ctl->entry = entry;
    pthread_mutex_init(&ctl->lock, NULL);
    pthread_cond_init(&ctl->cond, NULL);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, k_port_task_stack_size());
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, host_task_thread, ctl) != 0) {
        pthread_attr_destroy(&attr);
        pthread_cond_destroy(&ctl->cond);
        pthread_mutex_destroy(&ctl->lock);
        free(ctl);
        return -1;
    }

    pthread_attr_destroy(&attr);

    t->ctx = ctl;
    t->stack = NULL;

    return 0;
}

void k_port_task_start(struct task *t)
{
    host_task_ctl *ctl;

    if (t == NULL) {
        return;
    }

    ctl = (host_task_ctl *)t->ctx;
    if (ctl == NULL) {
        return;
    }

    pthread_mutex_lock(&ctl->lock);
    ctl->started = true;
    pthread_cond_signal(&ctl->cond);
    pthread_mutex_unlock(&ctl->lock);
}

void k_port_task_yield(void)
{
    /* POSIX scheduler yield (kernel/sched.h uses the k_ prefix to avoid the clash). */
    sched_yield();
}

void k_port_task_sleep_ms(k_u32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* retry */
    }
}

void k_port_task_exit(int code)
{
    K_UNUSED(code);
    pthread_exit(NULL);
}

struct task *k_port_task_current(void)
{
    struct task *t;

    pthread_once(&s_once, host_once_init);

    t = (struct task *)pthread_getspecific(s_task_key);
    return t;
}

/* --- time base --- */

static pthread_t s_tick_thread;
static volatile bool s_tick_running;

static void *host_tick_thread(void *arg)
{
    long interval_ms = (long)(intptr_t)arg;
    struct timespec ts;

    ts.tv_sec = interval_ms / 1000;
    ts.tv_nsec = (interval_ms % 1000) * 1000000L;

    while (s_tick_running) {
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            /* retry */
        }
        k_sched_tick();
    }

    return NULL;
}

void port_host_start_tick(long interval_ms)
{
    if (interval_ms <= 0) {
        interval_ms = 1;
    }

    s_tick_running = true;
    if (pthread_create(&s_tick_thread, NULL, host_tick_thread, (void *)(intptr_t)interval_ms) !=
        0) {
        s_tick_running = false;
    }
}

void port_host_stop_tick(void)
{
    s_tick_running = false;
    pthread_join(s_tick_thread, NULL);
}
