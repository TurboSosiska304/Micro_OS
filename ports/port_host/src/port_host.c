#define _GNU_SOURCE 1 /* pthread_getattr_np(): the bounds of the stack of a thread */

#include "port_host.h"
#include "port_host_psram.h"

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
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
#include "kernel/kmm.h"
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

/*
 * The codes of the three ends of the process, so that a script that started
 * the simulator can tell them apart (ADR 0050):
 *
 *   0   the machine was switched off - the clean end, a success;
 *   76  the machine asked for a reset and the process is the reset; a
 *       supervisor of the test may start it again on this code;
 *   89  the processor was halted - a panic, a failure (HOST_PANIC_EXIT_CODE).
 *
 * 76 has no POSIX meaning of its own: it is the number this port reserves
 * for "start me again", and a script of the tree reads it by name.
 */
#define HOST_POWER_OFF_EXIT_CODE 0
#define HOST_POWER_REBOOT_EXIT_CODE 76

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
    exit(HOST_POWER_REBOOT_EXIT_CODE);
}

void k_port_power(enum k_power_action action)
{
    switch (action) {
    case K_POWER_HALT:
        k_port_halt();
        break;
    case K_POWER_OFF:
        exit(HOST_POWER_OFF_EXIT_CODE);
        break;
    case K_POWER_REBOOT:
        k_port_reset();
        break;
    }
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
#define PORT_API_MINOR 16u

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

/* The ceiling of a task on the host is the one of every machine without a
 * window of its own: the 2 MiB the tests and the simulator have always had
 * (ADR 0057). */
k_u64_t k_port_memory_limit(void)
{
    return (k_u64_t)TASK_LIMIT_MEMORY_MAX;
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

void *k_port_dma_alloc(k_size_t size, k_size_t align)
{
    k_u8_t *raw;
    k_u8_t *aligned;
    k_size_t total;
    uintptr_t mask;

    if (size == 0u || align == 0u || (align & (align - 1u)) != 0u) {
        return NULL;
    }
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }

    mask = (uintptr_t)align - 1u;
    if (size > (k_size_t)-1 - align - sizeof(void *)) {
        return NULL;
    }
    total = size + align - 1u + sizeof(void *);

    raw = (k_u8_t *)kmm_alloc(total, KMM_PRIORITY_KERNEL);
    if (raw == NULL) {
        return NULL;
    }

    aligned = (k_u8_t *)(((uintptr_t)(raw + sizeof(void *)) + mask) & ~mask);
    ((void **)aligned)[-1] = raw;
    return aligned;
}

void k_port_dma_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    kmm_free(((void **)ptr)[-1]);
}

k_u64_t k_port_uptime_us(void)
{
    return host_uptime_ns() / 1000u;
}

/* --- tasks (pthreads) --- */

/*
 * The guard page under the stack of a task (ADR 0048). A pthread of glibc
 * already has a page at the bottom of its stack that is not readable and not
 * writable; the port says so explicitly and remembers the bounds of the
 * stack, so that a SIGSEGV whose address is in that page is an overflow of
 * this task and not a crash of the process.
 *
 * A signal cannot be handled on the stack that just overflowed: the kernel
 * would push the frame of the signal into the guard and the process would
 * die. Every task therefore takes one of the alternate stacks of a fixed
 * pool before it starts, and the handler runs there and jumps out of the
 * signal to the task's own escape point. The pool is fixed so that a run of
 * thousands of tasks leaks nothing, which the address sanitizer and the
 * valgrind of the gate would otherwise report.
 */
#define HOST_ALTSTACK_COUNT 32u
#define HOST_ALTSTACK_SIZE (16u * 1024u)

