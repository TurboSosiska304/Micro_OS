/*
 * Micro OS — the resolution of the paths of a program.
 *
 * The kernel keeps the working directory of a task: the shell's `cd` moves it
 * with `task_set_cwd`, the call 48 (`KSYS_CHDIR`) moves it from a program, and
 * `execve` carries it into the program it starts. This file used to keep a
 * copy of the directory in the library, because the ABI had no call that moved
 * one; a child started by `execve` then began in the directory of the task and
 * not in the one the program had moved to. The copy is gone: `__ulibc_cwd()`
 * asks the kernel, `chdir` is the call, and the two cannot disagree.
 *
 * The VFS of the system does not fold `.` and `..` inside a path, so the
 * resolver does it: a component `.` is dropped, `..` pops the last one, and an
 * empty component (a doubled or trailing slash) is dropped. The answer is
 * always absolute and fits `KSYS_PATH_MAX`, which is the longest path a call
 * accepts.
 *
 * On the board the resolver is not used (the port has no program with a
 * directory of its own), but the file is linked there by `rename`, and the
 * functions keep their shape for both machines.
 */
#include "ulibc_internal.h"

#include <errno.h>

#define PATH_MAX_BYTES (KSYS_PATH_MAX + 1)
#define PATH_ROTATION 4

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

static char g_cwd_s[PATH_MAX_BYTES] = "/";
static char g_path_s[PATH_ROTATION][PATH_MAX_BYTES];
static int g_path_next;

/*
 * The directory of the program, read from the task every time: the kernel is
 * the one place that keeps it, and a copy here would be the directory of a
 * moment that `chdir` has moved since. The call is one of the cheap ones - a
 * string of the task's record - and a path resolution spends one of them.
 */
const char *__ulibc_cwd(void)
{
    char buffer[PATH_MAX_BYTES];

    if (__ulibc_syscall6(KSYS_GETCWD, (long)(unsigned long)buffer, (long)sizeof(buffer), 0, 0, 0,
                         0) >= 0) {
        size_t length = strlen(buffer);

        if (length > 0 && length < sizeof(g_cwd_s)) {
            memcpy(g_cwd_s, buffer, length + 1);
        }
    }

    return g_cwd_s;
}

/*
 * The kernel's copy of the directory is moved by the shell and by the call of
 * `chdir`, so a library has nothing of its own to clear.
 */

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

/*
 * Move the directory of the program: the path is resolved against the one the
 * task has, and the kernel moves its record of the task. There is no second
 * copy to keep in step, so `getcwd` and the next relative path see the new
 * directory at once, and so does the child of a `fork` or an `execve`.
 */
int __ulibc_chdir(const char *path)
{
    const char *resolved = __ulibc_path(path);
    long rc;

    if (resolved == NULL) {
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_CHDIR, (long)(unsigned long)resolved, 0, 0, 0, 0, 0);
    if (rc < 0) {
        return (int)__ulibc_result(rc);
    }

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
