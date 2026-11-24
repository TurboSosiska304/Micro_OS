#include "kernel/task.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kfile.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/tty.h"

static struct task g_tasks[TASK_MAX_TASKS];
static k_u32_t g_next_pid = 1;
static k_size_t g_used;

static const char *const g_state_names[] = {
    "new", "ready", "running", "sleeping", "blocked", "zombie", "dead",
};

const char *task_state_name(task_state_t state)
{
    if ((k_u32_t)state >= K_ARRAY_SIZE(g_state_names)) {
        return "?";
    }
    return g_state_names[state];
}

/*
 * A copy of a name that a compiler cannot read as a truncation: the loop
 * stops one byte before the end and the last write is the zero. The kernel
 * has no string header of its own, and `strncpy` followed by a zero - the
 * idiom every libc offers - is still a truncation to GCC at -O2.
 */
static void copy_bounded(char *dst, k_size_t size, const char *src)
{
    k_size_t i = 0;

    if (size == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < size && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void task_set_name(struct task *t, const char *name)
{
    if (t == NULL) {
        return;
    }
    if (name == NULL) {
        t->name[0] = '\0';
        return;
    }
    copy_bounded(t->name, TASK_NAME_MAX, name);
}

void task_set_mem_priority(struct task *t, int priority)
{
    if (t == NULL) {
        return;
    }

    if (priority > KMM_PRIORITY_KERNEL) {
        priority = KMM_PRIORITY_KERNEL;
    }
    if (priority < KMM_PRIORITY_USER) {
        priority = KMM_PRIORITY_USER;
    }

    t->mem_priority = (k_i8_t)priority;
}

int task_get_mem_priority(struct task *t)
{
    if (t == NULL) {
        return KMM_PRIORITY_USER;
    }

    return (int)t->mem_priority;
}

/* --- the limits of a task --- */

static bool limit_known(k_u32_t which)
{
    return which == KSYS_RLIMIT_MEMORY || which == KSYS_RLIMIT_CPU || which == KSYS_RLIMIT_FDS;
}

int task_limit_get(struct task *t, k_u32_t which, k_u64_t *out)
{
    if (t == NULL || out == NULL || !limit_known(which)) {
        return KERR_INVAL;
    }

    k_port_enter_critical();

    switch (which) {
    case KSYS_RLIMIT_MEMORY:
        *out = t->limit_memory;
        break;
    case KSYS_RLIMIT_CPU:
        *out = (k_u64_t)t->limit_cpu_ms;
        break;
    case KSYS_RLIMIT_FDS:
        *out = (k_u64_t)t->limit_fds;
        break;
    default:
        k_port_exit_critical();
        return KERR_INVAL;
    }

    k_port_exit_critical();

    return 0;
}

/*
 * The limits start at the ceiling of the system and only go down. A program
 * that could raise its own limit would have no limit at all, and there is no
 * second user to grant the raise: uid 0 is the only user and it gets no
 * exemption (the rule of the mode check, ADR 0005). The answer to a raise is
 * EPERM and not EINVAL: the number is a real limit, only the right to set it
 * is missing.
 */
int task_limit_set(struct task *t, k_u32_t which, k_u64_t value)
{
    int rc = 0;

    if (t == NULL || !limit_known(which)) {
        return KERR_INVAL;
    }

    k_port_enter_critical();

    switch (which) {
    case KSYS_RLIMIT_MEMORY:
        if (value > t->limit_memory || value > (k_u64_t)TASK_LIMIT_MEMORY_MAX) {
            rc = KERR_PERM;
        } else {
            t->limit_memory = value;
        }
        break;
    case KSYS_RLIMIT_CPU:
        if (value > (k_u64_t)t->limit_cpu_ms || value > (k_u64_t)TASK_LIMIT_CPU_MAX) {
            rc = KERR_PERM;
        } else {
            t->limit_cpu_ms = (k_u32_t)value;
            /* A task that lowered its budget below the time it has already
             * used is over the limit at the next tick: the mark goes out
             * then, once. */
            t->cpu_limit_signalled = false;
        }
        break;
    case KSYS_RLIMIT_FDS:
        if (value > (k_u64_t)t->limit_fds || value > (k_u64_t)TASK_LIMIT_FDS_MAX) {
            rc = KERR_PERM;
        } else {
            t->limit_fds = (k_u8_t)value;
        }
        break;
    default:
        rc = KERR_INVAL;
        break;
    }

    k_port_exit_critical();

    return rc;
}

k_u32_t task_fd_limit(const struct task *t)
{
    k_u32_t limit;

    if (t == NULL) {
        return TASK_MAX_FDS;
    }

    k_port_enter_critical();
    limit = (k_u32_t)t->limit_fds;
    k_port_exit_critical();

    return (limit < TASK_MAX_FDS) ? limit : TASK_MAX_FDS;
}

k_u32_t task_get_uid(const struct task *t)
{
    k_u32_t uid;

    if (t == NULL) {
        return 0u;
    }

    k_port_enter_critical();
    uid = t->uid;
    k_port_exit_critical();

    return uid;
}

k_u32_t task_get_gid(const struct task *t)
{
    k_u32_t gid;

    if (t == NULL) {
        return 0u;
    }

    k_port_enter_critical();
    gid = t->gid;
    k_port_exit_critical();

    return gid;
}

void task_set_uid(struct task *t, k_u32_t uid)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->uid = uid;
    k_port_exit_critical();
}

void task_set_gid(struct task *t, k_u32_t gid)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->gid = gid;
    k_port_exit_critical();
}

