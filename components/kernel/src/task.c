#include "kernel/task.h"

#include <string.h>

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

const char *task_state_name(task_state_t state) {
    if ((k_u32_t)state >= K_ARRAY_SIZE(g_state_names)) {
        return "?";
    }
    return g_state_names[state];
}

void task_set_name(struct task *t, const char *name) {
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

/*
 * The trampoline: every task starts here. It marks the task as running and
 * calls the user entry point.
 */
static void task_trampoline(void *arg) {
    struct task *t = (struct task *)arg;

    k_sched_task_running(t);

    if (t->entry != NULL) {
        t->entry(t->arg);
    }

    task_exit(0);
}

struct task *task_create(const char *name, task_entry_fn entry, void *arg, k_u8_t prio) {
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
        task_set_name(t, name);
        g_used++;
    }

    k_port_exit_critical();

    if (t == NULL) {
        kerror("task: the task table is full (%u slots)", (unsigned)TASK_MAX_TASKS);
        return NULL;
    }

    /*
     * The execution context is created suspended, so that the task is
     * registered in the run queue before it can run for the first time.
     */
    if (k_port_task_create(t, task_trampoline, t) != 0) {
        k_port_enter_critical();
        t->in_use = false;
        g_used--;
        k_port_exit_critical();
        kerror("task: the port refused to create '%s'", name != NULL ? name : "?");
        return NULL;
    }

    k_sched_task_ready(t);
    k_port_task_start(t);

    kinfo("task: created '%s' (pid %u, prio %u)", t->name, (unsigned)t->pid, (unsigned)t->prio);

    return t;
}

void task_exit(int code) {
    struct task *self = task_self();

    if (self != NULL) {
        k_port_enter_critical();
        self->exit_code = (k_u32_t)code;
        k_port_exit_critical();

        k_sched_task_terminated(self);

        kinfo("task: '%s' (pid %u) exited with code %d", self->name, (unsigned)self->pid, code);
    }

    k_port_task_exit(code);

    /* Not reached. */
    for (;;) {
    }
}

struct task *task_self(void) {
    return k_port_task_current();
}

struct task *task_by_pid(k_u32_t pid) {
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

struct task *task_get_by_index(k_size_t index) {
    struct task *t = NULL;

    k_port_enter_critical();
    if (index < TASK_MAX_TASKS && g_tasks[index].in_use) {
        t = &g_tasks[index];
    }
    k_port_exit_critical();

    return t;
}

k_size_t task_count(void) {
    k_size_t count;

    k_port_enter_critical();
    count = g_used;
    k_port_exit_critical();

    return count;
}

k_size_t task_count_by_state(task_state_t state) {
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

int task_reap(k_u32_t pid) {
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
