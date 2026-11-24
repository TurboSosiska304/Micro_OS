#include "port_host.h"
#include "port_host_psram.h"

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "kernel/cpu.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"

/* --- output sink --- */

/*
 * The sink of the tests is closed by the test that replaces it while
 * another task can be writing a log line into it at the same moment:
 * fwrite() on a closed FILE scribbles over the heap of the process, and
 * the damage shows up much later, far away from the console - as a torn
 * block in somebody else's free(). The lock holds the pointer and the
 * file together for as long as the write lasts.
 */
static FILE *s_sink;
static pthread_mutex_t s_sink_lock = PTHREAD_MUTEX_INITIALIZER;

void port_host_set_sink(FILE *sink)
{
    /*
     * The sink of the tests is a file that somebody made: the one that is
     * replaced is closed, otherwise every run of the tests leaves a file
     * open until the process ends.
     */
    pthread_mutex_lock(&s_sink_lock);

    if (s_sink != NULL && s_sink != stdout && s_sink != sink) {
        (void)fclose(s_sink);
    }

    s_sink = sink;
    pthread_mutex_unlock(&s_sink_lock);
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

/*
 * The recursive mutex of the process. The critical section above is one
 * global lock of the kernel; a mutex is a lock of a subsystem and there
 * may be many of them, so each one is a structure of its own.
 */
struct k_port_mutex {
    pthread_mutex_t lock;
};

k_port_mutex_t *k_port_mutex_create(void)
{
    k_port_mutex_t *mutex = calloc(1, sizeof(*mutex));
    pthread_mutexattr_t attr;

    if (mutex == NULL) {
        return NULL;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(&mutex->lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(mutex);
        return NULL;
    }

    pthread_mutexattr_destroy(&attr);

    return mutex;
}

int k_port_mutex_destroy(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    /* A lock that a task holds or waits for is not ours to take away. */
    if (pthread_mutex_destroy(&mutex->lock) != 0) {
        return -1;
    }

    free(mutex);

    return 0;
}

int k_port_mutex_lock(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    return pthread_mutex_lock(&mutex->lock) == 0 ? 0 : -1;
}

int k_port_mutex_unlock(k_port_mutex_t *mutex)
{
    if (mutex == NULL) {
        return -1;
    }

    return pthread_mutex_unlock(&mutex->lock) == 0 ? 0 : -1;
}

/* --- basic port API --- */

void k_port_write(const char *data, k_size_t len)
{
    FILE *sink;

    if (data == NULL || len == 0) {
        return;
    }

    /* The pointer and the file are read and written under one lock: the
     * sink can be replaced and closed by another task in between. */
    pthread_mutex_lock(&s_sink_lock);
    sink = s_sink != NULL ? s_sink : stdout;
    fwrite(data, 1, len, sink);
    fflush(sink);
    pthread_mutex_unlock(&s_sink_lock);
}

/*
 * The console of the host process is a stream of bytes and has no program
 * that can own it: a panic and an ordinary line write into the same sink.
 */
void k_port_console_panic(void) {}

/*
 * The clock of the port counts from the start of the process, not from the
 * start of the machine: the uptime of the kernel is the time since its own
 * boot, and a simulator that reports the uptime of the laptop is lying -
 * the shell prints "3 h" a second after the kernel came up.
 */
static struct timespec s_epoch;
static pthread_once_t s_epoch_once = PTHREAD_ONCE_INIT;

static void host_epoch_init(void)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &s_epoch);
}

static k_u64_t host_uptime_ns(void)
{
    struct timespec ts;
    k_u64_t now;
    k_u64_t epoch;

    pthread_once(&s_epoch_once, host_epoch_init);

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    now = (k_u64_t)ts.tv_sec * 1000000000u + (k_u64_t)ts.tv_nsec;
    epoch = (k_u64_t)s_epoch.tv_sec * 1000000000u + (k_u64_t)s_epoch.tv_nsec;

    return now > epoch ? now - epoch : 0;
}

