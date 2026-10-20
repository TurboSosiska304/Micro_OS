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

/* The task that is executing in the calling context (NULL if there is none). */
struct task *k_port_task_current(void);

#endif /* MICRO_OS_KERNEL_PORT_TASK_H */
