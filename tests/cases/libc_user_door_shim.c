/*
 * Micro OS — the door of the library of a program, for the tests on the host.
 *
 * The library of a program reaches the kernel through one door
 * (`src/syscall.c`); the host has no kernel and no vector 128, so this file
 * is the door the functions tested here find. It answers the calls the tests
 * need - the directory of the program, the information about a path, the
 * entries of a directory, the open of a directory - from a small tree of names
 * in read-only memory. That tree is the fixture of the tests of `realpath`
 * and `glob`: a path is asked of the library, the library folds it and asks
 * this file, and the answer is the one a real volume would give.
 *
 * A stream is here as well: `fscanf` and `scanf` read through `fgetc`, and the
 * text the test primes is the file of that stream. The rest of the streams of
 * the library (`src/file.c`) is not linked into this binary, so the one byte a
 * scanner needs of a stream is all this file has to answer.
 *
 * The real door is `components/libc/user/src/syscall.c`, and its crossing is
 * covered by `make x86-smoke`, where a program of the machine calls the
 * kernel.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"

long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);
long __ulibc_result(long result);

const char *__ulibc_path(const char *path);

void libc_user_door_set_input(const char *text);

/* The tree of the fixture. A file has its text, a directory does not; the
 * order of the entries is the order `readdir` answers, which is the order a
 * test of GLOB_NOSORT reads. */
struct shim_node {
    const char *path;
    unsigned char type; /* 1: a file, 2: a directory */
    const char *text;
};

static const struct shim_node g_nodes_s[] = {
    {"/", 2, NULL},
    {"/etc", 2, NULL},
    {"/etc/passwd", 1, "root:0:0:/:/bin/sh\n"},
    {"/etc/setting", 1, "x=1\n"},
    {"/bin", 2, NULL},
    {"/bin/cat", 1, ""},
    {"/bin/ls", 1, ""},
    {"/bin/zcat", 1, ""},
    {"/tmp", 2, NULL},
    {"/tmp/a.c", 1, "a\n"},
    {"/tmp/b.c", 1, "b\n"},
    {"/tmp/ab.txt", 1, "ab\n"},
    {"/tmp/.hidden", 1, "h\n"},
    {NULL, 0, NULL},
};

#define SHIM_OPEN_MAX 8

struct shim_open {
    int used;
    int node;
    size_t index;
};

static struct shim_open g_open_s[SHIM_OPEN_MAX];

/* The words of the program, which `_start` would have set on a machine. */
char **environ;

static int node_find(const char *path)
{
    int at;

    for (at = 0; g_nodes_s[at].path != NULL; at++) {
        if (strcmp(g_nodes_s[at].path, path) == 0) {
            return at;
        }
    }

    return -1;
}

/* The directory a path sits in, in the text of the path: "/tmp/a.c" is a
 * child of "/tmp", and "/etc" and "/bin" are children of "/". */
static int node_parent(const char *path, char *parent, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t length;

    if (slash == NULL) {
        return -1;
    }
    if (slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
        return 0;
    }

    length = (size_t)(slash - path);
    if (length + 1 > size) {
        return -1;
    }
    memcpy(parent, path, length);
    parent[length] = '\0';

    return 0;
}

long __ulibc_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
    }

    return result;
}

/* The clock of the fixture: the tests of `strptime` read the calendar of the
 * formatter and never ask this file for the time. */
long uptime(void)
{
    return 0;
}

/* The four fields of the plain information; the statx adds the two moments
 * and the group, which the fixture keeps as zero. */
static void node_fill(void *plain, void *extended, int node, long *answer)
{
    const struct shim_node *entry = &g_nodes_s[node];
    size_t size = (entry->text != NULL) ? strlen(entry->text) : 0;

    if (plain != NULL) {
        struct vfs_stat *info = (struct vfs_stat *)plain;

        info->inode = (k_u32_t)node + 1;
        info->type = entry->type;
        info->size = (k_u64_t)size;
        info->mode = (entry->type == 2) ? 0755u : 0644u;
        info->owner = 0;
    }
    if (extended != NULL) {
        struct vfs_statx *info = (struct vfs_statx *)extended;

        info->inode = (k_u32_t)node + 1;
        info->type = entry->type;
        info->group = 0;
        info->size = (k_u64_t)size;
        info->mode = (entry->type == 2) ? 0755u : 0644u;
        info->owner = 0;
        info->mtime = 1700000000ull;
        info->ctime = 1700000000ull;
    }

    *answer = 0;
}

