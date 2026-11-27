/*
 * Micro OS — signals.
 *
 * A signal is a mark on a task: it is set by the sender, and it is answered
 * in the context of the task itself, at the next point where the task comes
 * back to the kernel or leaves it. The kernel does not run a handler in the
 * middle of another task's stack, and it does not trust a handler of a
 * program: on a machine with ring 3 the port builds a frame on the stack of
 * the program, and the program returns through the restorer of that frame.
 *
 * The numbers are the numbers of POSIX, and the set is small on purpose: the
 * marks that end a task or call a handler are SIGINT, SIGPIPE, SIGTERM and
 * SIGCHLD. SIGSTOP, SIGTSTP and SIGCONT are scheduler operations rather than
 * marks: they cannot be caught, and a stopped task runs only after SIGCONT.
 * SIGPIPE is part of the contract already, because a pipe is the next door of
 * the ABI and a program that writes one has to be able to die of the same
 * signal everywhere.
 */
#ifndef MICRO_OS_KERNEL_SIGNAL_H
#define MICRO_OS_KERNEL_SIGNAL_H

#include "kernel/types.h"

struct task;

#define K_SIGINT 2   /* the interrupt of the terminal: Ctrl+C */
#define K_SIGCONT 18 /* continue a stopped task */
#define K_SIGSTOP 19 /* stop a task; cannot be caught or ignored */
#define K_SIGTSTP 20 /* the suspend of the terminal: Ctrl+Z */
#define K_SIGPIPE 13 /* the reader of a pipe is gone */
#define K_SIGTERM 15 /* ask the task to end */
#define K_SIGCHLD 17 /* a child of the task finished */

typedef void (*task_signal_fn)(int);

/* The dispositions the kernel keeps itself, as the values of POSIX. */
#define K_SIG_DFL ((task_signal_fn)0)
#define K_SIG_IGN ((task_signal_fn)1)
#define K_SIG_ERR ((task_signal_fn) - 1) /* a refused task_signal_set() */

/*
 * Send `sig` to the task `pid`. Only a positive pid names one task: the two
 * meanings of POSIX for 0 and -1 are not offered by this call and are
 * answered EINVAL; task_signal_pgrp() and task_signal_job() are the group
 * doors. A pid that names no live task is ESRCH. STOP, TSTP and CONT change
 * the scheduler state and cannot be caught or ignored.
 * Returns 0, or a negative error code.
 */
int task_signal_send(k_u32_t pid, int sig);

/*
 * Set the disposition of `sig` for the task and answer the previous one, or
 * KERR_INVAL for a signal the system does not know. The state-changing
 * signals STOP, TSTP and CONT have no disposition and answer K_SIG_ERR. A
 * NULL task means the caller.
 */
task_signal_fn task_signal_set(struct task *t, int sig, task_signal_fn handler);

/*
 * Answer the pending signals of the task: a disposition of the kernel (end
 * the task or ignore), a handler that the kernel runs in place on a machine
 * without ring 3, or a handler the port hands to the context of the task.
 * `result` is the value of the system call that is about to return, so that
 * the interrupted call resumes with its own answer; `from_syscall` says
 * whether there is a system call to resume at all.
 *
 * The call may not return: the default action of SIGINT, SIGTERM and SIGPIPE
 * is the end of the task with the status 128 + the number of the signal.
 */
void task_signal_check(struct task *t, k_ssize_t result, bool from_syscall);

/*
 * Is there a pending signal that has to interrupt the task now? A signal
 * whose disposition is the default end of the task, or a handler the task
 * installed, interrupts a wait; an ignored signal and the default SIGCHLD do
 * not.
 */
bool task_signal_interrupts(const struct task *t);

/* The bits of the signals that wait for the task (the test reads them). */
k_u32_t task_signal_pending(const struct task *t);

/*
 * The purpose of the marks and of the dispositions of a forked child. The
 * child inherits what the parent installed; the marks themselves do not
 * travel with the fork, because they belong to the moment of the parent.
 */
void task_signal_inherit(struct task *child, const struct task *parent);

/*
 * The dispositions of a task across an execve (POSIX): a signal that the old
 * program caught goes back to its default action - the handler was code of
 * the image that is gone - and a signal the program ignored stays ignored.
 * The marks that are pending stay for the new program.
 */
void task_signal_reset_on_exec(struct task *t);

/*
 * The mark a finished child leaves on its parent. Called by task_exit; a
 * parent that is gone leaves the mark nowhere.
 */
void task_signal_notify_parent(struct task *self);

#endif /* MICRO_OS_KERNEL_SIGNAL_H */