void task_set_creds(struct task *t, k_u32_t uid, k_u32_t gid)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->gid = gid;
    t->uid = uid;
    k_port_exit_critical();
}

bool task_is_root(const struct task *t)
{
    return task_get_uid(t) == 0u;
}

/*
 * The session of a task. The mark is one byte and is written by the task
 * itself when it starts: a server creates a session and the child runs
 * before the server's next line, so the child is the one that can say what
 * it is without a race.
 */
void task_set_tty(struct task *t, k_u8_t tty)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->tty = tty;
    k_port_exit_critical();
}

k_u8_t task_get_tty(const struct task *t)
{
    if (t == NULL) {
        return TASK_TTY_NONE;
    }

    return t->tty;
}

const char *task_tty_name(k_u8_t tty)
{
    switch (tty) {
    case TASK_TTY_CONSOLE:
        return "console";
    case TASK_TTY_TELNET:
        return "telnet";
    default:
        return "none";
    }
}

void task_creds_inherit(struct task *child, const struct task *parent)
{
    k_u32_t uid;
    k_u32_t gid;

    if (child == NULL || parent == NULL) {
        return;
    }

    k_port_enter_critical();
    uid = parent->uid;
    gid = parent->gid;
    child->uid = uid;
    child->gid = gid;
    k_port_exit_critical();
}

void task_limits_inherit(struct task *child, const struct task *parent)
{
    if (child == NULL || parent == NULL) {
        return;
    }

    k_port_enter_critical();
    child->limit_memory = parent->limit_memory;
    child->limit_cpu_ms = parent->limit_cpu_ms;
    child->limit_fds = parent->limit_fds;
    k_port_exit_critical();
}

bool task_memory_fits(const struct task *t, k_size_t size)
{
    k_u64_t used;
    k_u64_t limit;

    if (t == NULL) {
        return true;
    }

    k_port_enter_critical();
    used = t->mem_used;
    limit = t->limit_memory;
    k_port_exit_critical();

    return ((k_u64_t)size <= limit) && (used <= limit - (k_u64_t)size);
}

void task_memory_reset(void)
{
    k_size_t i;

    k_port_enter_critical();
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        g_tasks[i].mem_used = 0;
    }
    k_port_exit_critical();
}

void task_memory_charge(struct task *t, k_size_t size)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    if (t->in_use) {
        t->mem_used += (k_u64_t)size;
    }
    k_port_exit_critical();
}

/*
 * A block may be given back by a task that did not allocate it: the memory
 * of a fork is shared on a machine without an MMU, and a child may free a
 * block of its parent. The charge goes to the task that owns the block,
 * found by the pid the block carries, so the budget of the freer is not
 * touched by somebody else's allocation.
 */
void task_memory_uncharge(k_u32_t pid, k_size_t size)
{
    k_size_t i;

    if (pid == 0u) {
        return;
    }

    k_port_enter_critical();
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];

        if (t->in_use && t->pid == pid) {
            if (t->mem_used >= (k_u64_t)size) {
                t->mem_used -= (k_u64_t)size;
            } else {
                t->mem_used = 0;
            }
            break;
        }
    }
    k_port_exit_critical();
}

