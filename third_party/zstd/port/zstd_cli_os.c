/*
 * Micro OS — the few calls of a Unix-like system the zstd CLI makes.
 *
 * The library of a program of this system has the streams, the allocation,
 * `<sys/stat.h>`, `<dirent.h>` and `<libgen.h>`; what it does not have is the
 * part of POSIX that belongs to a process and a machine with users, disks and
 * a terminal. The CLI of zstd asks for five of those calls, and this file
 * answers them the way the machine is, not the way a host would:
 *
 *   sysconf              one processor: the port started one core and the
 *                        bring-up of the second is not written (ADR 0012);
 *   chown / fchown       the one owner of ADR 0005; the call of the kernel
 *                        takes a uid and no gid, and a group does not exist;
 *   fchmod               no call of the ABI: a descriptor knows its position
 *                        and not its name, so the mode of a file is set by its
 *                        path (`chmod` of `<sys/stat.h>`), and this one answers
 *                        the success of a copy that has already done the work
 *                        by name or the refusal of a machine that cannot;
 *   utime                the filesystem keeps the moment of a change and no
 *                        caller of a program may set it (ADR 0013): the call
 *                        answers success, so a copy of a decompressed file
 *                        still happens and its moment is the one of the write.
 *
 * The wrappers keep the names of POSIX and not a prefix: nothing of the
 * library of a program defines them, so the symbols of the image are the ones
 * the release calls. The file is compiled with `ZSTD_CLI_OS_NO_RENAME` off -
 * it does not include the port header of the renames - and it calls the
 * library directly.
 */
#include <errno.h>
#include <sys/stat.h>

#include "utime.h"
#include "zstd_cli_os.h"

/* The types of the two wrappers of the context. */
#include "../lib/zstd.h"

/*
 * This file is compiled with the port header of the CLI, which renames two
 * calls of the library onto the two wrappers of this file: a wrapper that
 * called the renamed name would call itself. The names of the library are
 * restored here for the seven definitions below.
 */
#undef ZSTD_createDCtx
#undef ZSTD_CCtx_setParameter

size_t zstd_port_cstream_in_size(void)
{
    return (size_t)ZSTD_PORT_STREAM_SIZE;
}

size_t zstd_port_cstream_out_size(void)
{
    return (size_t)ZSTD_PORT_STREAM_SIZE;
}

size_t zstd_port_dstream_in_size(void)
{
    return (size_t)ZSTD_PORT_STREAM_SIZE;
}

size_t zstd_port_dstream_out_size(void)
{
    return (size_t)ZSTD_PORT_STREAM_SIZE;
}

/*
 * A decompression context that refuses a frame with a window this machine
 * cannot hold. The release creates the context and lets the frame name its
 * window; the parameter is read when the header of the frame is: a larger one
 * is the sentence "Frame requires too much memory for decoding", which is
 * what the CLI prints, instead of a failed allocation in the middle of a
 * stream.
 */
ZSTD_DCtx *zstd_port_create_dctx(void)
{
    ZSTD_DCtx *dctx = ZSTD_createDCtx();

    if (dctx != NULL) {
        (void)ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, ZSTD_PORT_DECODE_WINDOWLOG);
    }

    return dctx;
}

/*
 * The parameters of a compression context, with the window of this machine
 * where the release means "the default of the level". The CLI sets every
 * parameter of the release (a zero of `--zstd=` is a zero here), the level
 * comes first, and a value the caller named is kept: a person who writes
 * `--zstd=wlog=20` asks for a window this machine may not hold, and the
 * allocation error of the library is the honest answer to that.
 */
size_t zstd_port_cctx_set_parameter(ZSTD_CCtx *cctx, ZSTD_cParameter parameter, int value)
{
    if (value == 0) {
        switch (parameter) {
        case ZSTD_c_windowLog:
            value = ZSTD_PORT_WINDOWLOG;
            break;
        case ZSTD_c_chainLog:
            value = ZSTD_PORT_TABLELOG;
            break;
        case ZSTD_c_hashLog:
            value = ZSTD_PORT_HASHLOG;
            break;
        case ZSTD_c_srcSizeHint:
            /* A stream of an unknown length is the case of a pipe, and the
             * library sizes the whole workspace for the window of the level
             * when it has no hint: two megabytes at level 3. The hint is the
             * assumption of this machine (32 KiB) and not a limit - a longer
             * stream is compressed with the tables and the window of the
             * hint, which is what a program of a megabyte can hold. */
            value = ZSTD_PORT_SRC_SIZE_HINT;
            break;
        default:
            break;
        }
    }

    return ZSTD_CCtx_setParameter(cctx, parameter, value);
}

/*
 * The descriptor of a path in the shape of POSIX: the library of the program
 * answers the negative number of the ABI (the runtime of zstd is built with
 * `-Dopen=ulibc_open_call` for this file to have the name), and the release
 * tests the answer against -1 and hands it to `fdopen`. Without the wrapper a
 * failure of the output file of a stream would reach `fdopen` as a descriptor
 * of -2 and be reported as "invalid argument" instead of the error it was.
 */
int open(const char *path, int flags, ...)
{
    int fd = ulibc_open_call(path, flags);

    if (fd < 0) {
        errno = -fd;
        return -1;
    }

    return fd;
}

/* --- the calls of the process and the machine --- */

long sysconf(int name)
{
    (void)name;

    return 1;
}

int chown(const char *path, uid_t owner, gid_t group)
{
    (void)path;
    (void)owner;
    (void)group;

    /* The one owner of the system (ADR 0005): a copy keeps the owner it was
     * made with, and the call is the success of that. */
    return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
    (void)fd;
    (void)owner;
    (void)group;

    return 0;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;

    /* No call of the ABI sets the mode of an open descriptor; the CLI has
     * already set it by name where it wanted to (`UTIL_chmod`). */
    return 0;
}

int utime(const char *path, const struct utimbuf *times)
{
    (void)path;
    (void)times;

    /* A moment of a file belongs to the filesystem (ADR 0013). */
    return 0;
}
