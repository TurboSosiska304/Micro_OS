/*
 * Micro OS — the entries of a directory over the call of the kernel.
 *
 * The call `KSYS_READDIR` fills a batch of `struct vfs_dirent`, and one batch
 * is eight names for a descriptor: the loop of a source pays a call per eight
 * entries and not per entry, which is what a walk of `/etc` of the machine of
 * this system costs. The position of a directory is a byte offset and
 * `rewinddir` seeks the descriptor to its start, which is a call the VFS
 * answers for a directory as it does for a file.
 *
 * The file follows `feature/sbase`, which wrote the same layer for the POSIX
 * tools of the tree, and drops `fdopendir` with the registry of the text of a
 * directory that this base has no resolver for: a descriptor of a directory of
 * this system is opened by its path and by nothing else.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define DIR_BATCH 8

struct ulibc_dir {
    int fd;
    size_t count;
    size_t index;
    struct vfs_dirent entries[DIR_BATCH];
    struct dirent current;
};

DIR *opendir(const char *path)
{
    DIR *dir;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(path, ULIBC_O_RDONLY | ULIBC_O_DIRECTORY);
    if (fd < 0) {
        errno = -fd;
        return NULL;
    }

    dir = (DIR *)malloc(sizeof(*dir));
    if (dir == NULL) {
        (void)close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->count = 0;
    dir->index = 0;

    return dir;
}

struct dirent *readdir(DIR *dir)
{
    struct vfs_dirent *entry;
    size_t length;

    if (dir == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (dir->index >= dir->count) {
        long got = ulibc_readdir(dir->fd, dir->entries, sizeof(dir->entries));

        if (got < 0) {
            errno = (int)-got;
            return NULL;
        }
        if (got == 0) {
            return NULL; /* the end of the directory, not an error */
        }

        dir->count = (size_t)got / sizeof(dir->entries[0]);
        dir->index = 0;

        if (dir->count == 0) {
            return NULL;
        }
    }

    entry = &dir->entries[dir->index++];

    length = strlen(entry->name);
    if (length >= sizeof(dir->current.d_name)) {
        length = sizeof(dir->current.d_name) - 1;
    }
    memcpy(dir->current.d_name, entry->name, length);
    dir->current.d_name[length] = '\0';
    dir->current.d_ino = (unsigned long)entry->inode;
    dir->current.d_type = (entry->type == 2) ? DT_DIR : DT_REG;

    return &dir->current;
}

int closedir(DIR *dir)
{
    int rc;

    if (dir == NULL) {
        errno = EINVAL;
        return -1;
    }

    rc = close(dir->fd);
    free(dir);

    if (rc < 0) {
        errno = -rc;
        return -1;
    }

    return 0;
}

void rewinddir(DIR *dir)
{
    if (dir == NULL) {
        return;
    }

    if (lseek(dir->fd, 0, ULIBC_SEEK_SET) >= 0) {
        dir->count = 0;
        dir->index = 0;
    }
}

int dirfd(DIR *dir)
{
    if (dir == NULL) {
        errno = EINVAL;
        return -1;
    }

    return dir->fd;
}