bool task_set_cwd(struct task *t, const char *dir)
{
    if (t == NULL) {
        return false;
    }

    if (dir == NULL) {
        t->cwd[0] = '\0';
        return true;
    }

    /*
     * A directory that does not fit is refused and not cut: a cwd of 31
     * characters of a name of 40 is a directory of somebody else, and the
     * shell that walks it removes the wrong file (the cleanroom of 1.6).
     * The caller keeps the directory it had.
     */
    if (strlen(dir) >= TASK_CWD_MAX) {
        return false;
    }

    copy_bounded(t->cwd, sizeof(t->cwd), dir);

    return true;
}

const char *task_get_cwd(struct task *t)
{
    if (t == NULL) {
        return NULL;
    }

    return t->cwd;
}

void task_set_job(struct task *t, k_u32_t job)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->job = job;
    k_port_exit_critical();
}

k_u32_t task_get_job(const struct task *t)
{
    k_u32_t job;

    if (t == NULL) {
        return TASK_JOB_NONE;
    }

    k_port_enter_critical();
    job = t->job;
    k_port_exit_critical();

    return job;
}

/*
 * The trampoline: every task starts running here. It marks the task as
 * running and calls the user entry point.
 */
static void task_trampoline(void *arg)
{
    struct task *t = (struct task *)arg;

    k_sched_task_running(t);

    if (t->entry != NULL) {
        t->entry(t->arg);
    }

    task_exit(0);
}

/*
 * A free slot of the table with the fields a new task shares with every other
 * one: a pid, a state, a parent, the directory and the name. The port context
 * is not made here - an ordinary task and the child of a fork get different
 * ones - and the slot is not published until the caller says so.
 */
/*
 * What a new task takes from the one that makes it: nothing (a task of the
 * boot or of the kernel), the fields of a child whose program shares the
 * memory of its parent (the stages of a pipeline), or the whole of a child
 * with an address space of its own (a fork, which also keeps the dispositions
 * of the signals).
 */
enum task_inherit {
    TASK_INHERIT_NONE,
    TASK_INHERIT_SHARED,
    TASK_INHERIT_FORK,
};

static struct task *task_alloc_slot(const char *name, task_entry_fn entry, void *arg, k_u8_t prio,
                                    struct task *inherit, enum task_inherit kind)
{
    struct task *t = NULL;
    k_size_t i;

    if (prio > TASK_PRIO_MAX) {
        prio = TASK_PRIO_MAX;
    }

    k_port_enter_critical();

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        if (!g_tasks[i].in_use) {
            t = &g_tasks[i];
            break;
        }
    }

    if (t != NULL) {
        /*
         * The credentials come from the task that makes the new one: a
         * child of any kind runs as its parent does. The kernel itself, which
         * creates the first tasks of the boot, has no record and is root.
         */
        struct task *creator = task_self();

        memset(t, 0, sizeof(*t));
        t->in_use = true;
        t->pid = g_next_pid++;
        t->state = TASK_STATE_NEW;
        t->prio = prio;
        t->entry = entry;
        t->arg = arg;
        t->stack_size = k_port_task_stack_size();
        t->parent = creator;
        t->uid = (creator != NULL) ? creator->uid : 0u;
        t->gid = (creator != NULL) ? creator->gid : 0u;
        t->mem_priority = KMM_PRIORITY_USER;
        t->limit_memory = TASK_LIMIT_MEMORY_MAX;
        t->limit_cpu_ms = TASK_LIMIT_CPU_MAX;
        t->limit_fds = TASK_LIMIT_FDS_MAX;
        t->cpu_stamp_ms = k_sched_uptime_ms();
        task_set_cwd(t, "/");
        task_set_name(t, name);

        /*
         * What a child of a fork takes from its parent is written **in** the
         * section and not after it: the slot is `in_use` and in the table
         * from the moment above, so another core - the child itself, which
         * the port starts as a thread of its own - reads it while this write
         * is in flight. The memory checker of the cloud named the read of
         * `task_exit` and the write here as a race, which is exactly what a
         * second core would do with those two.
         */
        if (inherit != NULL && kind != TASK_INHERIT_NONE) {
            t->parent = inherit;
            t->job = inherit->job;
            task_set_cwd(t, task_get_cwd(inherit));
            task_creds_inherit(t, inherit);
            task_limits_inherit(t, inherit);

            if (kind == TASK_INHERIT_FORK) {
                task_signal_inherit(t, inherit);
            }
        }

        /* The table of the descriptors is taken below and not here: it is
         * the lock of the files and not the critical section, and kfile_alloc
         * takes the two in the other order - the critical section under the
         * lock of the files - so this one has to stay outside or the two
         * orders make a deadlock (the race detector of the cloud named
         * exactly the pair). */

        g_used++;
    }

    k_port_exit_critical();

    return t;
}