static k_u8_t g_altstacks[HOST_ALTSTACK_COUNT][HOST_ALTSTACK_SIZE] __attribute__((aligned(16)));
static atomic_bool g_altstack_used[HOST_ALTSTACK_COUNT];
typedef struct host_task_ctl {
    struct task *task;
    k_port_task_entry_fn entry;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool started;
    /* The bounds of the stack, from pthread_getattr_np() on this thread. */
    void *stack_base; /* the lowest byte the stack may use */
    size_t guard;     /* the bytes below it that are not mapped */
    /* Where the handler of the guard page comes back to, and the number of
     * the signal that brought it there. */
    sigjmp_buf escape;
    unsigned altstack; /* index into g_altstacks, HOST_ALTSTACK_COUNT for none */
    volatile sig_atomic_t fault;

    /*
     * A k_port_task_wake() arrived: the sleep of the task ends now. The flag
     * is guarded by `lock` and cleared by the sleep that consumed it, so a
     * wake that lands before the task begins to sleep still cuts that sleep
     * (the state of the task was changed by the kernel before the door was
     * called, and the sleep looks at the flag before it waits).
     */
    bool woken;
    struct host_task_ctl *next; /* the live contexts of the port */
} host_task_ctl;

/* The control block of the thread that runs: read by the signal handler. */
static __thread host_task_ctl *s_ctl;

/* The two handlers the process had before the port installed its own. */
static struct sigaction s_old_segv;
static struct sigaction s_old_bus;

static unsigned host_altstack_take(void)
{
    unsigned index;

    for (index = 0; index < HOST_ALTSTACK_COUNT; index++) {
        bool expected = false;

        if (atomic_compare_exchange_strong(&g_altstack_used[index], &expected, true)) {
            return index;
        }
    }

    return HOST_ALTSTACK_COUNT;
}

static void host_altstack_give(unsigned index)
{
    if (index < HOST_ALTSTACK_COUNT) {
        atomic_store(&g_altstack_used[index], false);
    }
}

/* Hand a signal the port does not answer to the handler of the process. */
static void host_fault_chain(int sig, siginfo_t *info, void *context, const struct sigaction *old)
{
    if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != NULL) {
        old->sa_sigaction(sig, info, context);
        return;
    }

    if (old->sa_handler == SIG_IGN) {
        return;
    }

    if (old->sa_handler != SIG_DFL && old->sa_handler != NULL) {
        old->sa_handler(sig);
        return;
    }

    /* The default action: the signal is raised again with the default
     * disposition, and the process ends the way it would have ended. */
    {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        (void)sigaction(sig, &action, NULL);
        (void)raise(sig);
    }
}

static void host_fault_handler(int sig, siginfo_t *info, void *context)
{
    host_task_ctl *ctl = s_ctl;
    uintptr_t address = (uintptr_t)((info != NULL) ? info->si_addr : NULL);

    if (ctl != NULL && ctl->stack_base != NULL && ctl->guard != 0) {
        uintptr_t bottom = (uintptr_t)ctl->stack_base;

        if (address >= bottom - ctl->guard && address < bottom) {
            ctl->fault = sig;
            siglongjmp(ctl->escape, 1);
        }
    }

    host_fault_chain(sig, info, context, (sig == SIGBUS) ? &s_old_bus : &s_old_segv);
}

static void host_fault_init(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = host_fault_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);

    (void)sigaction(SIGSEGV, &action, &s_old_segv);
    (void)sigaction(SIGBUS, &action, &s_old_bus);
}

static pthread_once_t s_fault_once = PTHREAD_ONCE_INIT;

/*
 * The contexts that a wake may reach, and the lock of the list.
 *
 * A wake of the kernel names a struct task, and the record of the kernel can
 * outlive the block of this port for a moment (a task that is exiting and a
 * wake that is in flight). The list is what makes the door safe: the wake
 * holds this lock while it finds the context and signals its condition, and
 * the thread that ends takes the lock before it destroys anything. A task
 * that is not in the list has no sleep to cut and the wake is dropped.
 */
