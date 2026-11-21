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
    int i;

    if (t == NULL || out == NULL) {
        return KERR_PERM;
    }

    *out = NULL;

    if (k_mutex_lock(&g_fd_lock) != 0) {
        return KERR_NOMEM;
    }

    for (i = 0; i < TASK_MAX_FDS; i++) {
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

    if (fd < 0 || f == NULL) {
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

        if (p->readers == 0 && p->writers == 0) {
            memset(p, 0, sizeof(*p));
        }
    }

    k_mutex_unlock(&g_fd_lock);
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
    k_u32_t waited = 0;

    if (p == NULL) {
        return KERR_BADF;
    }

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

        if (waited >= KFILE_PIPE_WAIT_MS) {
            return KERR_AGAIN;
        }

        k_sched_sleep_ms(1);
        waited++;
    }
}

static k_ssize_t pipe_write(struct kfile *f, const void *buf, k_size_t count)
{
    struct k_pipe *p = f->pipe;
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t done = 0;
    k_u32_t waited = 0;

    if (p == NULL) {
        return KERR_BADF;
    }

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

        if (done == count) {
            return (k_ssize_t)done;
        }

        /* The pipe is full: wait for the reader to make room. */
        if (waited >= KFILE_PIPE_WAIT_MS) {
            return (done > 0) ? (k_ssize_t)done : KERR_AGAIN;
        }

        k_sched_sleep_ms(1);
        waited++;
    }
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

    for (i = 0; i < TASK_MAX_FDS; i++) {
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

    if (p == NULL || r == NULL || w == NULL || fd_r < 0 || fd_w < 0) {
        k_mutex_unlock(&g_fd_lock);
        return KERR_NOSPC;
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
