/*
 * Micro OS — the open files of the tasks.
 *
 * The descriptions and the pipes live in two fixed tables and share one
 * recursive mutex: a description is counted and a pipe is a ring of bytes,
 * and both are changed by two tasks when a fork shared them. The mutex is
 * never held while a task sleeps or while a filesystem is asked to close,
 * because those waits can be long and the rest of the table has no reason
 * to wait with them.
 */
#include "kernel/kfile.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "kernel/wait.h"

/* One pipe of the system. */
struct k_pipe {
    bool used;
    k_u16_t readers; /* the descriptions that hold the reading end */
    k_u16_t writers; /* the descriptions that hold the writing end */
    k_u16_t head;    /* the next byte to read, a ring */
    k_u16_t count;   /* the bytes in the buffer */
    k_u8_t buf[KFILE_PIPE_SIZE];
};

static struct kfile g_files[KFILE_MAX];
static struct k_pipe g_pipes[KFILE_PIPE_MAX];
static k_mutex_t g_fd_lock = K_MUTEX_INITIALIZER;

/* --- the table of the descriptions --- */

void kfile_init(void)
{
    if (k_mutex_lock(&g_fd_lock) != 0) {
        return;
    }

    memset(g_files, 0, sizeof(g_files));
    memset(g_pipes, 0, sizeof(g_pipes));

    k_mutex_unlock(&g_fd_lock);
}

struct kfile *kfile_get(struct task *t, k_u32_t fd)
{
    if (t == NULL || fd >= (k_u32_t)TASK_MAX_FDS) {
        return NULL;
    }

    return t->files[fd];
}

int kfile_alloc(struct task *t, struct kfile **out)
{
    struct kfile *f = NULL;
    int fd = -1;
    int limit;
    int i;

    if (t == NULL || out == NULL) {
        return KERR_PERM;
    }

    *out = NULL;

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return KERR_NOMEM;
    }

    /*
     * A descriptor above the limit of the task is not given out, even when
     * the slot of the table is free: the limit is the rule of the task and
     * the table is the machine. The first is EMFILE and the second ENOSPC.
     */
    limit = (int)task_fd_limit(t);
    for (i = 0; i < limit; i++) {
        if (t->files[i] == NULL) {
            fd = i;
            break;
        }
    }

    for (i = 0; i < KFILE_MAX; i++) {
        if (!g_files[i].used) {
            f = &g_files[i];
            break;
        }
    }

    if (fd < 0) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_MFILE;
    }

    if (f == NULL) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_NOSPC;
    }

    memset(f, 0, sizeof(*f));
    f->used = true;
    f->kind = KFILE_FILE;
    f->refs = 1;
    t->files[fd] = f;

    k_mutex_unlock(&g_fd_lock);

    *out = f;

    return fd;
}

void kfile_discard(struct task *t, int fd)
{
    struct kfile *f;

    if (t == NULL || fd < 0 || fd >= TASK_MAX_FDS) {
        return;
    }

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return;
    }

    f = t->files[fd];
    t->files[fd] = NULL;
    if (f != NULL && f->refs == 1) {
        memset(f, 0, sizeof(*f));
    } else if (f != NULL && f->refs > 0) {
        f->refs--;
    }

    k_mutex_unlock(&g_fd_lock);
}

/* --- the pipes --- */

static struct k_pipe *pipe_find_free_locked(void)
{
    int i;

    for (i = 0; i < KFILE_PIPE_MAX; i++) {
        if (!g_pipes[i].used) {
            return &g_pipes[i];
        }
    }

    return NULL;
}

/* The reading end of a description is gone: the writers learn it the next
 * time they look, and the reader of an empty pipe gets the end of the file. */
static void pipe_end_close(struct kfile *f)
{
    struct k_pipe *p = f->pipe;
    bool changed = false;

    if (p == NULL || k_mutex_lock(&g_fd_lock) != 0) {
        return;
    }

    if (p->used) {
        if (f->pipe_write) {
            if (p->writers > 0) {
                p->writers--;
            }
        } else {
            if (p->readers > 0) {
                p->readers--;
            }
        }

        changed = true;

        if (p->readers == 0 && p->writers == 0) {
            memset(p, 0, sizeof(*p));
        }
    }

    k_mutex_unlock(&g_fd_lock);

    /*
     * The end of the file and the broken end are states a waiter is waiting
     * for: a reader that waits for a byte of a pipe whose writer just closed
     * has to see the zero of the end, and a writer whose reader went away has
     * to see EPIPE. The wakeup is outside the lock of the table because it
     * takes the lock of the scheduler.
     */
    if (changed) {
        k_wait_wake(K_WAIT_CHANNEL_PIPES);
    }
}

