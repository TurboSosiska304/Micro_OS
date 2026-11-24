/*
 * Micro OS — the machine that goes down, and comes back.
 *
 * A machine has four states that matter to the people and the tasks of it:
 * it is running, it is stopping, it is off and it has started again. The
 * shell asks for the last three (`shutdown`, `poweroff`, `halt`, `reboot`)
 * and the ABI has a reboot of its own; the panic stops the processor. All
 * of it ends in kernel/power.c and in the one hook of the port,
 * k_port_power().
 *
 * The clean end is not the port's: stopping the tasks that write, syncing
 * the volumes, flushing the cache of every disk and taking the mounts down
 * is the kernel's work and the order of it is ADR 0050. A disk that did not
 * answer the flush keeps the machine alive (ADR 0041: the data is worth
 * more than the speed of a shutdown), and the caller is told why.
 */
#ifndef MICRO_OS_KERNEL_POWER_H
#define MICRO_OS_KERNEL_POWER_H

#include "kernel/port/power.h"
#include "kernel/types.h"

/* How long a task gets to end after the SIGTERM of a shutdown. */
#define K_POWER_TASK_WAIT_MS 2000u

/* The step of the wait above. */
#define K_POWER_TASK_STEP_MS 20u

/* The name of an action for the log: "off", "halt" or "reboot". */
const char *k_power_action_name(enum k_power_action action);

/*
 * True from the moment a shutdown was asked for until it either takes the
 * machine down or is refused. The getty of every port reads it before it
 * starts a session, and the shell reads it before it starts a program that
 * is a session of its own: a machine that is going down does not open a new
 * door.
 */
bool k_power_ending(void);

/*
 * The clean end. The order is the one of ADR 0050:
 *
 *   1. no new session (k_power_ending() answers true);
 *   2. a first sync of the volumes and a flush of every disk - asked before
 *      a task is touched, so a disk that refuses the flush costs nothing;
 *   3. SIGTERM to every task but this one, and a bounded wait;
 *   4. the sync and the flush again, because a task may have written in
 *      the step above;
 *   5. every mount is taken down (the last one first, so a mount below is
 *      never busy);
 *   6. the disks are flushed once more and k_port_power(action) is asked.
 *
 * `force` skips the steps 2 to 6 and takes the machine down with a warning:
 * it is the answer for a person who knows that the volume is gone, and no
 * command of the shell makes it the default.
 *
 * A refusal (KERR_IO, KERR_PERM, KERR_BUSY) leaves the machine running and
 * the reason in the log; the stopping state is cleared, so the getty of the
 * port opens its door again. Only root may ask: another uid learns
 * KERR_PERM before a task or a disk is touched.
 */
int k_power_shutdown(enum k_power_action action, bool force);

/*
 * The clean part of the sequence without the tasks and without the last
 * step: the sync of the volumes and the flush of every disk. It is the
 * half a test can call - the last step of a port does not return - and it
 * is what k_power_shutdown() asks twice. Returns 0 or KERR_IO when one of
 * the disks did not answer.
 */
int k_power_sync(void);

/* Restart the board. Does not return. */
_Noreturn void k_reboot(void);

/* Stop the CPU forever. Does not return. */
_Noreturn void k_halt(void);

#endif /* MICRO_OS_KERNEL_POWER_H */