static void task_free_slot(struct task *t)
{
    if (t == NULL) {
        return;
    }

    /* The mode of the terminal of a session goes with the task, and a console
     * that a program left raw goes back to canonical (kernel/tty.c). */
    k_tty_task_exit(t);

    kfile_table_close(t);

    k_port_enter_critical();
    memset(t, 0, sizeof(*t));
    if (g_used > 0) {
        g_used--;
    }
    k_port_exit_critical();
}

/*
 * The common part of a started task: a fresh slot, the descriptors it
 * inherits - none for an ordinary task, the ones of the parent for the
 * child of a fork - and the context of the port. The copy of the open files
 * happens before the port starts the task, so a child of a fork can never
 * run with a half-copied table.
 */
static struct task *task_spawn(const char *name, task_entry_fn entry, void *arg, k_u8_t prio,
                               struct task *inherit)
{
    struct task *t;

    t = task_alloc_slot(name, entry, arg, prio, inherit,
                        inherit != NULL ? TASK_INHERIT_SHARED : TASK_INHERIT_NONE);
    if (t == NULL) {
        kerror("task: the task table is full (%u slots)", (unsigned)TASK_MAX_TASKS);
        return NULL;
    }

    /* The files of a child are copied before the port makes it runnable:
     * nothing reads the table of a task that does not exist yet. */
    kfile_table_copy(t, inherit);

    /*
     * The execution context is created suspended, so that the task is
     * registered in the run queue before it can run for the first time.
     */
    if (k_port_task_create(t, task_trampoline, t) != 0) {
        task_free_slot(t);
        kerror("task: the port refused to create '%s'", name != NULL ? name : "?");
        return NULL;
    }

    k_sched_task_ready(t);
    k_port_task_start(t);

    kinfo("task: created '%s' (pid %u, prio %u)", t->name, (unsigned)t->pid, (unsigned)t->prio);

    return t;
}

struct task *task_create(const char *name, task_entry_fn entry, void *arg, k_u8_t prio)
{
    return task_spawn(name, entry, arg, prio, NULL);
}

struct task *task_fork_shared(struct task *parent, const char *name, task_entry_fn entry, void *arg)
{
    if (parent == NULL) {
        return NULL;
    }

    return task_spawn(name, entry, arg, parent->prio, parent);
}

/*
 * The child of a fork on a machine whose port can copy the context of the
 * caller (ADR 0005, K4). The child has an address space of its own: the port
 * copies the tables, marks the pages copy-on-write and makes the context
 * return from the system call with zero. The open files are copied with a
 * reference each, as POSIX leaves them across a fork; the rest of the record
 * is the one of any task.
 */
int task_fork(struct task *parent, const char *name, struct task **child)
{
    struct task *c;
    int rc;

    if (parent == NULL || child == NULL) {
        return -2;
    }

    *child = NULL;

    c = task_alloc_slot(name, NULL, NULL, parent->prio, parent, TASK_INHERIT_FORK);
    if (c == NULL) {
        return -2;
    }

    kfile_table_copy(c, parent);

    rc = k_port_fork_copy(c);
    if (rc < 0) {
        task_free_slot(c);
        return (rc == -1) ? -1 : -2;
    }

    k_sched_task_ready(c);
    k_port_task_start(c);

    kinfo("task: forked '%s' (pid %u, parent %u)", c->name, (unsigned)c->pid,
          (unsigned)parent->pid);

    *child = c;
    return 0;
}