/* Take `n` bytes out of the ring into `out`; the caller holds the lock. */
static void pipe_take_locked(struct k_pipe *p, k_u8_t *out, k_size_t n)
{
    k_size_t first = K_MIN(n, (k_size_t)(KFILE_PIPE_SIZE - p->head));

    memcpy(out, p->buf + p->head, first);
    if (n > first) {
        memcpy(out + first, p->buf, n - first);
    }

    p->head = (k_u16_t)((p->head + n) % KFILE_PIPE_SIZE);
    p->count = (k_u16_t)(p->count - n);
}

/* Put `n` bytes into the ring; the caller holds the lock and has the room. */
static void pipe_put_locked(struct k_pipe *p, const k_u8_t *in, k_size_t n)
{
    k_u16_t tail = (k_u16_t)((p->head + p->count) % KFILE_PIPE_SIZE);
    k_size_t first = K_MIN(n, (k_size_t)(KFILE_PIPE_SIZE - tail));

    memcpy(p->buf + tail, in, first);
    if (n > first) {
        memcpy(p->buf, in + first, n - first);
    }

    p->count = (k_u16_t)(p->count + n);
}

static k_ssize_t pipe_read(struct kfile *f, void *buf, k_size_t count)
{
    struct k_pipe *p = f->pipe;
    k_u64_t deadline;

    if (p == NULL) {
        return KERR_BADF;
    }

    /*
     * The bound of the wait is one budget of time and not one wakeup: a
     * wakeup that finds the pipe empty again - another reader took the bytes,
     * or the writer woke this one for a byte it did not get - goes back to
     * sleep for what is left of the bound.
     */
    deadline = k_sched_uptime_ms() + KFILE_PIPE_WAIT_MS;

    for (;;) {
        k_size_t take;
        int rc = k_mutex_lock(&g_fd_lock);

        if (rc != 0) {
            return KERR_NOMEM;
        }

        if (!p->used) {
            k_mutex_unlock(&g_fd_lock);
            return KERR_BADF;
        }

        if (p->count > 0) {
            take = K_MIN(count, (k_size_t)p->count);
            pipe_take_locked(p, (k_u8_t *)buf, take);
            k_mutex_unlock(&g_fd_lock);

            /* The room this read made is what a writer waits for. */
            k_wait_wake(K_WAIT_CHANNEL_PIPES);

            return (k_ssize_t)take;
        }

        /*
         * An empty pipe with no writer is the end of the file, the zero of
         * POSIX. A pipe whose writer is still there is not: the reader
         * waits for the bytes, and a wait that lasts too long is EAGAIN,
         * which is not an end and must not be read as one.
         */
        if (p->writers == 0) {
            k_mutex_unlock(&g_fd_lock);
            return 0;
        }

        k_mutex_unlock(&g_fd_lock);

        {
            k_u64_t now = k_sched_uptime_ms();
            int answer;

            if (now >= deadline) {
                return KERR_AGAIN;
            }

            /*
             * The wait is the scheduler's: the task leaves the run queue and
             * the writer wakes it when it puts a byte into the pipe. The bound
             * is the one of the contract, so a reader whose writer is alive
             * but silent answers EAGAIN instead of waiting forever.
             */
            answer = k_wait_block(K_WAIT_CHANNEL_PIPES, NULL, (k_u32_t)(deadline - now));

            if (answer == K_WAIT_SIGNAL) {
                return KERR_INTR;
            }
        }
    }
}

