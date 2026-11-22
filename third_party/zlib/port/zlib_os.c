/*
 * Micro OS — the two allocation hooks of zlib.
 *
 * zlib does not call `malloc` directly: it asks the stream for a block
 * through `zalloc`/`zfree`, and the library fills the two defaults in with
 * these names. On this system the block has to come from the arena of the
 * program (`components/libc/user/src/malloc.c`): `KSYS_MALLOC` would answer
 * an address of ring 0 that the page tables of the program do not map with
 * the user bit.
 *
 * `MY_ZCALLOC` is set in `port/zconf.h`, so `zutil.c` does not compile its
 * own copies and the library has one allocator. The function casts and the
 * overflow check are the shapes zlib expects of a `zalloc`.
 */
#include "zutil.h"

#include "kernel/errno.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* The four wrappers of `port/zconf.h`. The header renames the calls of the
 * library for every other source of zlib; these are the names it renames them
 * to, and the declarations are here so that the port itself is compiled with
 * the same prototypes. */
int zlib_os_open(const char *path, int flags, ...);
long zlib_os_lseek(int fd, long offset, int whence);
long zlib_os_read(int fd, void *buffer, size_t count);
long zlib_os_write(int fd, const void *buffer, size_t count);

/*
 * The four doors between the POSIX shape zlib expects and the ABI of this
 * system. The library answers the negative error itself; a program of
 * Unix-like systems expects -1 and `errno`, and zlib tests open and lseek
 * against -1. The error is translated here and nowhere else, so the
 * utilities of the tree keep the shape their own code reads.
 *
 * A pipe that has no byte yet answers EAGAIN, which zlib would read as the
 * end of the stream, so read and write wait that condition out and a
 * `cat x.gz | zcat` works.
 */
int zlib_os_open(const char *path, int flags, ...)
{
    int fd = open(path, flags);

    if (fd < 0) {
        errno = -fd;
        return -1;
    }

    return fd;
}

long zlib_os_lseek(int fd, long offset, int whence)
{
    long result = lseek(fd, offset, whence);

    if (result < 0) {
        errno = (int)-result;
        return -1;
    }

    return result;
}

long zlib_os_read(int fd, void *buffer, size_t count)
{
    for (;;) {
        long result = read(fd, buffer, count);

        if (result == KERR_AGAIN) {
            continue;
        }
        if (result < 0) {
            errno = (int)-result;
            return -1;
        }
        return result;
    }
}

long zlib_os_write(int fd, const void *buffer, size_t count)
{
    for (;;) {
        long result = write(fd, buffer, count);

        if (result == KERR_AGAIN) {
            continue;
        }
        if (result < 0) {
            errno = (int)-result;
            return -1;
        }
        return result;
    }
}

voidpf zcalloc(voidpf opaque, unsigned items, unsigned size)
{
    (void)opaque;

    /* The product of two unsigned numbers can wrap, and zlib asks for a
     * count of a window and its size: the wrapped product would be a small
     * block that the next call writes past. */
    if (items != 0 && size > (unsigned)-1 / items) {
        return Z_NULL;
    }

    return (voidpf)calloc((size_t)items, (size_t)size);
}

void zcfree(voidpf opaque, voidpf address)
{
    (void)opaque;

    free(address);
}
