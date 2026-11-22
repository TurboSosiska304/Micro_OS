/*
 * Micro OS — the host stress of the campaign of 1.0.
 *
 * The unit tests run one task and a piece of the kernel at a time. This
 * program runs the whole portable kernel of the host port under a load of
 * several tasks at once: allocation and free, pipes, files created,
 * overwritten and removed, and a fork child that exits. It is the load the
 * thread sanitizer needs to see a race, and the load a leak of a block or a
 * descriptor needs to become visible.
 *
 * The program is the harness and not a test of the tree: it makes no judgement
 * about a number the kernel prints. It reports whether the load made
 * progress (a deadlock is a silence and then the watchdog of
 * tools/soak_host.py), how many operations it did, and whether the allocator
 * and the scheduler are where they started.
 *
 *     microos_stress [--seconds N] [--workers N] [--seed N]
 *
 * The exit status is 0 when the load ended with the invariants of the
 * scheduler intact and no block left behind, 1 otherwise.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/kfile.h"
#include "kernel/kmm.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

#define STRESS_WORKERS_MAX 8
#define STRESS_BLOCKS 1024
#define STRESS_RECORDS 128
#define STRESS_FORK_EVERY 64
#define STRESS_PROGRESS_MS 2000u

struct stress {
    atomic_bool done;
    atomic_bool stop;
    atomic_bool first_taken;
    atomic_ulong operations;
    atomic_ulong errors;
    atomic_ulong forks;
    k_u64_t deadline_ms;
    unsigned workers;
    k_u32_t seed;
    int result;
    char first_error[160];
};

struct worker {
    struct stress *stress;
    unsigned id;
    k_u32_t rng;
    k_u8_t file_data[VFS_FILE_DATA_SIZE];
};

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static k_u32_t next_u32(k_u32_t *state)
{
    k_u32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;

    return value;
}

static void note_error(struct stress *stress, const char *text)
{
    (void)atomic_fetch_add_explicit(&stress->errors, 1u, memory_order_relaxed);
    if (!atomic_exchange_explicit(&stress->first_taken, true, memory_order_acq_rel)) {
        (void)snprintf(stress->first_error, sizeof(stress->first_error), "%s", text);
    }
}

static bool past_deadline(const struct stress *stress)
{
    return k_sched_uptime_ms() >= stress->deadline_ms;
}

/* --- the operations of a worker ------------------------------------ */

static void op_memory(struct worker *w)
{
    k_size_t sizes[6] = {16, 32, 128, 512, 1024, 4096};
    k_size_t size = sizes[next_u32(&w->rng) % 6u];
    k_u8_t *block = (k_u8_t *)kmm_alloc(size, KMM_PRIORITY_SYSTEM);

    if (block == NULL) {
        return;
    }

    memset(block, (int)(w->id & 0xFFu), size);
    (void)atomic_fetch_add_explicit(&w->stress->operations, 1u, memory_order_relaxed);
    kmm_free(block);
}

static void op_pipe(struct worker *w)
{
    struct task *self = task_self();
    struct kfile *reader;
    struct kfile *writer;
    int fds[2];
    k_u8_t buffer[64];
    k_ssize_t n;
    k_size_t i;

    if (kfile_pipe(self, fds) != 0) {
        return;
    }

    reader = kfile_get(self, fds[0]);
    writer = kfile_get(self, fds[1]);

    if (reader != NULL && writer != NULL) {
        for (i = 0; i < sizeof(buffer); i++) {
            buffer[i] = (k_u8_t)(w->id + i);
        }

        (void)kfile_write(writer, buffer, 16u);
        n = kfile_read(reader, buffer, sizeof(buffer));

        if (n != 16) {
            note_error(w->stress, "a pipe did not give back what it took");
        } else {
            (void)atomic_fetch_add_explicit(&w->stress->operations, 1u, memory_order_relaxed);
        }
    }

    (void)kfile_close(self, fds[0]);
    (void)kfile_close(self, fds[1]);
}