static pthread_mutex_t s_task_lock = PTHREAD_MUTEX_INITIALIZER;
static host_task_ctl *s_tasks;

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

    if (ctl->altstack < HOST_ALTSTACK_COUNT) {
        stack_t disable;

        memset(&disable, 0, sizeof(disable));
        disable.ss_flags = SS_DISABLE;
        (void)sigaltstack(&disable, NULL);
        host_altstack_give(ctl->altstack);
        ctl->altstack = HOST_ALTSTACK_COUNT;
    }

    pthread_mutex_lock(&s_task_lock);

    {
        host_task_ctl **slot = &s_tasks;

        while (*slot != NULL) {
            if (*slot == ctl) {
                *slot = ctl->next;
                break;
            }
            slot = &(*slot)->next;
        }
    }

    /*
     * The condition is destroyed after the context has left the list and
     * while the lock of the list is held: a wake that found it a moment ago
     * has finished signalling, and a wake that comes now finds nothing.
     */
    pthread_mutex_destroy(&ctl->lock);
    pthread_cond_destroy(&ctl->cond);

    pthread_mutex_unlock(&s_task_lock);

    free(ctl);
}

/*
 * The bounds of the stack of this thread and the guard page at its bottom.
 * glibc allocates the stack and the port only reads the numbers: the guard is
 * one page unless the machine says otherwise, and a thread whose numbers are
 * not there runs without the contract instead of failing to start.
 */
static void host_task_stack_bounds(host_task_ctl *ctl)
{
    pthread_attr_t attr;
    void *base = NULL;
    size_t size = 0;
    size_t guard = 0;

    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return;
    }

    if (pthread_attr_getstack(&attr, &base, &size) == 0 &&
        pthread_attr_getguardsize(&attr, &guard) == 0) {
        ctl->stack_base = base;
        ctl->guard = guard;
    }

    pthread_attr_destroy(&attr);
}

