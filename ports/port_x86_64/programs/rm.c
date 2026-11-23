/*
 * Micro OS — rm, a name removed through the call of the kernel.
 *
 * Without `-r` the program removes a file and refuses a directory, even an
 * empty one: taking a tree down is the flag that says so. With `-r` a
 * directory is walked one entry at a time - the listing is opened, the first
 * name is read, the descriptor is closed and the name is removed, and the
 * listing begins again - because an entry that goes away moves the ones
 * after it and the position of an open directory would skip a neighbour.
 *
 * A path deeper than RM_DEPTH is refused with a sentence: the stack of a
 * program is 64 KiB and a path is 128 bytes, and running the stack out in
 * the middle of a removal is worse than not starting one.
 */
#include "ulibc.h"

#include <unistd.h>

#define RM_DEPTH 8
#define RM_PATH_MAX 128

static int rm_path(const char *path, int recursive, int depth);

static int rm_children(const char *path, int depth)
{
    char child[RM_PATH_MAX];

    for (;;) {
        struct vfs_dirent entry;
        size_t length = strlen(path);
        long got;
        int fd;

        fd = open(path, ULIBC_O_RDONLY | ULIBC_O_DIRECTORY);
        if (fd < 0) {
            printf("rm: %s: error %d\n", path, -fd);
            return 1;
        }

        got = ulibc_readdir(fd, &entry, sizeof(entry));
        (void)close(fd);

        if (got < 0) {
            printf("rm: %s: error %ld\n", path, -got);
            return 1;
        }

        if (got == 0) {
            return 0; /* nothing left below this directory */
        }

        if (length + 1 + strlen(entry.name) + 1 > sizeof(child)) {
            printf("rm: %s: the path is too long\n", path);
            return 1;
        }

        memcpy(child, path, length);
        child[length] = '/';
        memcpy(child + length + 1, entry.name, strlen(entry.name) + 1);

        if (rm_path(child, 1, depth + 1) != 0) {
            return 1;
        }
    }
}

static int rm_path(const char *path, int recursive, int depth)
{
    struct vfs_stat info;
    int rc;

    rc = ulibc_stat_call(path, &info);
    if (rc < 0) {
        printf("rm: %s: error %d\n", path, -rc);
        return 1;
    }

    if (info.type == 2) {
        if (!recursive) {
            printf("rm: %s: is a directory, use -r\n", path);
            return 1;
        }

        if (depth >= RM_DEPTH) {
            printf("rm: %s: the tree is too deep\n", path);
            return 1;
        }

        if (rm_children(path, depth) != 0) {
            return 1;
        }
    }

    /* A file goes with unlink, a directory with rmdir: the two calls of
     * POSIX, and the filesystem says KERR_ISDIR when the wrong one is used. */
    rc = (info.type == 2) ? rmdir(path) : unlink(path);
    if (rc < 0) {
        printf("rm: %s: error %d\n", path, rc);
        return 1;
    }

    return 0;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int recursive = 0;
    int status = 0;
    int first = 1;
    int i;

    (void)envp;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *letter = argv[first] + 1;

        for (; *letter != '\0'; letter++) {
            if (*letter == 'r' || *letter == 'R') {
                recursive = 1;
            } else {
                printf("usage: rm [-r] <path>...\n");
                return ULIBC_EXIT_FAILURE;
            }
        }
        first++;
    }

    if (first == argc) {
        static const char usage[] = "usage: rm [-r] <path>...\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    for (i = first; i < argc; i++) {
        if (rm_path(argv[i], recursive, 0) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
