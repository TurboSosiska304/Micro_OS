/*
 * Micro OS — the directory of a program and the resolution of its paths.
 *
 * The kernel keeps the working directory of a task (the shell's `cd` moves it
 * and `execve` carries it into the program), but the ABI has no call that
 * moves it from a program: a shell changes the task's field and a program was
 * never given that door. POSIX sources still write `ls a b`, `find .` and the
 * `chdir` of `ls` between two directories, so the library keeps the directory
 * itself and resolves every path of a call against it. The prefix of a
 * relative path is the directory of the program at its start, read once with
 * `KSYS_GETCWD`.
 *
 * The VFS of the system does not fold `.` and `..` inside a path, so the
 * resolver does it: a component `.` is dropped, `..` pops the last one, and an
 * empty component (a doubled or trailing slash) is dropped. The answer is
 * always absolute and fits `KSYS_PATH_MAX`, which is the longest path a call
 * accepts.
 *
 * A program that chdirs and then starts another program through `execve` sees
 * the shell's directory in the child: the kernel copies the cwd of the task
 * and the library of the *new* program starts from it, not from the directory
 * this one moved to. POSIX would carry the moved directory; the ABI cannot,
 * and `docs/porting-programs.md` says so.
 */
#include "ulibc_internal.h"

#include <errno.h>

#define PATH_MAX_BYTES (KSYS_PATH_MAX + 1)
#define PATH_ROTATION 4

static char g_cwd_s[PATH_MAX_BYTES] = "/";

/*
 * The text of the last directories named by an open: the one place where a
 * descriptor of a directory becomes a path again. Four is the depth of the
 * walk of a tree of this system and the number of the descriptors of a task.
 */
#define DIR_MAP 4

struct dir_name {
    int fd;
    char path[PATH_MAX_BYTES];
};

static struct dir_name g_dirs_s[DIR_MAP];
static int g_dirs_ready_s;

static void dir_map_init(void)
{
    if (!g_dirs_ready_s) {
        for (int i = 0; i < DIR_MAP; i++) {
            g_dirs_s[i].fd = -1;
        }
        g_dirs_ready_s = 1;
    }
}

static int g_cwd_ready;
static char g_path_s[PATH_ROTATION][PATH_MAX_BYTES];
static int g_path_next;

/* The directory of the program, read from the task the first time. */
const char *__ulibc_cwd(void)
{
    if (!g_cwd_ready) {
        char buffer[PATH_MAX_BYTES];

        if (__ulibc_syscall6(KSYS_GETCWD, (long)(unsigned long)buffer, (long)sizeof(buffer), 0, 0,
                             0, 0) >= 0) {
            size_t length = strlen(buffer);

            if (length > 0 && length < sizeof(g_cwd_s)) {
                memcpy(g_cwd_s, buffer, length + 1);
            }
        }
        g_cwd_ready = 1;
    }

    return g_cwd_s;
}

void __ulibc_cwd_reset(void)
{
    g_cwd_ready = 0;
}

/*
 * Add a component to the path being built. The root keeps its one slash: a
 * component never adds a second one after it.
 */
static int path_push(char *out, size_t *used, const char *component, size_t length)
{
    if (*used + 1 + length + 1 > PATH_MAX_BYTES) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (!(*used == 1 && out[0] == '/')) {
        out[(*used)++] = '/';
    }
    memcpy(out + *used, component, length);
    *used += length;
    out[*used] = '\0';

    return 0;
}

/* The directory part of the path being built, one component back. */
static void path_pop(char *out, size_t *used)
{
    if (*used <= 1) {
        return;
    }

    while (*used > 1 && out[*used - 1] != '/') {
        (*used)--;
    }
    if (*used > 1) {
        (*used)--;
    }
    out[*used] = '\0';
}

const char *__ulibc_path(const char *path)
{
    char *out;
    const char *start;
    size_t used;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    out = g_path_s[g_path_next];
    g_path_next = (g_path_next + 1) % PATH_ROTATION;

    if (path[0] == '/') {
        out[0] = '\0';
        used = 0;
        start = path;
    } else {
        const char *cwd = __ulibc_cwd();
        size_t length = strlen(cwd);

        if (length >= PATH_MAX_BYTES) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(out, cwd, length + 1);
        used = length;
        start = path;
    }

    while (*start != '\0') {
        const char *component;
        size_t length;

        while (*start == '/') {
            start++;
        }
        component = start;
        while (*start != '\0' && *start != '/') {
            start++;
        }
        length = (size_t)(start - component);

        if (length == 0 || (length == 1 && component[0] == '.')) {
            continue;
        }
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            path_pop(out, &used);
            continue;
        }
        if (path_push(out, &used, component, length) != 0) {
            return NULL;
        }
    }

    if (used == 0) {
        out[0] = '/';
        out[1] = '\0';
    }

    return out;
}

int __ulibc_chdir(const char *path)
{
    const char *resolved;
    struct vfs_stat info;

    resolved = __ulibc_path(path);
    if (resolved == NULL) {
        return -1;
    }

    if (__ulibc_syscall6(KSYS_STAT, (long)(unsigned long)resolved, (long)(unsigned long)&info, 0, 0,
                         0, 0) < 0) {
        return -1;
    }
    if (info.type != 2) {
        errno = ENOTDIR;
        return -1;
    }

    /*
     * The resolved path names a directory, but `info` of a directory is the
     * only proof the call has: the cwd is the path, not the inode, and the
     * library stores the text. The copy is safe because the resolver writes
     * into one of its own buffers and the buffer of the cwd is another.
     */
    if (strlen(resolved) >= PATH_MAX_BYTES) {
        errno = ENAMETOOLONG;
        return -1;
    }
    {
        size_t length = strlen(resolved);

        memcpy(g_cwd_s, resolved, length + 1);
    }
    g_cwd_ready = 1;

    return 0;
}

void __ulibc_dir_remember(int fd, const char *path)
{
    int free_slot = -1;

    if (fd < 0 || path == NULL) {
        return;
    }

    dir_map_init();

    for (int i = 0; i < DIR_MAP; i++) {
        if (g_dirs_s[i].fd == fd) {
            free_slot = i;
            break;
        }
        if (g_dirs_s[i].fd < 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        free_slot = 0; /* the oldest of a full table */
    }

    if (strlen(path) >= PATH_MAX_BYTES) {
        return;
    }

    g_dirs_s[free_slot].fd = fd;
    strcpy(g_dirs_s[free_slot].path, path);
}

void __ulibc_dir_forget(int fd)
{
    dir_map_init();

    for (int i = 0; i < DIR_MAP; i++) {
        if (g_dirs_s[i].fd == fd) {
            g_dirs_s[i].fd = -1;
        }
    }
}

const char *__ulibc_dir_path(int fd)
{
    dir_map_init();

    for (int i = 0; i < DIR_MAP; i++) {
        if (g_dirs_s[i].fd == fd) {
            return g_dirs_s[i].path;
        }
    }

    return NULL;
}