void task_exit(int code)
{
    struct task *self = task_self();

    if (self != NULL) {
        char name[TASK_NAME_MAX];
        k_u32_t pid;

        /*
         * The slot is read before the task is published as a zombie: from
         * that moment its parent may reap it, the slot is zeroed, and the
         * line in the log would name a task that never exited - or worse,
         * the task that got the slot next.
         */
        k_port_enter_critical();
        self->exit_code = (k_u32_t)code;
        memcpy(name, self->name, sizeof(name));
        name[sizeof(name) - 1] = '\0';
        pid = self->pid;
        /* The parent learns of the end before the slot can be reaped. */
        task_signal_notify_parent(self);

        /*
         * The children of a task that ends are the children of init from
         * here on (POSIX reparents them to the init process). Without it a
         * child of a parent that was killed - Ctrl+C on a program that had
         * forked - keeps a parent that no longer exists, nobody calls
         * waitpid() for it, and its slot stays a zombie for the life of the
         * machine: the table of 32 slots is a resource, and the soak of the
         * two-core machine found the leak as a task that was 3000 pids old.
         */
        {
            struct task *init = task_by_pid(1);

            if (init != NULL && init != self) {
                k_size_t i;

                for (i = 0; i < TASK_MAX_TASKS; i++) {
                    if (g_tasks[i].in_use && g_tasks[i].parent == self) {
                        g_tasks[i].parent = init;
                    }
                }
            }
        }

        k_port_exit_critical();

        /*
         * The descriptors of the task go with it, as POSIX closes the files
         * of a process that exits. This happens before the task is published
         * as a zombie: from then on its parent may reap the slot, and a
         * descriptor that was not closed would be lost from both tables.
         */
        kfile_table_close(self);

        /*
         * The line of the end goes out before the state that lets the parent
         * continue. `k_sched_task_terminated()` publishes ZOMBIE, and a parent
         * that waits for this task may reap it and print the next prompt at
         * once - on a machine of two cores, from another processor. The exit
         * of a task would then appear after the prompt of the shell that
         * started it, which is a console that shows a command finishing after
         * the next one began. A log line is cheap next to that.
         */
        kinfo("task: '%s' (pid %u) exited with code %d", name, (unsigned)pid, code);

        k_sched_task_terminated(self);
    }

    k_port_task_exit(code);

    /* Not reached. */
    for (;;) {
    }
}

struct task *task_self(void)
{
    return k_port_task_current();
}

task_state_t task_state_of(const struct task *t)
{
    task_state_t state = TASK_STATE_DEAD;

    if (t == NULL || !t->in_use) {
        return state;
    }

    k_port_enter_critical();
    state = t->state;
    k_port_exit_critical();

    return state;
}

struct task *task_by_pid(k_u32_t pid)
{
    struct task *found = NULL;
    k_size_t i;

    k_port_enter_critical();
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].pid == pid) {
            found = &g_tasks[i];
            break;
        }
    }
    k_port_exit_critical();

    return found;
}

struct task *task_get_by_index(k_size_t index)
{
    struct task *t = NULL;

    k_port_enter_critical();
    if (index < TASK_MAX_TASKS && g_tasks[index].in_use) {
        t = &g_tasks[index];
    }
    k_port_exit_critical();

    return t;
}

struct task *task_get_by_index_unlocked(k_size_t index)
{
    if (index < TASK_MAX_TASKS && g_tasks[index].in_use) {
        return &g_tasks[index];
    }

    return NULL;
}

k_size_t task_count(void)
{
    k_size_t count;

    k_port_enter_critical();
    count = g_used;
    k_port_exit_critical();

    return count;
}

k_size_t task_count_by_state(task_state_t state)
{
    k_size_t i;
    k_size_t count = 0;

    k_port_enter_critical();
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].state == state) {
            count++;
        }
    }
    k_port_exit_critical();

    return count;
}

int task_reap(k_u32_t pid)
{
    k_size_t i;
    int rc = -1;

    k_port_enter_critical();
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].pid == pid &&
            (g_tasks[i].state == TASK_STATE_ZOMBIE || g_tasks[i].state == TASK_STATE_DEAD)) {
            memset(&g_tasks[i], 0, sizeof(g_tasks[i]));
            if (g_used > 0) {
                g_used--;
            }
            rc = 0;
            break;
        }
    }
    k_port_exit_critical();

    return rc;
}

/*
 * The state and the code of a task, read in one look under the critical
 * section: the slot can be reaped by another task between two reads, and a
 * caller that saw a zombie and then read the code of a stranger is a bug
 * the `ps` of the shell would show and nobody could explain.
 */
static bool task_lookup(k_u32_t pid, task_state_t *state, int *code)
{
    k_size_t i;
    bool found = false;

    k_port_enter_critical();

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        if (g_tasks[i].in_use && g_tasks[i].pid == pid) {
            if (state != NULL) {
                *state = g_tasks[i].state;
            }
            if (code != NULL) {
                *code = (int)g_tasks[i].exit_code;
            }
            found = true;
            break;
        }
    }

    k_port_exit_critical();

    return found;
}

