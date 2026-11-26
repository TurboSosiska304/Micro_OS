/*
 * Micro OS — the clean end of the machine (ADR 0050).
 *
 * The port promises that k_port_power() does not return. The kernel does
 * not take that on faith, and it does not ask the port before the data is
 * safe: every task that may write is asked to end, every disk is flushed,
 * every mount is taken down, and only then does the machine go off.
 *
 * The order is the one of ADR 0050 and it is written in one function so
 * that a reader can check it: a machine that stops without the flush is a
 * machine that lost the last write of a person, and the speed of a shutdown
 * is not worth it (ADR 0041).
 */
#include "kernel/power.h"

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/vfs.h"

/*
 * The state of the machine, as the rest of the kernel reads it. A write is
 * a plain store and a read an atomic load: the flag is asked from many
 * tasks and only the one that runs the shutdown writes it, and a byte of a
 * machine of one core needs no lock for that.
 */
static bool g_ending;

const char *k_power_action_name(enum k_power_action action)
{
    switch (action) {
    case K_POWER_HALT:
        return "halt";
    case K_POWER_OFF:
        return "off";
    case K_POWER_REBOOT:
        return "reboot";
    default:
        return "unknown";
    }
}

bool k_power_ending(void)
{
    return __atomic_load_n(&g_ending, __ATOMIC_RELAXED);
}

int k_power_sync(void)
{
    if (k_blockdev_sync_all() != 0) {
        return KERR_IO;
    }

    return 0;
}

/*
 * Take every mount down, the last one first. A mount that lies below
 * another is never busy then, and the root leaves at the end - the shell
 * of the call is still a task, and a machine that goes off does not need a
 * prompt after it.
 *
 * A filesystem that refuses to leave is a refusal of the whole clean end:
 * its metadata is the last thing that has not reached the medium, and the
 * caller has to say so instead of taking the power down.
 */
static int unmount_all(void)
{
    k_size_t guard = vfs_mount_count();
    int rc = 0;
    int refused = 0;

    while (guard-- > 0) {
        k_size_t count = vfs_mount_count();
        const char *point;

        if (count == 0) {
            break;
        }

        point = vfs_mount_name(count - 1);
        if (point == NULL) {
            break;
        }

        rc = vfs_umount(point);
        if (rc != 0) {
            refused = rc;
            kwarn("power: the mount %s did not leave (rc=%d)", point, rc);
            break;
        }
    }

    return refused;
}

int k_power_shutdown(enum k_power_action action, bool force)
{
    const char *name = k_power_action_name(action);
    struct task *self = task_self();
    k_u32_t waited = 0;
    int sent;
    int rc;

    if (self != NULL && !task_is_root(self)) {
        kerror("power: %s: only the root takes the machine down", name);
        return KERR_PERM;
    }

    if (k_power_ending()) {
        kwarn("power: %s: the machine is already going down", name);
        return KERR_BUSY;
    }

    g_ending = true;

    kinfo("power: the machine goes down: %s", name);

    if (force) {
        kwarn("power: -f: the volumes are not synced and the disks are not flushed");
        k_port_power(action);
        KERNEL_PANIC("the port returned from the power of the machine");
    }

    /*
     * The first flush is asked before a task is touched. A disk that does
     * not answer is the one case the machine has to survive: nothing has
     * been written since the filesystem's last call that returned, no task
     * was signalled and the getty is still there. The second flush, after
     * the tasks, is the one that catches a write of a task that ended.
     */
    kinfo("power: syncing the volumes and flushing %u disk(s)", (unsigned)k_blockdev_count());

    rc = k_power_sync();
    if (rc != 0) {
        kwarn("power: a disk did not answer the flush, the machine stays on");
        g_ending = false;
        return rc;
    }

    kinfo("power: the volumes are synced, the disks are flushed");

    /* No new session from here: the getty of the port reads the flag. */
    kinfo("power: no new session starts, the tasks are asked to end");

    sent = task_signal_all(self, K_SIGTERM);
    if (sent < 0) {
        sent = 0;
    }

    kinfo("power: %d task(s) were asked to end", sent);

    while (waited < K_POWER_TASK_WAIT_MS && task_count_living_except(self) > 0) {
        k_sched_sleep_ms(K_POWER_TASK_STEP_MS);
        waited += K_POWER_TASK_STEP_MS;
    }

    if (task_count_living_except(self) > 0) {
        kwarn("power: %u task(s) did not end in %u ms", (unsigned)task_count_living_except(self),
              (unsigned)K_POWER_TASK_WAIT_MS);
    } else {
        kinfo("power: every task has ended");
    }

    /* The writers are gone: the volumes and the disks are read again. */
    kinfo("power: syncing the volumes and flushing the disks once more");

    rc = k_power_sync();
    if (rc != 0) {
        kerror("power: a disk did not answer the flush, the machine stays on");
        g_ending = false;
        return rc;
    }

    rc = unmount_all();
    if (rc != 0) {
        kerror("power: a mount did not leave, the machine stays on (rc=%d)", rc);
        g_ending = false;
        return rc;
    }

    kinfo("power: the mounts are gone");

    /*
     * The last flush belongs to the unmount: a filesystem writes the last
     * of its metadata in its own `umount`, and a cache of the medium below
     * it may still hold the sectors of that write.
     */
    rc = k_power_sync();
    if (rc != 0) {
        kerror("power: a disk did not answer the last flush, the machine stays on");
        g_ending = false;
        return rc;
    }

    kinfo("power: the machine goes down: %s", name);

    k_port_power(action);

    KERNEL_PANIC("the port returned from the power of the machine");
}

[[noreturn]] void k_reboot(void)
{
    k_port_reset();

    KERNEL_PANIC("the port returned from reset");
}

[[noreturn]] void k_halt(void)
{
    k_port_halt();

    KERNEL_PANIC("the port returned from halt");
}
