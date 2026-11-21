#include "kernel/task.h"

#include <string.h>

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"

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

    k_port_enter_critical();
    memset(t, 0, sizeof(*t));
    if (g_used > 0) {
        g_used--;
    }
    k_port_exit_critical();
}

struct task *task_create(const char *name, task_entry_fn entry, void *arg, k_u8_t prio)
{
    struct task *t;

    t = task_alloc_slot(name, entry, arg, prio);
    if (t == NULL) {
        kerror("task: the task table is full (%u slots)", (unsigned)TASK_MAX_TASKS);
        return NULL;
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

/*
 * The child of a fork on a machine whose port can copy the context of the
 * caller (ADR 0005, K4). The child has an address space of its own: the port
 * copies the tables, marks the pages copy-on-write and makes the context
 * return from the system call with zero. The open files are not copied yet -
 * a program of this milestone opens nothing - and the rest of the record is
 * the one of any task.
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
        k_port_exit_critical();

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