int task_wait(k_u32_t pid, int *exit_code, k_u32_t timeout_ms)
{
    k_u32_t waited = 0;

    for (;;) {
        task_state_t state;
        int code = 0;

        if (!task_lookup(pid, &state, &code)) {
            /* Already reaped or never existed. */
            return -1;
        }

        if (state == TASK_STATE_ZOMBIE || state == TASK_STATE_DEAD) {
            if (exit_code != NULL) {
                *exit_code = code;
            }
            task_reap(pid);
            return 0;
        }

        if (waited >= timeout_ms) {
            return -2;
        }

        k_sched_sleep_ms(1);
        waited++;
    }
}

/* --- signals --- */

/* The place of a signal in the table of a task, -1 when it is not one of the
 * signals of the system. The four are not a range: the numbers are POSIX. */
static int signal_slot(int sig)
{
    switch (sig) {
    case K_SIGINT:
        return 0;
    case K_SIGPIPE:
        return 1;
    case K_SIGTERM:
        return 2;
    case K_SIGCHLD:
        return 3;
    default:
        return -1;
    }
}

static int signal_number(int slot)
{
    static const int numbers[] = {K_SIGINT, K_SIGPIPE, K_SIGTERM, K_SIGCHLD};

    if (slot < 0 || (k_size_t)slot >= K_ARRAY_SIZE(numbers)) {
        return 0;
    }

    return numbers[slot];
}

static k_u32_t signal_bit(int sig)
{
    return (k_u32_t)1u << (k_u32_t)signal_slot(sig);
}

/*
 * The processor time of a task is the time between the moments it is known
 * to be running. The tick of a port that can see the running task is one
 * such moment, and so is the edge of a system call; the stamp keeps the two
 * from counting the same milliseconds twice. A task that sleeps has its
 * stamp set on both sides of the wait, so a long wait is not processor time.
 * When the task reaches the limit it is marked with SIGTERM, the closest
 * signal of the four: POSIX has SIGXCPU for this, but a fifth signal is not
 * in the contract (kernel/signal.h). The default action of SIGTERM ends the
 * task with 128 + 15; a handler may catch it, exactly as a handler of SIGXCPU
 * may, and then the task goes on - the limit is a mark and not a kill from
 * the timer, which would end a task from the context of another.
 */
bool task_cpu_tick(struct task *t)
{
    k_u64_t now;
    k_u64_t delta;
    bool marked = false;

    if (t == NULL) {
        return false;
    }

    now = k_sched_uptime_ms();

    k_port_enter_critical();

    if (t->in_use && t->state != TASK_STATE_ZOMBIE && t->state != TASK_STATE_DEAD) {
        delta = (now > t->cpu_stamp_ms) ? (now - t->cpu_stamp_ms) : 0u;
        t->cpu_stamp_ms = now;

        if (delta > (k_u64_t)TASK_LIMIT_CPU_MAX - t->cpu_ms) {
            t->cpu_ms = TASK_LIMIT_CPU_MAX;
        } else {
            t->cpu_ms += (k_u32_t)delta;
        }

        if (!t->cpu_limit_signalled && t->cpu_ms >= t->limit_cpu_ms) {
            t->cpu_limit_signalled = true;
            __atomic_fetch_or(&t->sig_pending, signal_bit(K_SIGTERM), __ATOMIC_RELAXED);
            marked = true;
        }
    }

    k_port_exit_critical();

    return marked;
}
/* The signals whose default answer is the end of the task. SIGCHLD is the
 * one whose default is to be ignored, as everywhere else. */
static bool signal_ends_by_default(int sig)
{
    return sig != K_SIGCHLD;
}

bool task_signal_interrupts(const struct task *t)
{
    k_u32_t pending;
    int slot;

    if (t == NULL) {
        return false;
    }

    /*
     * The mark and the handler are written by a sender on another core and
     * read here by the task itself on its way into a wait, without the lock:
     * every access to the pair is an atomic, so the reader sees a mark or a
     * handler, and the pair the write made, never half of one (the two-core
     * run of TSan named this pair).
     */
    pending = __atomic_load_n(&t->sig_pending, __ATOMIC_RELAXED);

    if (pending == 0u) {
        return false;
    }

    for (slot = 0; slot < (int)K_ARRAY_SIZE(t->sig_handlers); slot++) {
        int sig = signal_number(slot);
        task_signal_fn handler;

        if (sig == 0 || (pending & signal_bit(sig)) == 0u) {
            continue;
        }

        handler = __atomic_load_n(&t->sig_handlers[slot], __ATOMIC_RELAXED);

        if (handler == K_SIG_IGN) {
            continue;
        }

        if (handler != K_SIG_DFL) {
            return true;
        }

        if (signal_ends_by_default(sig)) {
            return true;
        }
    }

    return false;
}