static void op_file(struct worker *w)
{
    static const char *names[2] = {"alpha", "beta"};
    struct vfs_file file;
    char path[64];
    char data[48];
    char readback[48];
    int rc;

    (void)snprintf(path, sizeof(path), "/w%u/%s", w->id, names[next_u32(&w->rng) % 2u]);
    (void)snprintf(data, sizeof(data), "worker %u rng %u", w->id, next_u32(&w->rng));

    rc = vfs_open(path, VFS_O_CREAT | VFS_O_RDWR | VFS_O_TRUNC, &file, w->file_data);

    if (rc != 0) {
        note_error(w->stress, "a file could not be created");
        return;
    }

    rc = (int)vfs_write(&file, data, strlen(data));
    (void)vfs_close(&file);

    if (rc != (int)strlen(data)) {
        note_error(w->stress, "a file took less than it was given");
        return;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, w->file_data);

    if (rc != 0) {
        note_error(w->stress, "a file could not be opened again");
        return;
    }

    memset(readback, 0, sizeof(readback));
    rc = (int)vfs_read(&file, readback, sizeof(readback));
    (void)vfs_close(&file);

    if (rc != (int)strlen(data) || strcmp(readback, data) != 0) {
        note_error(w->stress, "a file did not give back its own bytes");
        return;
    }

    (void)atomic_fetch_add_explicit(&w->stress->operations, 1u, memory_order_relaxed);

    /* The other name is removed, so every cycle has a create and an
     * unlink of its own as well as a rewrite. */
    if (strcmp(names[next_u32(&w->rng) % 2u], names[0]) != 0) {
        (void)vfs_unlink(path);
    }
}

static void fork_child(void *arg)
{
    struct worker *w = (struct worker *)arg;
    k_u8_t *block;

    block = (k_u8_t *)kmm_alloc(256, KMM_PRIORITY_SYSTEM);

    if (block != NULL) {
        memset(block, 0xA5, 256);
        kmm_free(block);
    }

    (void)atomic_fetch_add_explicit(&w->stress->operations, 1u, memory_order_relaxed);
    (void)atomic_fetch_add_explicit(&w->stress->forks, 1u, memory_order_relaxed);
    task_exit(0);
}

static void op_fork(struct worker *w)
{
    struct task *child;
    int status = 0;

    child = task_fork_shared(task_self(), "stress-fork", fork_child, w);

    if (child == NULL) {
        return;
    }

    if (task_wait(child->pid, &status, 5000u) != 0) {
        note_error(w->stress, "a fork child was never reaped");
    }
}

static void worker_main(void *arg)
{
    struct worker *w = (struct worker *)arg;
    unsigned iteration = 0;

    while (!atomic_load_explicit(&w->stress->stop, memory_order_relaxed) &&
           !past_deadline(w->stress)) {
        op_memory(w);
        op_pipe(w);
        op_file(w);

        iteration++;
        if (iteration % STRESS_FORK_EVERY == 0) {
            op_fork(w);
        }

        if (iteration % 8u == 0) {
            k_sched_sleep_ms(1u);
        } else {
            k_sched_yield();
        }
    }

    task_exit(0);
}

/* --- the session ---------------------------------------------------- */

static bool prepare_volume(void)
{
    k_size_t i;

    if (mfs_ram_init(&g_volume, STRESS_BLOCKS) != 0) {
        return false;
    }

    mfs_ram_device(&g_volume, &g_device);

    if (mfs_format(&g_device, STRESS_RECORDS) != MFS_OK || mfs_mount(&g_device) != MFS_OK) {
        return false;
    }

    if (mfs_vfs_register("/") != 0) {
        return false;
    }

    for (i = 0; i < STRESS_WORKERS_MAX; i++) {
        char path[16];

        (void)snprintf(path, sizeof(path), "/w%u", (unsigned)i);
        if (vfs_mkdir(path) != 0) {
            return false;
        }
    }

    return true;
}

