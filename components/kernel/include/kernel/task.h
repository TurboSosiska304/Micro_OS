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
#include "kernel/vfs.h"

#define TASK_NAME_MAX 16
#define TASK_PRIO_MIN 0
#define TASK_PRIO_MAX 15
#define TASK_PRIO_DEFAULT 5
#define TASK_MAX_TASKS 32
#define TASK_CWD_MAX 128

/* The dispositions a task keeps: the signals of kernel/signal.h and room for
 * the next few. The table is small on purpose, the memory of the board is. */
#define TASK_SIGNALS 8

/*
 * The job of a task. A job is the set of tasks a terminal interrupts
 * together: the stages of a shell pipeline and the programs they started.
 * Zero is no job, and a signal of a job never reaches its owner.
 */
#define TASK_JOB_NONE 0u

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

/*
 * The kinds of terminal a session runs on: the console of the machine and a
 * session of telnet (ADR 0033). Zero is a task that is not a session, which
 * is every task of the system but the ones `who` names.
 */
#define TASK_TTY_NONE 0u
#define TASK_TTY_CONSOLE 1u
#define TASK_TTY_TELNET 2u

/*
 * The terminal of a task (kernel/console.h): NULL for the console of the
 * port, which is what every task has. A session of telnet sets one so that
 * the commands it runs read and write its socket.
 */
struct k_terminal;

/*
 * The maximum number of the open files of a task. Sixteen slots hold the
 * journal and the tables of a program like SQLite, the stages of a pipeline
 * with their redirects, and the files `xargs` keeps open at once, while the
 * memory of the board still allows the table: a slot is one pointer, so the
 * array costs 48 bytes per task on the board and 96 on a 64-bit machine.
 */
#define TASK_MAX_FDS 16

/*
 * The ceiling of the resources of a task. A limit of a fresh task is its
 * ceiling and may only be lowered (kernel/ksys.h has the ABI of the calls).
 * The memory ceiling is the external pool of the board: one task can never
 * own more than the PSRAM, and the internal RAM stays for the kernel.
 * The CPU ceiling is a 32-bit number of milliseconds, which is the width the
 * accounting of a task has; about 49 days of processor time.
 */
#define TASK_LIMIT_MEMORY_MAX (2u * 1024u * 1024u)
#define TASK_LIMIT_CPU_MAX 0xFFFFFFFFu
#define TASK_LIMIT_FDS_MAX TASK_MAX_FDS

/* The length of the path that is kept with an open file. */
#define TASK_PATH_MAX 48

/*
 * One open file of a task: a descriptor is a reference to a description of
 * the kernel (kernel/kfile.h). Two descriptors of one task, and the
 * descriptors of the parent and of the child of a fork, may share one.
 */
struct kfile;
struct k_tty;

