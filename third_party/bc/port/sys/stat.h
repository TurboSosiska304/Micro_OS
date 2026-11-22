/*
 * Micro OS — the information about a file, as the release reads it.
 *
 * `src/read.c` of bc includes `<sys/stat.h>` and asks for two facts: the
 * size of the file it opened and whether the object is a directory. This
 * system has no `fstat` in the ABI and no POSIX `struct stat` in the library
 * of a program, so the port answers with the two fields the release reads;
 * `port/bc_os.c` fills them from the position of the end of the descriptor.
 *
 * The header is only in the include path of the bc sources, and only for the
 * program of the machine. The host build of the same sources sees the header
 * of the host (`BC_PORT_HOST`), so a test of the library keeps the C library
 * of the build machine and not this shape.
 */
#ifndef MICRO_OS_BC_PORT_SYS_STAT_H
#define MICRO_OS_BC_PORT_SYS_STAT_H

#ifdef BC_PORT_HOST

#include_next <sys/stat.h>

#else /* BC_PORT_HOST */

#include <stddef.h>

struct stat {
    unsigned int st_mode;
    long st_size;
};

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)

int fstat(int fd, struct stat *info);

#endif /* BC_PORT_HOST */

#endif /* MICRO_OS_BC_PORT_SYS_STAT_H */