long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
    (void)a3;
    (void)a4;
    (void)a5;

    switch (number) {
    case KSYS_GETCWD: {
        char *buffer = (char *)(uintptr_t)a0;
        size_t size = (size_t)a1;

        if (size < 2) {
            return KERR_INVAL;
        }
        buffer[0] = '/';
        buffer[1] = '\0';
        return 1;
    }
    case KSYS_STAT: {
        int node = node_find((const char *)(uintptr_t)a0);
        long answer;

        if (node < 0) {
            return KERR_NOENT;
        }
        node_fill((void *)(uintptr_t)a1, NULL, node, &answer);
        return answer;
    }
    case KSYS_STATX: {
        int node = node_find((const char *)(uintptr_t)a0);
        long answer;

        if (node < 0) {
            return KERR_NOENT;
        }
        node_fill(NULL, (void *)(uintptr_t)a1, node, &answer);
        return answer;
    }
    default:
        return 0;
    }
}

int open(const char *path, int flags, ...)
{
    const char *resolved;
    int node;
    int at;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    resolved = __ulibc_path(path);
    if (resolved == NULL) {
        return -1;
    }

    node = node_find(resolved);
    if (node < 0) {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_DIRECTORY) != 0 && g_nodes_s[node].type != 2) {
        errno = ENOTDIR;
        return -1;
    }

    for (at = 0; at < SHIM_OPEN_MAX; at++) {
        if (!g_open_s[at].used) {
            g_open_s[at].used = 1;
            g_open_s[at].node = node;
            g_open_s[at].index = 0;
            return 3 + at;
        }
    }

    errno = EMFILE;
    return -1;
}

int close(int fd)
{
    if (fd < 3 || fd >= 3 + SHIM_OPEN_MAX || !g_open_s[fd - 3].used) {
        errno = EBADF;
        return -1;
    }

    g_open_s[fd - 3].used = 0;

    return 0;
}

long ulibc_readdir(int fd, struct vfs_dirent *entries, size_t count)
{
    struct shim_open *opened;
    const char *path;
    size_t room;
    size_t used = 0;

    if (fd < 3 || fd >= 3 + SHIM_OPEN_MAX || !g_open_s[fd - 3].used) {
        return KERR_BADF;
    }
    opened = &g_open_s[fd - 3];
    path = g_nodes_s[opened->node].path;
    room = count / sizeof(*entries);

    while (used < room && g_nodes_s[opened->index].path != NULL) {
        char parent[VFS_PATH_MAX];
        const struct shim_node *node = &g_nodes_s[opened->index];

        opened->index++;

        if (node_parent(node->path, parent, sizeof(parent)) != 0 || strcmp(parent, path) != 0) {
            continue;
        }

        memset(&entries[used], 0, sizeof(entries[used]));
        strncpy(entries[used].name, strrchr(node->path, '/') + 1, VFS_NAME_MAX);
        entries[used].inode = (k_u32_t)opened->index;
        entries[used].type = node->type;
        entries[used].size = (node->text != NULL) ? (k_u64_t)strlen(node->text) : 0;
        used++;
    }

    return (long)(used * sizeof(*entries));
}

/*
 * The one stream of this binary. `fscanf` reads through `fgetc`; the text the
 * test primes with `libc_user_door_set_input` is the file under it.
 */
static const char *g_input_s;
static size_t g_input_at_s;
static int g_stdin_mark_s;

FILE *stdin = (FILE *)&g_stdin_mark_s;

void libc_user_door_set_input(const char *text)
{
    g_input_s = text;
    g_input_at_s = 0;
}

int fgetc(FILE *stream)
{
    if (stream != stdin) {
        errno = EBADF;
        return EOF;
    }
    if (g_input_s == NULL || g_input_s[g_input_at_s] == '\0') {
        return EOF;
    }

    return (unsigned char)g_input_s[g_input_at_s++];
}
