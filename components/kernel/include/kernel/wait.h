/*
 * Micro OS — the channels a task waits on.
 *
 * A task that waits for an event of an object - the bytes of a pipe, a key of
 * the console, a page of a program - does not spin: it is taken out of the
 * run queue by the scheduler and put back when the object says that its state
 * has changed. This file is the seam between the two: the object names a
 * channel (the address of the state that changed, which is a stable name for
 * as long as the object lives) and the waiter is woken by whoever changes it.
 *
 * The wait itself is the scheduler's. A waiter that is woken while it sleeps
 * has its wake tick moved to the present, and the tick of the timer is what
 * runs it again; a waiter with a deadline is woken by the same tick when the
 * deadline passes. Nothing here loops on the processor: a task that waits
 * costs no time until the moment it is runnable again.
 *
 * The channels are a fixed table of the size of the table of the tasks: a
 * waiter is one record and the system has no more waiters than tasks. The
 * broadcast is deliberately coarse - `k_wait_wake()` wakes every waiter of a
 * channel and each of them looks at its own state again - because a machine
 * of this size does not need a list of the waiters of one pipe, and a wakeup
 * that finds nothing to read is a wasted millisecond, not a bug.
 */
#ifndef MICRO_OS_KERNEL_WAIT_H
#define MICRO_OS_KERNEL_WAIT_H

#include "kernel/task.h"
#include "kernel/types.h"

/* The answers of k_wait_block(). */
#define K_WAIT_WOKEN 0    /* the object said that its state changed */
#define K_WAIT_TIMEOUT -1 /* the deadline passed */
#define K_WAIT_SIGNAL -2  /* a signal the task has to answer arrived */

/*
 * The channels of the kernel. A channel is the address of the state a task
 * waits for: the pipe of an end, the ring of the console, the mapping of a
 * page. Two objects that live at the same address at different moments are
 * two channels, and a waiter that is woken by the second one only looks at
 * its state again - which is why a channel does not have to be unregistered.
 */
#define K_WAIT_CHANNEL_PIPES ((const void *)&k_wait_channel_pipes)
extern const char k_wait_channel_pipes;
#define K_WAIT_CHANNEL_CONSOLE ((const void *)&k_wait_channel_console)
extern const char k_wait_channel_console;

/*
 * Wait on `channel` (and on `second`, when it is not NULL) for at most
 * `timeout_ms` milliseconds; a timeout of K_WAIT_INFINITE waits until the
 * object wakes the task. Two channels are enough for every wait of this
 * system: the wait of `poll` is the one that watches a pipe and the console at
 * the same time.
 *
 * The call blocks the calling task in the scheduler, so the processor goes to
 * somebody else and the wait costs nothing. The answer is K_WAIT_WOKEN, or
 * K_WAIT_TIMEOUT, or K_WAIT_SIGNAL when a pending signal that the task has to
 * answer cut the wait (the call of the object then answers EINTR). A caller
 * outside of a task sleeps for the timeout and answers K_WAIT_TIMEOUT: there
 * is no run queue to leave.
 */
#define K_WAIT_INFINITE 0xFFFFFFFFu

int k_wait_block(const void *channel, const void *second, k_u32_t timeout_ms);

/* Wake every waiter of `channel`. Safe from an interrupt and from a task. */
void k_wait_wake(const void *channel);

/* Wake every waiter of the system: the coarse wakeup of a state that is not
 * one object (a program that ended, a table that changed). */
void k_wait_wake_all(void);

/* Forget the waits of a task that is going away: a slot of a task that ended
 * must not wake the next task that takes its number. */
void k_wait_forget(const struct task *t);

/* The number of waiters of a channel, and of the whole system (the tests and
 * /proc read the second one). */
k_size_t k_wait_count(const void *channel);
k_size_t k_wait_total(void);

#endif /* MICRO_OS_KERNEL_WAIT_H */