static k_ssize_t pipe_write(struct kfile *f, const void *buf, k_size_t count)
{
    struct k_pipe *p = f->pipe;
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t done = 0;
    k_u64_t deadline;

    if (p == NULL) {
        return KERR_BADF;
    }

    deadline = k_sched_uptime_ms() + KFILE_PIPE_WAIT_MS;

    for (;;) {
        k_size_t room;
        k_size_t put;
        int rc = k_mutex_lock(&g_fd_lock);

        if (rc != 0) {
            return (done > 0) ? (k_ssize_t)done : KERR_NOMEM;
        }

        if (!p->used) {
            k_mutex_unlock(&g_fd_lock);
            return (done > 0) ? (k_ssize_t)done : KERR_BADF;
        }

        if (p->readers == 0) {
            /*
             * Nobody will ever read these bytes. There are no signals in
             * this system, so a write past a closed reader is EPIPE and
             * not a SIGPIPE; the bytes before it are already in the pipe
             * and are the answer.
             */
            k_mutex_unlock(&g_fd_lock);
            return (done > 0) ? (k_ssize_t)done : KERR_PIPE;
        }

        room = (k_size_t)(KFILE_PIPE_SIZE - p->count);
        put = K_MIN(count - done, room);
        if (put > 0) {
            pipe_put_locked(p, in + done, put);
            done += put;
        }

        k_mutex_unlock(&g_fd_lock);

        if (done > 0) {
            /* The bytes are in the ring: a reader that waits may take them. */
            k_wait_wake(K_WAIT_CHANNEL_PIPES);
        }

        if (done == count) {
            return (k_ssize_t)done;
        }

        /* The pipe is full: wait for the reader to make room, inside the one
         * budget of the wait. */
        {
            k_u64_t now = k_sched_uptime_ms();
            int answer;

            if (now >= deadline) {
                return (done > 0) ? (k_ssize_t)done : KERR_AGAIN;
            }

            answer = k_wait_block(K_WAIT_CHANNEL_PIPES, NULL, (k_u32_t)(deadline - now));

            if (answer == K_WAIT_SIGNAL) {
                return (done > 0) ? (k_ssize_t)done : KERR_INTR;
            }
        }
    }
}

/*
 * The state of an object for `poll`. A regular file is always ready: this
 * system has no file whose read would block, and a write of a file is a
 * write of the filesystem. A pipe answers the three bits that matter: a byte
 * to read or the end of the file, room to write, and the error of an end that
 * is gone.
 */
k_u16_t kfile_poll(struct kfile *f, k_u16_t events)
{
    struct k_pipe *p;
    k_u16_t revents = 0;

    if (f == NULL) {
        return KFILE_POLLERR;
    }

    if (f->kind != KFILE_PIPE) {
        /* A file and a device of the VFS: readable and writable when the
         * access of the descriptor allows it. A device that would block - the
         * console - is reported by the caller of this function, which knows
         * which device it is (kernel/ksys.c, the console of the system). */
        if ((events & KFILE_POLLIN) != 0) {
            revents |= KFILE_POLLIN;
        }

        if ((events & KFILE_POLLOUT) != 0) {
            revents |= KFILE_POLLOUT;
        }

        return revents;
    }

    p = f->pipe;
    if (p == NULL || k_mutex_lock(&g_fd_lock) != 0) {
        return KFILE_POLLERR;
    }

    if (!p->used) {
        k_mutex_unlock(&g_fd_lock);
        return KFILE_POLLERR;
    }

    if (f->pipe_write) {
        if (p->readers == 0) {
            /* A write of this end is EPIPE and will never succeed. */
            revents |= KFILE_POLLERR;
        } else if (p->count < KFILE_PIPE_SIZE) {
            revents |= KFILE_POLLOUT;
        }
    } else {
        if (p->count > 0 || p->writers == 0) {
            /* A byte is there, or the end of the file is: a read answers
             * without waiting in both cases. */
            revents |= KFILE_POLLIN;
        }
    }

    k_mutex_unlock(&g_fd_lock);

    /* Only what was asked for is reported, except the error: a caller that
     * waits for a byte has to learn that the end is gone. */
    revents &= (k_u16_t)(events | KFILE_POLLERR);

    return revents;
}

void kfile_wake(void)
{
    k_wait_wake(K_WAIT_CHANNEL_PIPES);
}

/* --- the descriptors --- */

int kfile_pipe(struct task *t, int fds[2])
{
    struct k_pipe *p;
    struct kfile *r = NULL;
    struct kfile *w = NULL;
    int fd_r = -1;
    int fd_w = -1;
    int i;

    if (t == NULL || fds == NULL) {
        return KERR_PERM;
    }

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return KERR_NOMEM;
    }

    p = pipe_find_free_locked();

    for (i = 0; i < KFILE_MAX; i++) {
        if (g_files[i].used) {
            continue;
        }
        if (r == NULL) {
            r = &g_files[i];
        } else {
            w = &g_files[i];
            break;
        }
    }

    for (i = 0; i < (int)task_fd_limit(t); i++) {
        if (t->files[i] != NULL) {
            continue;
        }
        if (fd_r < 0) {
            fd_r = i;
        } else {
            fd_w = i;
            break;
        }
    }

    if (p == NULL || r == NULL || w == NULL) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_NOSPC;
    }

    if (fd_r < 0 || fd_w < 0) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_MFILE;
    }

    memset(p, 0, sizeof(*p));
    p->used = true;
    p->readers = 1;
    p->writers = 1;

    memset(r, 0, sizeof(*r));
    r->used = true;
    r->kind = KFILE_PIPE;
    r->refs = 1;
    r->flags = VFS_O_RDONLY;
    r->pipe = p;

    memset(w, 0, sizeof(*w));
    w->used = true;
    w->kind = KFILE_PIPE;
    w->refs = 1;
    w->flags = VFS_O_WRONLY;
    w->pipe = p;
    w->pipe_write = true;

    t->files[fd_r] = r;
    t->files[fd_w] = w;

    fds[0] = fd_r;
    fds[1] = fd_w;

    k_mutex_unlock(&g_fd_lock);

    return 0;
}