struct task {
    k_u32_t pid;
    /*
     * The credentials of the task (ADR 0005, the second half of 1.4): the
     * user it runs as and the group it belongs to. The uid decides the
     * triads of the mode of every object the task touches; uid zero is the
     * root of the system and is the only one that bypasses them. The gid is
     * kept and inherited but no object of the filesystems holds one yet, so
     * the group triad is never the one the VFS reads (docs/debts.md).
     *
     * A fresh task takes both numbers from the task that creates it, and a
     * fork and a program keep them. There is one credential and not the
     * three of POSIX: setuid moves the real and the effective uid together,
     * because the image of a program has no setuid bit to move one alone.
     */
    k_u32_t uid;
    k_u32_t gid;
    /*
     * The session of the task: the kind of terminal it is the session of,
     * and the terminal itself. A task with TASK_TTY_NONE is not a session
     * and `who` does not name it; the terminal is NULL for the console of
     * the port (kernel/console.h), so a task of telnet is the one case with
     * a terminal of its own.
     */
    struct k_terminal *terminal;
    /*
     * The mode of the terminal of that session (kernel/tty.h): made on the
     * first question a program asks of its terminal and freed with the task.
     * A task with no terminal of its own uses the mode of the console and
     * this pointer stays NULL.
     */
    struct k_tty *tty_state;
    k_u8_t tty;
    char name[TASK_NAME_MAX];
    task_state_t state;
    task_state_t wake_state; /* state to restore when a sleeping task wakes up */
    k_u8_t prio;
    /*
     * The core that owns the task. The scheduler writes it when the task is
     * created, when it is woken and when the core it runs on claims it; no
     * other code changes it, and a task is in the run queue of that core and
     * nowhere else (ADR 0012). Zero in a one-core kernel.
     */
    k_u32_t cpu;
    k_u32_t wake_tick; /* tick at which a sleeping task wakes up */
    /*
     * A task that waits with a timeout is transiently sleeping. A block or
     * an unblock that arrives in that moment must not get lost: the request
     * is remembered here and applied when the task wakes up.
     */
    bool pending_block;
    bool pending_wake;
    k_u32_t exit_code;
    k_u64_t runtime_ticks; /* bookkeeping: ticks spent running */
    k_u32_t quantum_left;  /* ticks left in the current time slice */
    void *stack;           /* owned by the port */
    k_size_t stack_size;
    void *ctx; /* port context: FreeRTOS handle / pthread */
    task_entry_fn entry;
    void *arg;
    struct task *parent;
    /*
     * The job of the task (TASK_JOB_NONE when it belongs to none). The
     * stages of a pipeline and the programs they run share one, so that
     * Ctrl+C reaches all of them and not only the one that reads the key.
     */
    k_u32_t job;
    /*
     * True for the child of a fork. In Linux the child knows it by the zero
     * that fork() returns; here the child starts the program from the
     * beginning, so the program has to ask.
     */
    bool is_child;
    /*
     * The signals of the task: the bits that wait for it and the handlers it
     * installed. A zero handler is the default action, one is "ignore",
     * anything else is a function of the task (see kernel/signal.h).
     */
    k_u32_t sig_pending;
    void (*sig_handlers[TASK_SIGNALS])(int);
    struct kfile *files[TASK_MAX_FDS]; /* the open files of the task */
    /*
     * The priority of the memory allocations of the task: the system flag
     * raises it from KMM_PRIORITY_USER to KMM_PRIORITY_SYSTEM.
     */
    k_i8_t mem_priority;
    /*
     * The limits of the task (KSYS_RLIMIT_*). The ceiling of each is in
     * kernel/task.h; a fresh task starts there and can only lower it. The
     * memory is counted for the user blocks the task allocated and has not
     * freed; the CPU time is the ticks the task spent running; the
     * descriptors are the slots of `files` the task may still take.
     */
    k_u64_t limit_memory;
    k_u64_t mem_used;
    k_u32_t limit_cpu_ms;
    k_u32_t cpu_ms;
    k_u64_t cpu_stamp_ms; /* the last moment the CPU time was accounted */
    k_u8_t limit_fds;
    bool cpu_limit_signalled; /* the SIGTERM of the CPU limit went out */
    char cwd[TASK_CWD_MAX];
    bool in_use;
};

/* Create a task. Returns NULL if the table is full or the port refused. */
struct task *task_create(const char *name, task_entry_fn entry, void *arg, k_u8_t prio);

/*
 * The placeholder of a fork on a machine without an MMU: a task that shares
 * the memory of the parent and re-enters its program from the start. The
 * open files are copied before the child can run, so the child holds the
 * ends of a pipe the parent made before the call, exactly as a real fork
 * would leave them.
 */
struct task *task_fork_shared(struct task *parent, const char *name, task_entry_fn entry,
                              void *arg);

/*
 * The shape of a fork on a machine with an address space per task. The record
 * of the child is made here (a pid, the parent, the working directory) and
 * the port is asked to copy the context of the caller into it: the memory,
 * the registers and the point where the system call returns, with the answer
 * zero. The caller gets the child back and its pid through `child`.
 *
 * Returns 0 on success, -1 when the port cannot copy a context (the caller
 * keeps the placeholder of a machine without an MMU), and -2 when there is no
 * room for the child.
 */
int task_fork(struct task *parent, const char *name, struct task **child);

/* Terminate the current task. Does not return. */
void task_exit(int code);

/* The task that is executing right now (NULL outside of a task). */
struct task *task_self(void);

/* Lookups. */
struct task *task_by_pid(k_u32_t pid);

/*
 * The state of a task, read under the section the scheduler uses: the field
 * belongs to the kernel and is changed from another task, so a caller that
 * reads it plainly reads a value somebody is writing. The list of the tasks
 * of `ps` and the waiting of a parent both go through here.
 */
task_state_t task_state_of(const struct task *t);
struct task *task_get_by_index(k_size_t index);
k_size_t task_count(void);
k_size_t task_count_by_state(task_state_t state);

/* Release the slot of a finished task (Linux style reaping). */
int task_reap(k_u32_t pid);

/*
 * Wait for a child task to finish, like waitpid(). The task is reaped and
 * its exit code is stored in exit_code (may be NULL).
 * Returns 0 on success, -1 if there is no such task, -2 on timeout.
 */
int task_wait(k_u32_t pid, int *exit_code, k_u32_t timeout_ms);

/* Wait for any child task to finish. Returns the pid or -1. */
int task_wait_any(int *exit_code, k_u32_t timeout_ms);

/* The priority of the memory allocations of a task (KMM_PRIORITY_*). */
void task_set_mem_priority(struct task *t, int priority);
int task_get_mem_priority(struct task *t);

