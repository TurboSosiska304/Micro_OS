/*
 * Micro OS — the open files of the tasks.
 *
 * A descriptor is an index into the table of its task. The open file
 * behind it is a description of the kernel, counted by the descriptors
 * that refer to it: `dup2` gives two descriptors of one task the same
 * open file, and a fork gives one to the parent and one to the child.
 * The position of a file and the ends of a pipe are therefore one state,
 * exactly as in POSIX; closing one descriptor does not close the others.
 */
#ifndef MICRO_OS_KERNEL_KFILE_H
#define MICRO_OS_KERNEL_KFILE_H

#include "kernel/task.h"
#include "kernel/vfs.h"

/* The object behind an open file: a file of a filesystem, or one end of a
 * pipe. */
#define KFILE_FILE 0
#define KFILE_PIPE 1

/*
 * The descriptions of the system. The table is fixed, like the table of
 * the tasks: an open file is a small record and a board does not want a
 * heap that fragments for it. Forty descriptions hold the sixteen files of a
 * task that asks for its whole table, the sixteen ends of a pipeline of the
 * shell - the deepest line its parser can hold, one boundary at a time - and
 * the copies a fork adds without a new record.
 */
#define KFILE_MAX 40

/* The pipes of the system and the bytes of one. Sixteen pipes are the
 * deepest pipeline of the shell: every stage after the first is one more
 * boundary, and the table of the descriptions above is sized for all of them.
 *
 * The buffer is the grain of a pipeline: with 256 bytes a producer of a
 * megabyte waited for the reader four thousand times, and a machine with
 * memory has no reason to move its bytes a quarter of a kilobyte at a time.
 * The size is a setting of the port, because the two machines are not alike:
 * the x86-64 machine and the host have 64 KiB (the block one call of the
 * system can carry, KSYS_MAX_IO), and the board has 4 KiB, because its
 * internal RAM is scarce. The memory is not the bss of the kernel: a pipe
 * takes its block from `kmm_alloc` when it is made and gives it back when
 * the last end is closed, so sixteen pipes cost a megabyte of a machine that
 * has one only when sixteen of them are open, and the board pays its 4 KiB
 * per pipe and not 64 KiB when it has two stages.
 */
#ifndef KFILE_PIPE_SIZE
#define KFILE_PIPE_SIZE 4096
#endif
#define KFILE_PIPE_MAX 16

/*
 * The write of at most this many bytes is one step of the ring: no other
 * writer can put a byte between the first and the last of them. The number
 * is the `PIPE_BUF` of the C library (components/libc/user/include/limits.h)
 * and the kernel has no header of the library, so the two are kept equal by
 * this sentence and by the test of the atomic write.
 */
#define KFILE_PIPE_BUF 512u

/* One open file. It is made by `open` or `pipe` and lives until the last
 * descriptor that refers to it is closed. */
struct kfile {
    bool used;
    k_u8_t kind;  /* KFILE_FILE or KFILE_PIPE */
    k_u16_t refs; /* the descriptors that refer to this description */
    int flags;
    char path[TASK_PATH_MAX]; /* the path the file was opened with */
    /*
     * The private data of the filesystem for this open file. The address
     * of the description is stable while it is open, so `file.priv` may
     * point here for as long as the description lives. The buffer holds
     * the structures of the filesystems and is aligned as one of them.
     */
    _Alignas(8) k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file; /* the state of the filesystem */
    struct k_pipe *pipe;  /* the pipe of this end, NULL for a file */
    bool pipe_write;      /* this end writes into the pipe */
};

/*
 * Prepare the table. The boot and the tests call it; the descriptors that
 * were open are forgotten, not closed, so a caller flushes its filesystem
 * first.
 */
void kfile_init(void);

/* The description of a descriptor, NULL when the slot is empty or the
 * number is not one of the table. */
struct kfile *kfile_get(struct task *t, k_u32_t fd);

/*
 * Take a free description and a free descriptor of the task. Returns the
 * number of the descriptor and the description through `out`, or a
 * negative error: KERR_MFILE when the limit of the task is reached, KERR_NOSPC
 * when the table of the descriptions of the system is full. The caller fills
 * the description; `kfile_discard` gives it back when the open fails.
 */
int kfile_alloc(struct task *t, struct kfile **out);

/* Give back a description that was never opened (a failed open). */
void kfile_discard(struct task *t, int fd);

/* Close a descriptor. The open file goes with the last descriptor. */
int kfile_close(struct task *t, int fd);

/*
 * POSIX dup2: the number `newfd` becomes a second descriptor of the open
 * file of `oldfd`. A descriptor that was open at `newfd` is closed first,
 * and `oldfd == newfd` changes nothing. Returns `newfd` or EBADF.
 */
int kfile_dup2(struct task *t, int oldfd, int newfd);

/*
 * Make a pipe: fds[0] reads, fds[1] writes. Both ends are descriptors of
 * the task; the pipe goes when the last of them is closed.
 */
int kfile_pipe(struct task *t, int fds[2]);

/* Read and write through a description, a file of a pipe, as the kind
 * says. The access is checked by the caller, at the edge of the calls. */
k_ssize_t kfile_read(struct kfile *f, void *buf, k_size_t count);
k_ssize_t kfile_write(struct kfile *f, const void *buf, k_size_t count);

/*
 * Give a fresh task the descriptors of a fork: every open file of the
 * parent gets one more reference. `dst` must have an empty table.
 */
void kfile_table_copy(struct task *dst, const struct task *src);

/* Close every descriptor of a task (it is exiting). */
void kfile_table_close(struct task *t);

#endif /* MICRO_OS_KERNEL_KFILE_H */
