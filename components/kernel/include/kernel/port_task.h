/*
 * Micro OS — task related part of the platform port contract.
 *
 * The port owns the execution context of a task:
 *   ESP32 : a FreeRTOS task (xTaskCreateStatic)
 *   host  : a pthread
 */
#ifndef MICRO_OS_KERNEL_PORT_TASK_H
#define MICRO_OS_KERNEL_PORT_TASK_H

#include "kernel/types.h"

struct task;

typedef void (*k_port_task_entry_fn)(void *arg);

/* Stack size of a kernel task, in bytes. */
k_size_t k_port_task_stack_size(void);

/*
 * Create the execution context of a task. The task stays suspended until
 * k_port_task_start() is called.
 */
int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg);

/* Let a suspended task start running. */
void k_port_task_start(struct task *t);

/* Give up the CPU. */
void k_port_task_yield(void);

/*
 * Wait until the calling task is no longer stopped. The kernel calls this
 * after SIGSTOP or SIGTSTP removed the task from its run queue; SIGCONT puts
 * it back and wakes the port's door. A port with one stack of its own may
 * implement this with its scheduler, and a port with threads may wait on a
 * condition. The weak fallback polls in bounded sleeps.
 */
void k_port_task_stop(void);

/* Block the calling task for the given number of milliseconds. */
void k_port_task_sleep_ms(k_u32_t ms);

/*
 * Whether a sleep of this port can end before its deadline, when the task it
 * belongs to is woken by another one.
 *
 * Two ports answer yes for two different reasons. The x86-64 port looks at
 * the state of the sleeping task on its way back from the scheduler and
 * returns as soon as somebody changed it; the host gives its tasks a door of
 * their own (k_port_task_wake below) that cuts the sleep. A port that answers
 * no sleeps for the whole time it was asked for and nothing outside the task
 * can end it (the board in its compatibility mode): the wait of the kernel
 * then looks at its record every ten milliseconds instead of sleeping until
 * the object answers, which is the behaviour of 1.8.
 *
 * The weak default answers no, so that a port written before this call keeps
 * the bounded wait it had.
 */
bool k_port_task_sleep_cuttable(void);

/*
 * Cut the sleep of `t`: the task has just been woken by the kernel and must
 * not sit out the rest of the sleep it is in. The call is a hint - a port
 * whose sleep already ends with the state of the task ignores it - and it is
 * made with no lock of the kernel held, from a task or from an interrupt.
 *
 * A nudge may arrive a moment after the sleep it was meant for has ended (the
 * task was woken, left the port and entered another sleep): the flag of the
 * door is then spent on that sleep, which returns at once and looks at its
 * state again. No caller of a sleep assumes that a sleep is never cut short -
 * every loop of the kernel takes a deadline and a state - so a nudge that
 * costs a turn is cheaper than a flag that must be cleared under a lock the
 * side that sleeps already holds.
 *
 * The host implements it with the condition variable of the task: the sleep
 * of the port wakes at once and the task looks at its state. A port whose
 * tasks are threads of another scheduler and that does not implement the call
 * keeps the weak default, which does nothing.
 */
void k_port_task_wake(struct task *t);

/* Terminate the calling task. Does not return. */
void k_port_task_exit(int code);

/*
 * Hand the handler of `sig` to the context of the task, for a machine that
 * runs the task as a program of ring 3: the interrupted frame of `t` is
 * saved and the handler runs in user space. `result` is the answer of the
 * system call that is about to return, or ignored when `from_syscall` is
 * false (the interruption came from a device).
 *
 * Answers 1 when the handler was entered, 0 when the machine has no user
 * context and the kernel has to run the handler itself, and -1 when the
 * context cannot carry a frame (the task has to end).
 */
int k_port_task_signal_enter(struct task *t, int sig, void (*handler)(int), k_ssize_t result,
                             bool from_syscall);

/*
 * Return from a handler entered by k_port_task_signal_enter(): the frame of
 * the interrupted code is restored. Answers 0, or a negative error when
 * there is no frame to return to. `result` receives the value the interrupted
 * context had in the register of the result - a negative answer of an
 * interrupted call is a real value here, not an error.
 */
int k_port_task_signal_leave(struct task *t, k_ssize_t *result);

/* The task that is executing in the calling context (NULL if there is none). */
struct task *k_port_task_current(void);

#endif /* MICRO_OS_KERNEL_PORT_TASK_H */
