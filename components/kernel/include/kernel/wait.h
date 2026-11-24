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
 * A wait is three calls and the lock of the object held across the first one:
 *
 *     lock(object);
 *     if (the state is already the one I want) { unlock; answer; }
 *     k_wait_arm(channel, second);     // still under the lock
 *     unlock(object);
 *     rc = k_wait_sleep(timeout);      // the scheduler sleeps the task
 *     k_wait_disarm();
 *
 * The arm is what closes the window of 1.8 (docs/debts.md, K15). The writer
 * changes the state under the same lock of the object and wakes the channel
 * after it drops that lock: a wake that comes after the arm finds the record
 * of this task and not nothing, and a wake that comes before the arm has
 * changed a state the look under the lock could not see yet. There is no
 * moment in which a wake is lost, so a wait needs no bound and no chunk: the
 * task sleeps until the object wakes it, the deadline passes, or a signal
 * arrives.
 *
 * The wake itself is immediate. `k_wait_wake()` takes the waiter out of the
 * sleeping table, puts it in the run queue of its core and tells that core
 * (the IPI of ADR 0012/0034 when it is another one); the sleep of the port is
 * cut by `k_port_task_wake()`, so a waiter of the host or of the board does
 * not sit out the rest of a chunk. A port whose sleep already ends when the
 * state of the task changes - x86-64 - needs no door and says so.
 */
#ifndef MICRO_OS_KERNEL_WAIT_H
#define MICRO_OS_KERNEL_WAIT_H

#include "kernel/task.h"
#include "kernel/types.h"

/* The answers of k_wait_sleep(). */
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
 * Register the wait of the calling task on `channel` (and on `second`, when it
 * is not NULL) and forget every wakeup that came before it. The caller holds
 * the lock of the object whose state it has just looked at, and keeps it until
 * the state has been looked at once more - a look before the arm is enough
 * when the writer needs the same lock to change the state, because the two
 * cannot interleave.
 *
 * Two channels are enough for every wait of this system: the wait of `poll`
 * is the one that watches a pipe and the console at the same time.
 *
 * Answers false when there is no task to wait (the boot, a test of the host)
 * or no record free; the caller then looks at its object in a loop or sleeps
 * for its timeout, exactly as a caller outside of a task always did.
 */
bool k_wait_arm(const void *channel, const void *second);

/*
 * Sleep the calling task until the record armed by k_wait_arm() has been
 * woken, the deadline passes, or a pending signal that the task has to answer
 * arrives. The answer is K_WAIT_WOKEN, K_WAIT_TIMEOUT or K_WAIT_SIGNAL (the
 * call of the object then answers EINTR).
 *
 * The call leaves the run queue and costs no processor time; the wake of the
 * object puts the task back in it, and the sleep of the port is cut, so the
 * caller is runnable in microseconds and not at the next tick. A caller
 * outside of a task sleeps for the timeout and answers K_WAIT_TIMEOUT: there
 * is no run queue to leave and nothing to wake.
 *
 * `timeout_ms` is measured from this call and not from the arm: a caller in a
 * loop asks again with the time that is left, which is what the loop of
 * `poll` does with its own deadline.
 */
#define K_WAIT_INFINITE 0xFFFFFFFFu

int k_wait_sleep(k_u32_t timeout_ms);

/*
 * Drop the record of the calling task. A waiter that is woken after this has
 * been armed again by a later wait is woken for that one, which is why the
 * call is the last of the three and not a way to cancel a wait in flight.
 */
void k_wait_disarm(void);

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