static void *host_task_thread(void *arg)
{
    host_task_ctl *ctl = (host_task_ctl *)arg;

    pthread_once(&s_once, host_once_init);
    pthread_once(&s_fault_once, host_fault_init);
    pthread_setspecific(s_task_key, ctl->task);

    host_task_stack_bounds(ctl);

    ctl->altstack = host_altstack_take();
    if (ctl->altstack < HOST_ALTSTACK_COUNT) {
        stack_t altstack;

        memset(&altstack, 0, sizeof(altstack));
        altstack.ss_sp = g_altstacks[ctl->altstack];
        altstack.ss_size = HOST_ALTSTACK_SIZE;
        altstack.ss_flags = 0;
        (void)sigaltstack(&altstack, NULL);
    }

    s_ctl = ctl;

    pthread_cleanup_push(host_task_cleanup, ctl);

    /*
     * The escape point of the handler of the guard page. A stack overflow
     * cannot return to the code that overflowed it, and a task that is
     * abandoned would leave its record and its slot to the machine: the
     * handler jumps here and the task ends through the kernel, with the
     * status a program that faulted has on the other ports.
     */
    if (sigsetjmp(ctl->escape, 1) != 0) {
        char line[160];
        int length = snprintf(line, sizeof(line),
                              "host: the stack of the task '%s' overflowed; the task ends, the "
                              "machine stays\n",
                              (ctl->task != NULL) ? ctl->task->name : "?");

        if (length > 0) {
            k_port_write(line, (k_size_t)length);
        }

        task_exit(255);

        /* Not reached: the exit of a task does not come back. */
        pthread_exit(NULL);
    }

    pthread_mutex_lock(&ctl->lock);
    while (!ctl->started) {
        pthread_cond_wait(&ctl->cond, &ctl->lock);
    }
    pthread_mutex_unlock(&ctl->lock);

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
    ctl->altstack = HOST_ALTSTACK_COUNT;
    pthread_mutex_init(&ctl->lock, NULL);

    {
        /*
         * The condition counts from the monotonic clock, the clock of the
         * uptime of this port: a machine whose wall clock is set during a
         * sleep must not make the sleep longer or shorter than it asked for.
         */
        pthread_condattr_t cond_attr;

        pthread_condattr_init(&cond_attr);
        pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
        pthread_cond_init(&ctl->cond, &cond_attr);
        pthread_condattr_destroy(&cond_attr);
    }

    ctl->started = false;
    ctl->woken = false;

    pthread_mutex_lock(&s_task_lock);
    ctl->next = s_tasks;
    s_tasks = ctl;
    pthread_mutex_unlock(&s_task_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, k_port_task_stack_size());
    /* One page under the stack that is not there: the guard of ADR 0048. */
    (void)pthread_attr_setguardsize(&attr, 4096u);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, host_task_thread, ctl) != 0) {
        pthread_attr_destroy(&attr);

        pthread_mutex_lock(&s_task_lock);

        {
            host_task_ctl **slot = &s_tasks;

            while (*slot != NULL) {
                if (*slot == ctl) {
                    *slot = ctl->next;
                    break;
                }
                slot = &(*slot)->next;
            }
        }

        pthread_mutex_unlock(&s_task_lock);

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

void k_port_task_stop(void)
{
    struct task *self = k_port_task_current();
    host_task_ctl *ctl = (self != NULL) ? (host_task_ctl *)self->ctx : NULL;

    if (ctl == NULL) {
        return;
    }

    pthread_mutex_lock(&ctl->lock);
    while (task_state_of(self) == TASK_STATE_STOPPED) {
        pthread_cond_wait(&ctl->cond, &ctl->lock);
    }
    /* The wake that ended the stop is not a wake of the next sleep. */
    ctl->woken = false;
    pthread_mutex_unlock(&ctl->lock);
}

/*
 * The sleep of a task of the host is a wait on the condition of its own
 * context, and not a nanosleep: the kernel wakes a task by name when the
 * object it waits for changes its state, and that wake has to end the sleep
 * at once (ADR 0046). A nanosleep runs to the end and the task would come
 * back a millisecond later - the tick that 1.8 paid for every pipe.
 *
 * A thread that is not a task of the kernel (the boot, a helper of a test)
 * has no context and keeps the plain sleep of the process.
 */
bool k_port_task_sleep_cuttable(void)
{
    return true;
}

void k_port_task_wake(struct task *t)
{
    host_task_ctl *ctl;

    if (t == NULL) {
        return;
    }

    pthread_mutex_lock(&s_task_lock);

    for (ctl = s_tasks; ctl != NULL; ctl = ctl->next) {
        if (ctl->task == t) {
            pthread_mutex_lock(&ctl->lock);
            ctl->woken = true;
            pthread_cond_signal(&ctl->cond);
            pthread_mutex_unlock(&ctl->lock);
            break;
        }
    }

    pthread_mutex_unlock(&s_task_lock);
}

void k_port_task_sleep_ms(k_u32_t ms)
{
    struct task *self = k_port_task_current();
    host_task_ctl *ctl = (self != NULL) ? (host_task_ctl *)self->ctx : NULL;
    struct timespec deadline;

    if (ms == 0) {
        return;
    }

    if (ctl == NULL) {
        struct timespec ts;

        ts.tv_sec = (time_t)(ms / 1000u);
        ts.tv_nsec = (long)(ms % 1000u) * 1000000L;

        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            /* retry */
        }

        return;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        deadline.tv_sec = 0;
        deadline.tv_nsec = 0;
    }

    deadline.tv_sec += (time_t)(ms / 1000u);
    deadline.tv_nsec += (long)(ms % 1000u) * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ctl->lock);

    while (!ctl->woken) {
        if (pthread_cond_timedwait(&ctl->cond, &ctl->lock, &deadline) != 0) {
            /* The deadline, or a condition that cannot be waited on any
             * more: either way this sleep is over. */
            break;
        }
    }

    /*
     * The flag is consumed whether the wait ended by it or by its deadline:
     * a wake that lost the race with the clock must not cut the next sleep of
     * this task.
     */
    ctl->woken = false;

    pthread_mutex_unlock(&ctl->lock);
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
