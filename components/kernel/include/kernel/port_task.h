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

/* Block the calling task for the given number of milliseconds. */
void k_port_task_sleep_ms(k_u32_t ms);

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