k_u64_t k_port_uptime_ms(void)
{
    return host_uptime_ns() / 1000000u;
}

/* The code the process of the host stops with: a panic is a failure. */
#define HOST_PANIC_EXIT_CODE 89

void k_port_halt(void)
{
    /*
     * The host is a process, not a board: a halted process is a test that
     * failed, and a CI that waits for a quarter of an hour for it is a
     * test that hides the failure.
     */
    _exit(HOST_PANIC_EXIT_CODE);
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

/* --- the cores ------------------------------------------------------------ */

/*
 * The host is the place where the kernels of the project meet the races
 * first: every task is a pthread already, so the machine is as parallel as
 * the operating system makes it, and the number of cores the kernel is told
 * about is a setting of the run, not a property of the hardware. The
 * environment variable MICRO_OS_CPUS is that setting (ADR 0012): one core by
 * default, so a plain test is the 1.0 kernel; two for `make test-smp` and the
 * soak, where a task of the kernel is put in the queue of one core or the
 * other by the scheduler.
 *
 * A core here is not a thread of the port - the port already gives every
 * task a thread - it is a queue of the kernel and an identity a task carries
 * (k_port_cpu_id() answers the core of the task that asks). A task cannot be
 * on two cores at once because it is one pthread with one `cpu` field, and
 * the scheduler is the only writer of that field.
 */
static k_u32_t s_cpu_count;
static pthread_once_t s_cpu_once = PTHREAD_ONCE_INIT;

static void host_cpus_init(void)
{
    const char *text = getenv("MICRO_OS_CPUS");
    unsigned long count = 1;

    if (text != NULL && text[0] != '\0') {
        char *end = NULL;

        count = strtoul(text, &end, 10);

        if (end == text || (end != NULL && *end != '\0') || count == 0) {
            count = 1;
        }
    }

    if (count > KERNEL_CPUS_MAX) {
        count = KERNEL_CPUS_MAX;
    }

    s_cpu_count = (k_u32_t)count;
}

k_u32_t k_port_cpu_count(void)
{
    pthread_once(&s_cpu_once, host_cpus_init);

    return s_cpu_count;
}

/*
 * The core of the calling task: the scheduler put the task in the queue of
 * one core and the record follows the task, so the answer is the same on
 * every thread that runs it. A thread that is not a task (the tick of the
 * clock, the main thread of the tests) is the core 0.
 */
k_u32_t k_port_cpu_id(void)
{
    struct task *self = k_port_task_current();

    /*
     * The scheduler writes the field under its lock; the read here is the one
     * a task makes about itself on every lock, so it is an atomic load and
     * not a plain one (TSan named exactly this pair when the core count
     * arrived - the kernel and the port are two threads of one process).
     */
    return self != NULL ? __atomic_load_n(&self->cpu, __ATOMIC_RELAXED) : 0u;
}

/*
 * The contract this port was written for: the assert turns a header that
 * grew (or changed its meaning) into a build error, the way vermagic does
 * for a module of Linux.
 */
#define PORT_API_MAJOR 1u
#define PORT_API_MINOR 12u

_Static_assert(PORT_API_MAJOR == KERNEL_PORT_API_VERSION_MAJOR,
               "port_host does not speak the contract of this kernel");

unsigned k_port_api_major(void)
{
    return PORT_API_MAJOR;
}

unsigned k_port_api_minor(void)
{
    return PORT_API_MINOR;
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

int k_port_console_getchar(void)
{
    int c = getchar();

    if (c == EOF) {
        return -1;
    }

    return c;
}

int k_port_console_poll(void)
{
    fd_set readable;
    struct timeval now = {0, 0};

    FD_ZERO(&readable);
    FD_SET(STDIN_FILENO, &readable);

    /* A zero timeout: "is there a character already?" and no waiting. */
    if (select(STDIN_FILENO + 1, &readable, NULL, NULL, &now) <= 0) {
        return -1;
    }

    return k_port_console_getchar();
}

/*
 * The size of the terminal of the developer, in cells. The host is a process
 * and its console is the terminal it was started in, so the answer is the one
 * the kernel of the host gives: TIOCGWINSZ, and 80x25 for a pipe or a file.
 */
void k_port_console_size(k_u16_t *rows, k_u16_t *columns)
{
    struct winsize size;

    memset(&size, 0, sizeof(size));

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row == 0 || size.ws_col == 0) {
        size.ws_row = 25;
        size.ws_col = 80;
    }

    if (rows != NULL) {
        *rows = size.ws_row;
    }
    if (columns != NULL) {
        *columns = size.ws_col;
    }
}

/*
 * The memory a program can map. The host is a process: a task of the kernel
 * and a program of it share the address space of the process, and there is no
 * window of addresses to hand a program page by page. `mmap` is ENOSYS here,
 * as it is on the board, and the x86-64 port is the machine that has the
 * pages (kernel/port/mm.h).
 */
bool k_port_mm_supported(void)
{
    return false;
}

k_u64_t k_port_mm_base(void)
{
    return 0;
}

k_u64_t k_port_mm_limit(void)
{
    return 0;
}

k_u64_t k_port_mm_top(void)
{
    return 0;
}

k_u64_t k_port_mm_current(void)
{
    return 0;
}

void *k_port_mm_page(k_u64_t space, k_u64_t vaddr, k_u32_t prot)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(prot);
    return NULL;
}