/*
 * The credentials of a task. The uid is the one the permission checks of the
 * VFS compare with the owner of an object; uid zero is the root and the only
 * value that is allowed to bypass a triad, to change the owner of an object,
 * to mount a volume and to signal a task of another user. The gid is kept
 * with the task and inherited, and it decides nothing yet: no filesystem of
 * this system stores a group (docs/debts.md).
 *
 * A NULL task names the kernel itself, which runs as root: the helpers are
 * called by the boot before a task exists and answer zero there.
 */
k_u32_t task_get_uid(const struct task *t);
k_u32_t task_get_gid(const struct task *t);
void task_set_uid(struct task *t, k_u32_t uid);
void task_set_gid(struct task *t, k_u32_t gid);

/*
 * Move the two numbers together, in one critical section. A login moves a task
 * from one person to another, and a scheduler that ran the task between the
 * two writes would run it with the gid of the new user and the uid of the old
 * one - a state no rule of this system describes. The gid is written first,
 * which is also the order of `setgid` and `setuid` of the ABI: the caller
 * loses the right to change the group with the uid it gives up.
 */
void task_set_creds(struct task *t, k_u32_t uid, k_u32_t gid);
bool task_is_root(const struct task *t);

/*
 * The session of a task: `tty` is TASK_TTY_NONE for a task that is not one,
 * and the name is a word for `who` and /proc/sessions. A session is marked by
 * the task itself when it starts (the server that made it does not know when
 * the child runs).
 */
void task_set_tty(struct task *t, k_u8_t tty);
k_u8_t task_get_tty(const struct task *t);
const char *task_tty_name(k_u8_t tty);

/*
 * A child takes the credentials of its parent. task_create() already copies
 * them from the task that calls it; the explicit call is what the two forks
 * use and what a port of a program inherits through task_fork_shared().
 */
void task_creds_inherit(struct task *child, const struct task *parent);

/*
 * The limits of a task. `which` is KSYS_RLIMIT_MEMORY, KSYS_RLIMIT_CPU or
 * KSYS_RLIMIT_FDS (kernel/ksys.h). get answers the current limit, set lowers
 * it and refuses a raise with KERR_PERM and an unknown resource with
 * KERR_INVAL. A NULL task is a mistake of the caller.
 */
int task_limit_get(struct task *t, k_u32_t which, k_u64_t *out);
int task_limit_set(struct task *t, k_u32_t which, k_u64_t value);

/* The descriptors a task may open: its limit, clamped to the table. */
k_u32_t task_fd_limit(const struct task *t);

/* A child of a fork starts with the limits of its parent. */
void task_limits_inherit(struct task *child, const struct task *parent);

/*
 * Would a block of `size` still fit under the memory limit of the task? The
 * memory manager asks this before it looks for a block, so a task that is
 * over its budget does not spend the memory of the system on the way to a
 * refusal.
 */
bool task_memory_fits(const struct task *t, k_size_t size);

/* Charge or give back the memory of a block to the task that owns it. */
void task_memory_charge(struct task *t, k_size_t size);
void task_memory_uncharge(k_u32_t pid, k_size_t size);

/* Forget the memory the tasks hold: the allocator was re-initialised. */
void task_memory_reset(void);

/*
 * Account the processor time of a task since the last point it was known to
 * be running: a tick of a port that can see the running task, or the edge of
 * a system call. The unit of the limit is the millisecond, and the time
 * between two points is the time the task had the processor - a sleep sets
 * the point again on the way in and out, so a wait costs nothing. When the
 * CPU limit is reached the task is marked with SIGTERM once; the mark is
 * answered at the next system call or, on a machine of ring 3, by the tick
 * itself. Returns true when the mark went out.
 */
bool task_cpu_tick(struct task *t);

/* Working directory of a task. */
/* False when the directory does not fit: a cwd cut in the middle names a
 * directory that is not the one the caller asked for. */
bool task_set_cwd(struct task *t, const char *dir);
const char *task_get_cwd(struct task *t);

/*
 * The job of a task. A child of a fork and a task started with
 * task_fork_shared() inherit the job of its parent, and the program that
 * execve starts inherits it from the stage that asked for it.
 */
void task_set_job(struct task *t, k_u32_t job);
k_u32_t task_get_job(const struct task *t);

/*
 * Send a signal to every living task of a job except the caller, which is
 * the task that reads the console of the pipeline. The answer is 0 when at
 * least one task was named, and ESRCH when the job has no other task.
 */
int task_signal_job(k_u32_t job, int sig);

/*
 * Access to the task table without taking the lock. Only for the internals
 * of the kernel, which already hold it.
 */
struct task *task_get_by_index_unlocked(k_size_t index);

/* Helpers. */
void task_set_name(struct task *t, const char *name);
const char *task_state_name(task_state_t state);

#endif /* MICRO_OS_KERNEL_TASK_H */