static void stress_main(void *arg)
{
    struct stress *stress = (struct stress *)arg;
    struct worker workers[STRESS_WORKERS_MAX];
    struct task *tasks[STRESS_WORKERS_MAX];
    struct kmm_stats before;
    struct kmm_stats after;
    unsigned started = 0;
    unsigned i;
    k_u64_t next_report;

    if (!prepare_volume()) {
        note_error(stress, "the volume of the stress could not be prepared");
        atomic_store_explicit(&stress->done, true, memory_order_release);
        return;
    }

    memset(workers, 0, sizeof(workers));
    kmm_get_stats(&before);

    for (i = 0; i < stress->workers; i++) {
        char name[TASK_NAME_MAX];

        (void)snprintf(name, sizeof(name), "s%u", (unsigned)i);
        workers[i].stress = stress;
        workers[i].id = i;
        workers[i].rng = 0x9E3779B9u ^ stress->seed ^ (i + 1u) * 2654435761u;
        tasks[i] = task_create(name, worker_main, &workers[i], TASK_PRIO_DEFAULT);
        if (tasks[i] == NULL) {
            note_error(stress, "a worker task could not be created");
            break;
        }
        started++;
    }

    next_report = k_sched_uptime_ms() + STRESS_PROGRESS_MS;

    while (!past_deadline(stress) && started > 0) {
        k_u64_t now = k_sched_uptime_ms();

        if (now >= next_report) {
            fprintf(stderr, "stress: %lu operation(s), %lu fork(s), %lu error(s), %u task(s)\n",
                    (unsigned long)atomic_load_explicit(&stress->operations, memory_order_relaxed),
                    (unsigned long)atomic_load_explicit(&stress->forks, memory_order_relaxed),
                    (unsigned long)atomic_load_explicit(&stress->errors, memory_order_relaxed),
                    (unsigned)task_count());
            fflush(stderr);
            next_report = now + STRESS_PROGRESS_MS;

            /* The queues of every core have to stay consistent while the
             * tasks move between them: a task in two queues, or a ready one
             * in none, is the bug the two-core soak exists to find. */
            if (k_sched_validate() != 0) {
                note_error(stress, "the run queues of the scheduler are not consistent");
            }
        }

        k_sched_sleep_ms(50u);
    }

    atomic_store_explicit(&stress->stop, true, memory_order_release);

    for (i = 0; i < started; i++) {
        int status = 0;

        if (task_wait(tasks[i]->pid, &status, 10000u) != 0) {
            note_error(stress, "a worker did not stop");
        }
    }

    kmm_get_stats(&after);

    if (k_sched_validate() != 0) {
        note_error(stress, "the run queues of the scheduler are not consistent");
    }

    if (after.allocations > before.allocations) {
        char text[120];

        (void)snprintf(text, sizeof(text), "the load left %lu block(s) of kmm behind",
                       (unsigned long)(after.allocations - before.allocations));
        note_error(stress, text);
    }

    if (task_count() > 1) {
        char text[120];

        (void)snprintf(text, sizeof(text), "the load left %u task(s) behind",
                       (unsigned)(task_count() - 1u));
        note_error(stress, text);
    }

    fprintf(stderr, "stress: %lu operation(s), %lu fork(s), %lu error(s)\n",
            (unsigned long)atomic_load_explicit(&stress->operations, memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&stress->forks, memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&stress->errors, memory_order_relaxed));
    fflush(stderr);

    stress->result = atomic_load_explicit(&stress->errors, memory_order_relaxed) == 0 ? 0 : 1;
    atomic_store_explicit(&stress->done, true, memory_order_release);
}

int main(int argc, char **argv)
{
    struct stress stress;
    struct task *runner;
    unsigned seconds = 60u;
    unsigned workers = 4u;
    k_u32_t seed = 1u;
    int i;
    unsigned long waited = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (k_u32_t)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--seconds N] [--workers N] [--seed N]\n", argv[0]);
            return 2;
        }
    }

    if (workers < 1) {
        workers = 1;
    }
    if (workers > STRESS_WORKERS_MAX) {
        workers = STRESS_WORKERS_MAX;
    }

    memset(&stress, 0, sizeof(stress));
    stress.workers = workers;
    stress.result = 0;
    stress.seed = seed;

    printf("Micro OS host stress: %u worker(s), %u second(s), %u core(s)\n", workers, seconds,
           (unsigned)k_sched_cpu_count());
    fflush(stdout);

    k_sched_init();
    port_host_start_tick(1);

    stress.deadline_ms = (k_u64_t)seconds * 1000u;
    runner = task_create("stress-main", stress_main, &stress, TASK_PRIO_DEFAULT);

    if (runner == NULL) {
        printf("cannot create the stress task\n");
        port_host_stop_tick();
        return 1;
    }

    /*
     * A run that does not end is a deadlock and not a slow machine: the
     * watchdog of the wrapper reads the progress of the stress, and this
     * upper bound only keeps the process from waiting forever when the
     * wrapper is not there.
     */
    while (!atomic_load_explicit(&stress.done, memory_order_acquire) &&
           waited < (unsigned long)seconds * 1000u + 30000u) {
        usleep(1000);
        waited += 1;
    }

    if (!atomic_load_explicit(&stress.done, memory_order_acquire)) {
        fprintf(stderr, "stress: no progress in %u second(s): a deadlock\n", seconds + 30u);
        stress.result = 1;
    }

    port_host_stop_tick();

    if (stress.first_error[0] != '\0') {
        fprintf(stderr, "stress: the first error: %s\n", stress.first_error);
    }

    printf("RESULT: %s\n", stress.result == 0 ? "PASS" : "FAIL");

    return stress.result;
}