void *k_port_mm_writable(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void *k_port_mm_private(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

bool k_port_mm_present(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return false;
}

void *k_port_mm_phys(k_u64_t space, k_u64_t vaddr)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    return NULL;
}

void k_port_mm_unmap(k_u64_t space, k_u64_t vaddr, k_size_t len)
{
    K_UNUSED(space);
    K_UNUSED(vaddr);
    K_UNUSED(len);
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
    return host_uptime_ns() / 1000u;
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

/*
 * The block of the thread is given back whether the work of the task
 * returns or not: a task of the kernel ends with a call that never comes
 * back, and a detached thread that leaves its block behind loses it on
 * every task that has ever run.
 */
static void host_task_cleanup(void *arg)
{
    host_task_ctl *ctl = (host_task_ctl *)arg;

    pthread_mutex_destroy(&ctl->lock);
    pthread_cond_destroy(&ctl->cond);
    free(ctl);
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

    pthread_cleanup_push(host_task_cleanup, ctl);

    ctl->entry(ctl->task);

    pthread_cleanup_pop(1);

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

/*
 * The tasks of the host are functions of the process: there is no ring 3 and
 * no frame of a program to put a handler on, so the kernel runs the handler
 * in place (the answer 0) and a restorer has nothing to restore (ENOSYS).
 */
int k_port_task_signal_enter(struct task *t, int sig, void (*handler)(int), k_ssize_t result,
                             bool from_syscall)
{
    K_UNUSED(t);
    K_UNUSED(sig);
    K_UNUSED(handler);
    K_UNUSED(result);
    K_UNUSED(from_syscall);
    return 0;
}

int k_port_task_signal_leave(struct task *t, k_ssize_t *result)
{
    K_UNUSED(t);
    K_UNUSED(result);
    return KERR_NOSYS;
}

/* --- time base --- */

static pthread_t s_tick_thread;
static atomic_bool s_tick_running;

static void *host_tick_thread(void *arg)
{
    long interval_ms = (long)(intptr_t)arg;
    struct timespec ts;

    ts.tv_sec = interval_ms / 1000;
    ts.tv_nsec = (interval_ms % 1000) * 1000000L;

    while (atomic_load(&s_tick_running)) {
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

    atomic_store(&s_tick_running, true);
    if (pthread_create(&s_tick_thread, NULL, host_tick_thread, (void *)(intptr_t)interval_ms) !=
        0) {
        atomic_store(&s_tick_running, false);
    }
}

void port_host_stop_tick(void)
{
    atomic_store(&s_tick_running, false);
    pthread_join(s_tick_thread, NULL);
}
