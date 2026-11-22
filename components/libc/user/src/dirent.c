/*
 * Micro OS — the entries of a directory over the call of the kernel.
 *
 * The call `KSYS_READDIR` fills a batch of `struct vfs_dirent`; a program that
 * includes `<dirent.h>` sees the POSIX `DIR` and walks it one entry at a time.
 * The library keeps one batch of eight entries per open directory and refills
 * it when it runs out, so a source pays one call per eight names and the loop
 * it has everywhere is the loop of this system.
 *
 * The position of a directory is a byte offset inside the batch. `rewinddir`
 * seeks the descriptor to its start, which is the call the VFS answers for a
 * directory as it does for a file. `telldir`/`seekdir` are not here: the ABI
 * has no cookie for a directory, and no command of this tree asks for one.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#define DIR_BATCH 8

struct ulibc_dir {
    int fd;
    size_t count;
    size_t index;
    struct vfs_dirent entries[DIR_BATCH];
    struct dirent current;
};

static DIR *dir_new(int fd)
{
    DIR *dir = (DIR *)malloc(sizeof(*dir));

    if (dir == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    dir->fd = fd;
    dir->count = 0;
    dir->index = 0;

    return dir;
}

DIR *opendir(const char *path)
{
    DIR *dir;
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return NULL;
    }

    dir = dir_new(fd);
    if (dir == NULL) {
        (void)close(fd);
    }

    return dir;
}

/*
 * A directory from a descriptor that is already open. The ABI has no call for
 * it and the descriptor knows no name, but the library remembers the text of
 * the directories it opened (the recursive walk of `grep -r` opens one and
 * hands it here); the descriptor becomes the one of the DIR and is closed with
 * it, as POSIX says.
 */
DIR *fdopendir(int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    return dir_new(fd);
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
    dir->current.d_ino = entry->inode;
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
