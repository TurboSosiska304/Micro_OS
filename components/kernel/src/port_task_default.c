/*
 * Micro OS — the fallback of a port that cannot cut a sleep.
 *
 * kernel/port_task.h grew two calls with the wake that preempts (ADR 0046):
 * a task that is woken must not sit out the rest of the sleep it is in, and a
 * port whose tasks are threads of another scheduler needs a door of its own
 * for that. A port that does not implement the door - the board in its
 * compatibility mode, a port of a machine that looks at the state of its task
 * on the way back from its own scheduler - keeps the wait of 1.8: the sleep
 * runs to its end and the waiter looks at its record again, ten milliseconds
 * at a time.
 *
 * The weak definitions say exactly that, the way cpu_default.c says "one
 * core". A port that implements them overrides them and raises its
 * api_minor.
 */
#include "kernel/port_task.h"

#include "kernel/task.h"

[[gnu::weak]] bool k_port_task_sleep_cuttable(void)
{
    return false;
}

[[gnu::weak]] void k_port_task_wake(struct task *t)
{
    K_UNUSED(t);
}

[[gnu::weak]] void k_port_task_stop(void)
{
    struct task *self = task_self();

    while (self != NULL && task_state_of(self) == TASK_STATE_STOPPED) {
        k_port_task_sleep_ms(10u);
    }
}