k_u32_t task_signal_pending(const struct task *t)
{
    return t != NULL ? __atomic_load_n(&t->sig_pending, __ATOMIC_RELAXED) : 0u;
}

int task_signal_send(k_u32_t pid, int sig)
{
    struct task *t;

    if (signal_slot(sig) < 0) {
        return KERR_INVAL;
    }

    if (pid == 0u) {
        return KERR_INVAL;
    }

    t = task_by_pid(pid);
    if (t == NULL) {
        return KERR_SRCH;
    }

    k_port_enter_critical();
    if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_DEAD) {
        k_port_exit_critical();
        return KERR_SRCH;
    }
    k_port_exit_critical();

    /*
     * The mark is one bit of a word the task itself reads without the lock
     * (task_signal_interrupts, task_signal_check): every access to it is an
     * atomic, because the critical section of a port is the interrupts of
     * one core and not a lock of the machine (ADR 0012).
     */
    __atomic_fetch_or(&t->sig_pending, signal_bit(sig), __ATOMIC_RELAXED);

    /* A task that waits for an event or a timer may come back at once: the
     * check of the signals is what its wait and its next call look at. */
    k_sched_unblock(t);

    return 0;
}

/*
 * The signal of a whole job: the stages of a pipeline and the programs they
 * started. The caller is the task that reads the console for the job, so it
 * is not a target - Ctrl+C is the end of the work and not of the shell.
 *
 * The pids are collected under one critical section because the table can
 * change while the signals are sent; task_signal_send() checks the state of
 * each task again, so a task that ended in between is simply passed over.
 */
int task_signal_job(k_u32_t job, int sig)
{
    struct task *self = task_self();
    k_u32_t pids[TASK_MAX_TASKS];
    k_size_t count = 0;
    k_size_t i;
    int sent = 0;

    if (job == TASK_JOB_NONE) {
        return KERR_INVAL;
    }

    k_port_enter_critical();

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];

        if (!t->in_use || t == self || t->job != job) {
            continue;
        }

        if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_DEAD) {
            continue;
        }

        pids[count++] = t->pid;
    }

    k_port_exit_critical();

    for (i = 0; i < count; i++) {
        if (task_signal_send(pids[i], sig) == 0) {
            sent++;
        }
    }

    return (sent > 0) ? 0 : KERR_SRCH;
}

/*
 * The signal of the whole machine: every living task but the one that asks.
 * The pids are collected under the critical section for the same reason as
 * the job above; the signal itself goes out through task_signal_send(),
 * which checks the state again.
 */
int task_signal_all(const struct task *except, int sig)
{
    k_u32_t pids[TASK_MAX_TASKS];
    k_size_t count = 0;
    k_size_t i;
    int sent = 0;

    if (signal_slot(sig) < 0) {
        return KERR_INVAL;
    }

    k_port_enter_critical();

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = &g_tasks[i];

        if (!t->in_use || t == except) {
            continue;
        }

        if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_DEAD) {
            continue;
        }

        pids[count++] = t->pid;
    }

    k_port_exit_critical();

    for (i = 0; i < count; i++) {
        if (task_signal_send(pids[i], sig) == 0) {
            sent++;
        }
    }

    return sent;
}

k_size_t task_count_living_except(const struct task *except)
{
    k_size_t i;
    k_size_t count = 0;

    k_port_enter_critical();

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        const struct task *t = &g_tasks[i];

        if (!t->in_use || t == except) {
            continue;
        }

        if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_DEAD) {
            continue;
        }

        count++;
    }

    k_port_exit_critical();

    return count;
}

task_signal_fn task_signal_set(struct task *t, int sig, task_signal_fn handler)
{
    int slot = signal_slot(sig);
    task_signal_fn previous;

    if (t == NULL) {
        t = task_self();
    }

    if (t == NULL || slot < 0) {
        return K_SIG_ERR;
    }

    previous = __atomic_load_n(&t->sig_handlers[slot], __ATOMIC_RELAXED);
    __atomic_store_n(&t->sig_handlers[slot], handler, __ATOMIC_RELAXED);

    /* An ignored signal is thrown away, as POSIX asks: there is no point in
     * waking the task for a mark it will never answer. */
    if (handler == K_SIG_IGN) {
        __atomic_fetch_and(&t->sig_pending, (k_u32_t)~signal_bit(sig), __ATOMIC_RELAXED);
    }

    return previous;
}

