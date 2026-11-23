/*
 * Micro OS — the information about an object in the POSIX structure.
 *
 * The kernel call `statx` answers the inode, the type, the size, the mode, the
 * owner and the two moments; `struct stat` of `<sys/stat.h>` is the same
 * information with the fields and the macros a portable source reads. The
 * filesystem of this system keeps no links, no device numbers and no access
 * time: `lstat` is the same call as `stat`, the access time is the moment of
 * the last change (a read does not change a record), and the device is zero.
 *
 * `fstat` asks the kernel about the name the descriptor was opened with: the
 * descriptor keeps that text, and the answer is the same statx of the object
 * - the mode, the owner, the size and the two moments. A descriptor of a pipe
 * has no name, and the call answers ESPIPE; the library describes it as a
 * stream, which is the fact `tail -f` checks before it sleeps on it.
 */
#include <errno.h>
#include <sys/stat.h>

#include "ulibc_internal.h"

static mode_t stat_type(unsigned char type)
{
    return (type == 2) ? S_IFDIR : S_IFREG;
}

static void stat_fill(struct stat *out, const struct vfs_statx *info)
{
    memset(out, 0, sizeof(*out));

    out->st_dev = 0;
    out->st_ino = (ino_t)info->inode;
    out->st_mode = stat_type(info->type) | (mode_t)(info->mode & 07777u);
    out->st_nlink = (info->type == 2) ? 2 : 1;
    out->st_uid = (uid_t)info->owner;
    out->st_gid = (gid_t)info->owner;
    out->st_rdev = 0;
    out->st_size = (off_t)info->size;
    out->st_blksize = 512;
    out->st_blocks = (blkcnt_t)((info->size + 511u) / 512u);
    out->st_atim.tv_sec = (time_t)info->mtime;
    out->st_atim.tv_nsec = 0;
    out->st_mtim.tv_sec = (time_t)info->mtime;
    out->st_mtim.tv_nsec = 0;
    out->st_ctim.tv_sec = (time_t)info->ctime;
    out->st_ctim.tv_nsec = 0;
}

int stat(const char *path, struct stat *out)
{
    struct vfs_statx info;
    const char *resolved;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    resolved = __ulibc_path(path);
    if (resolved == NULL) {
        return -1;
    }

    {
        long rc = __ulibc_syscall6(KSYS_STATX, (long)(unsigned long)resolved,
                                   (long)(unsigned long)&info, 0, 0, 0, 0);

        if (rc < 0) {
            errno = (int)-rc;
            return -1;
        }
    }

    stat_fill(out, &info);

    return 0;
}

/* The system has no links, so the two calls are one. */
int lstat(const char *path, struct stat *out)
{
    return stat(path, out);
}

int fstat(int fd, struct stat *out)
{
    struct vfs_statx info;
    long rc;

    if (out == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * The descriptor keeps the name it was opened with, and the kernel answers
     * the statx of that name: the mode, the owner and the two moments are the
     * ones of the object and not a guess of 0666. A pipe has no name and
     * answers ESPIPE; it is described from the one fact the library knows
     * about it, that it is a stream with no size.
     */
    rc = __ulibc_syscall6(KSYS_FSTAT, fd, (long)(unsigned long)&info, 0, 0, 0, 0);
    if (rc == 0) {
        stat_fill(out, &info);
        return 0;
    }

    if (-rc == ESPIPE) {
        memset(out, 0, sizeof(*out));
        out->st_nlink = 1;
        out->st_blksize = 512;
        out->st_mode = S_IFIFO;
        return 0;
    }

    errno = (int)-rc;
    return -1;
}

/*
 * The mask of the file mode. The kernel gives a new file the mode of the
 * design and `chmod` changes it; the mask is the number a source reads to
 * print or parse a mode, and 022 is the one of a tree where the owner writes.
 */
static mode_t g_umask_s = 022;

mode_t umask(mode_t mask)
{
    mode_t previous = g_umask_s;

    g_umask_s = mask & 0777u;

    return previous;
}

int chmod(const char *path, mode_t mode)
{
    const char *resolved = __ulibc_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return (int)__ulibc_result(__ulibc_syscall6(KSYS_CHMOD, (long)(unsigned long)resolved,
                                                (long)(mode & 07777u), 0, 0, 0, 0));
}