int kfile_dup2(struct task *t, int oldfd, int newfd)
{
    struct kfile *f;
    struct kfile *old = NULL;
    bool destroy = false;

    if (t == NULL) {
        return KERR_PERM;
    }

    if (oldfd < 0 || oldfd >= TASK_MAX_FDS || newfd < 0 || newfd >= TASK_MAX_FDS) {
        return KERR_BADF;
    }

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return KERR_NOMEM;
    }

    f = t->files[oldfd];
    if (f == NULL) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_BADF;
    }

    if (oldfd == newfd) {
        k_mutex_unlock(&g_fd_lock);
        return newfd;
    }

    /*
     * A number at or above the limit of the task is not a descriptor it may
     * create. `oldfd == newfd` is above this check: a descriptor that was
     * open before the limit went down stays a descriptor, and POSIX lets
     * dup2 of it onto itself succeed.
     */
    if ((k_u32_t)newfd >= task_fd_limit(t)) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_MFILE;
    }

    old = t->files[newfd];
    f->refs++;
    t->files[newfd] = f;

    if (old != NULL) {
        if (old->refs > 0) {
            old->refs--;
        }
        destroy = (old->refs == 0);
    }

    k_mutex_unlock(&g_fd_lock);

    if (destroy) {
        if (old->kind == KFILE_PIPE) {
            pipe_end_close(old);
        } else {
            vfs_close(&old->file);
        }

        if (k_mutex_lock(&g_fd_lock) == 0) {
            memset(old, 0, sizeof(*old));
            k_mutex_unlock(&g_fd_lock);
        }
    }

    return newfd;
}

int kfile_close(struct task *t, int fd)
{
    struct kfile *f;
    bool destroy;

    if (t == NULL || fd < 0 || fd >= TASK_MAX_FDS) {
        return KERR_BADF;
    }

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return KERR_NOMEM;
    }

    f = t->files[fd];
    if (f == NULL) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_BADF;
    }

    t->files[fd] = NULL;
    if (f->refs > 0) {
        f->refs--;
    }
    destroy = (f->refs == 0);

    k_mutex_unlock(&g_fd_lock);

    if (destroy) {
        if (f->kind == KFILE_PIPE) {
            pipe_end_close(f);
        } else {
            vfs_close(&f->file);
        }

        if (k_mutex_lock(&g_fd_lock) == 0) {
            memset(f, 0, sizeof(*f));
            k_mutex_unlock(&g_fd_lock);
        }
    }

    return 0;
}

void kfile_table_copy(struct task *dst, const struct task *src)
{
    int i;

    if (dst == NULL || src == NULL) {
        return;
    }

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return;
    }

    for (i = 0; i < TASK_MAX_FDS; i++) {
        if (src->files[i] != NULL) {
            dst->files[i] = src->files[i];
            dst->files[i]->refs++;
        }
    }

    k_mutex_unlock(&g_fd_lock);
}

void kfile_table_close(struct task *t)
{
    int i;

    if (t == NULL) {
        return;
    }

    for (i = 0; i < TASK_MAX_FDS; i++) {
        if (t->files[i] != NULL) {
            kfile_close(t, i);
        }
    }
}

k_ssize_t kfile_read(struct kfile *f, void *buf, k_size_t count)
{
    if (f == NULL) {
        return KERR_BADF;
    }

    if (f->kind == KFILE_PIPE) {
        return pipe_read(f, buf, count);
    }

    return vfs_read(&f->file, buf, count);
}

k_ssize_t kfile_write(struct kfile *f, const void *buf, k_size_t count)
{
    if (f == NULL) {
        return KERR_BADF;
    }

    if (f->kind == KFILE_PIPE) {
        return pipe_write(f, buf, count);
    }

    return vfs_write(&f->file, buf, count);
}
