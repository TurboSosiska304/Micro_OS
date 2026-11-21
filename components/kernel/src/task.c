#include "kernel/task.h"

#include <string.h>

#include "kernel/kfile.h"
#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/signal.h"

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

void task_set_name(struct task *t, const char *name)
{
    if (t == NULL) {
        return;
    }
    if (name == NULL) {
        t->name[0] = '\0';
        return;
    }
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->name[TASK_NAME_MAX - 1] = '\0';
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

void task_set_cwd(struct task *t, const char *dir)
{
    if (t == NULL) {
        return;
    }

    if (dir == NULL) {
        t->cwd[0] = '\0';
        return;
    }

    strncpy(t->cwd, dir, TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = '\0';
}

const char *task_get_cwd(struct task *t)
{
    if (t == NULL) {
        return NULL;
    }

    return t->cwd;
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
static struct task *task_alloc_slot(const char *name, task_entry_fn entry, void *arg, k_u8_t prio)
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
        memset(t, 0, sizeof(*t));
        t->in_use = true;
        t->pid = g_next_pid++;
        t->state = TASK_STATE_NEW;
        t->prio = prio;
        t->entry = entry;
        t->arg = arg;
        t->stack_size = k_port_task_stack_size();
        t->parent = task_self();
        t->mem_priority = KMM_PRIORITY_USER;
        task_set_cwd(t, "/");
        task_set_name(t, name);
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

    t = task_alloc_slot(name, entry, arg, prio);
    if (t == NULL) {
        kerror("task: the task table is full (%u slots)", (unsigned)TASK_MAX_TASKS);
        return NULL;
    }

    if (inherit != NULL) {
        t->parent = inherit;
        task_set_cwd(t, task_get_cwd(inherit));
        kfile_table_copy(t, inherit);
    }

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

    c = task_alloc_slot(name, NULL, NULL, parent->prio);
    if (c == NULL) {
        return -2;
    }

    c->parent = parent;
    task_set_cwd(c, task_get_cwd(parent));
    kfile_table_copy(c, parent);
    task_signal_inherit(c, parent);

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
        k_port_exit_critical();

        /*
         * The descriptors of the task go with it, as POSIX closes the files
         * of a process that exits. This happens before the task is published
         * as a zombie: from then on its parent may reap the slot, and a
         * descriptor that was not closed would be lost from both tables.
         */
        kfile_table_close(self);

        k_sched_task_terminated(self);

        kinfo("task: '%s' (pid %u) exited with code %d", name, (unsigned)pid, code);
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

/* The signals whose default answer is the end of the task. SIGCHLD is the
 * one whose default is to be ignored, as everywhere else. */
static bool signal_ends_by_default(int sig)
{
    return sig != K_SIGCHLD;
}

bool task_signal_interrupts(const struct task *t)
{
    int slot;

    if (t == NULL || t->sig_pending == 0u) {
        return false;
    }

    for (slot = 0; slot < (int)K_ARRAY_SIZE(t->sig_handlers); slot++) {
        int sig = signal_number(slot);
        task_signal_fn handler;

        if (sig == 0 || (t->sig_pending & signal_bit(sig)) == 0u) {
            continue;
        }

        handler = t->sig_handlers[slot];

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
    return t != NULL ? t->sig_pending : 0u;
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
    t->sig_pending |= signal_bit(sig);
    k_port_exit_critical();

    /* A task that waits for an event or a timer may come back at once: the
     * check of the signals is what its wait and its next call look at. */
    k_sched_unblock(t);

    return 0;
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

    k_port_enter_critical();
    previous = t->sig_handlers[slot];
    t->sig_handlers[slot] = handler;

    /* An ignored signal is thrown away, as POSIX asks: there is no point in
     * waking the task for a mark it will never answer. */
    if (handler == K_SIG_IGN) {
        t->sig_pending &= ~signal_bit(sig);
    }
    k_port_exit_critical();

    return previous;
}

void task_signal_inherit(struct task *child, const struct task *parent)
{
    if (child == NULL || parent == NULL) {
        return;
    }

    k_port_enter_critical();
    memcpy(child->sig_handlers, parent->sig_handlers, sizeof(child->sig_handlers));
    child->sig_pending = 0u;
    k_port_exit_critical();
}

void task_signal_notify_parent(struct task *self)
{
    if (self == NULL || self->parent == NULL) {
        return;
    }

    self->parent->sig_pending |= signal_bit(K_SIGCHLD);
}

void task_signal_check(struct task *t, k_ssize_t result, bool from_syscall)
{
    int slot;

    if (t == NULL || t->sig_pending == 0u) {
        return;
    }

    for (slot = 0; slot < (int)K_ARRAY_SIZE(t->sig_handlers); slot++) {
        int sig = signal_number(slot);
        task_signal_fn handler;
        int rc;

        if (sig == 0 || (t->sig_pending & signal_bit(sig)) == 0u) {
            continue;
        }

        handler = t->sig_handlers[slot];

        k_port_enter_critical();
        t->sig_pending &= ~signal_bit(sig);
        k_port_exit_critical();

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