void task_signal_inherit(struct task *child, const struct task *parent)
{
    if (child == NULL || parent == NULL) {
        return;
    }

    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(child->sig_handlers); i++) {
        task_signal_fn handler = __atomic_load_n(&parent->sig_handlers[i], __ATOMIC_RELAXED);

        __atomic_store_n(&child->sig_handlers[i], handler, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&child->sig_pending, 0u, __ATOMIC_RELAXED);
}

/*
 * The dispositions of a task across an execve. The handler of a signal is an
 * address of the image that called the call, and the image is gone once
 * execve replaces it: POSIX resets a caught signal to its default action and
 * leaves an ignored one ignored, which is what this does. The marks that are
 * pending stay: a signal that arrived before the exec is answered by the new
 * program, as POSIX leaves it.
 */
void task_signal_reset_on_exec(struct task *t)
{
    k_size_t i;

    if (t == NULL) {
        return;
    }

    for (i = 0; i < K_ARRAY_SIZE(t->sig_handlers); i++) {
        task_signal_fn handler = __atomic_load_n(&t->sig_handlers[i], __ATOMIC_RELAXED);

        if (handler != K_SIG_IGN) {
            __atomic_store_n(&t->sig_handlers[i], K_SIG_DFL, __ATOMIC_RELAXED);
        }
    }
}

void task_signal_notify_parent(struct task *self)
{
    struct task *parent;

    if (self == NULL) {
        return;
    }

    k_port_enter_critical();
    parent = self->parent;
    k_port_exit_critical();

    if (parent != NULL) {
        /* The parent may be on another core and reads its marks without the
         * lock: the bit is added atomically. */
        __atomic_fetch_or(&parent->sig_pending, signal_bit(K_SIGCHLD), __ATOMIC_RELAXED);
    }
}

void task_signal_check(struct task *t, k_ssize_t result, bool from_syscall)
{
    int slot;

    if (t == NULL) {
        return;
    }

    for (slot = 0; slot < (int)K_ARRAY_SIZE(t->sig_handlers); slot++) {
        int sig = signal_number(slot);
        task_signal_fn handler;
        int rc;

        if (sig == 0) {
            continue;
        }

        /* Take the mark with one atomic operation: another core may add a
         * bit at any moment, and clearing ours must not lose it. The handler
         * is read after the mark is consumed, which is what POSIX asks of a
         * signal delivered while the program changes its handler. */
        k_u32_t mark = signal_bit(sig);
        k_u32_t seen = __atomic_fetch_and(&t->sig_pending, (k_u32_t)~mark, __ATOMIC_RELAXED);

        if ((seen & mark) == 0u) {
            continue;
        }

        handler = __atomic_load_n(&t->sig_handlers[slot], __ATOMIC_RELAXED);

        if (handler == K_SIG_IGN) {
            continue;
        }

        if (handler == K_SIG_DFL) {
            if (!signal_ends_by_default(sig)) {
                continue;
            }
            task_exit(128 + sig);
            return;
        }

        rc = k_port_task_signal_enter(t, sig, handler, result, from_syscall);
        if (rc > 0) {
            /* The port put the handler in the context of the program: it runs
             * when the task returns to user space. */
            return;
        }

        if (rc < 0) {
            /* The frame of the task cannot carry the handler: the signal ends
             * it, as it does when the handler cannot be called at all. */
            task_exit(128 + sig);
            return;
        }

        /* A machine whose tasks are not programs: the handler is an ordinary
         * function of the kernel and runs here, in the task's own context. */
        handler(sig);
        return;
    }
}

int task_wait_any(int *exit_code, k_u32_t timeout_ms)
{
    k_u32_t waited = 0;

    for (;;) {
        k_size_t i;
        bool found = false;
        k_u32_t found_pid = 0;
        int found_code = 0;

        k_port_enter_critical();

        for (i = 0; i < TASK_MAX_TASKS; i++) {
            if (g_tasks[i].in_use &&
                (g_tasks[i].state == TASK_STATE_ZOMBIE || g_tasks[i].state == TASK_STATE_DEAD)) {
                found = true;
                found_pid = g_tasks[i].pid;
                found_code = (int)g_tasks[i].exit_code;
                break;
            }
        }

        k_port_exit_critical();

        if (found) {
            if (exit_code != NULL) {
                *exit_code = found_code;
            }
            task_reap(found_pid);
            return (int)found_pid;
        }

        if (waited >= timeout_ms) {
            return -1;
        }

        k_sched_sleep_ms(1);
        waited++;
    }
}
