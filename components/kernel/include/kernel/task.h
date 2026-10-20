/*
 * Micro OS — tasks.
 *
 * A task is the unit of execution. The kernel keeps the metadata (state,
 * priority, PID) in a fixed table; the actual execution context belongs to the
 * platform port (a FreeRTOS task on ESP32, a pthread on the host).
 */
#ifndef MICRO_OS_KERNEL_TASK_H
#define MICRO_OS_KERNEL_TASK_H

#include "kernel/types.h"

#define TASK_NAME_MAX 16
#define TASK_PRIO_MIN 0
#define TASK_PRIO_MAX 15
#define TASK_PRIO_DEFAULT 5
#define TASK_MAX_TASKS 32

typedef enum {
    TASK_STATE_NEW = 0,  /* created, not started yet */
    TASK_STATE_READY,    /* in the run queue */
    TASK_STATE_RUNNING,  /* executing */
    TASK_STATE_SLEEPING, /* waiting for a timer */
    TASK_STATE_BLOCKED,  /* waiting for an event */
    TASK_STATE_ZOMBIE,   /* exited, not reaped yet */
    TASK_STATE_DEAD      /* slot released */
} task_state_t;

typedef void (*task_entry_fn)(void *arg);

struct task {
    k_u32_t pid;
    char name[TASK_NAME_MAX];
    task_state_t state;
    task_state_t wake_state; /* state to restore when a sleeping task wakes up */
    k_u8_t prio;
    k_u32_t wake_tick; /* tick at which a sleeping task wakes up */
    k_u32_t exit_code;
    k_u64_t runtime_ticks; /* bookkeeping: ticks spent running */
    void *stack;           /* owned by the port */
    k_size_t stack_size;
    void *ctx; /* port context: FreeRTOS handle / pthread */
    task_entry_fn entry;
    void *arg;
    struct task *parent;
    bool in_use;
};

/* Create a task. Returns NULL if the table is full or the port refused. */
struct task *task_create(const char *name, task_entry_fn entry, void *arg, k_u8_t prio);

/* Terminate the current task. Does not return. */
void task_exit(int code);

/* The task that is executing right now (NULL outside of a task). */
struct task *task_self(void);

/* Lookups. */
struct task *task_by_pid(k_u32_t pid);
struct task *task_get_by_index(k_size_t index);
k_size_t task_count(void);
k_size_t task_count_by_state(task_state_t state);

/* Release the slot of a finished task (Linux style reaping). */
int task_reap(k_u32_t pid);

/* Helpers. */
void task_set_name(struct task *t, const char *name);
const char *task_state_name(task_state_t state);

#endif /* MICRO_OS_KERNEL_TASK_H */
