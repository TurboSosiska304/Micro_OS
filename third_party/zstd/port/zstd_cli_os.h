/*
 * Micro OS — what the port of the zstd CLI adds to a program of this system.
 *
 * The declarations of the wrappers that `port/zstd_cli_port.h` renames the
 * release onto, and the memory policy of the machine they answer with. The
 * header is included by the port header of the CLI and by the file of the
 * wrappers, which is compiled without the renames: the two halves of the port
 * see the same numbers and the same prototypes.
 */
#ifndef MICRO_OS_ZSTD_CLI_OS_H
#define MICRO_OS_ZSTD_CLI_OS_H

#include <stddef.h>
#include <sys/stat.h>

/*
 * The memory policy of the port, in the numbers of the machine: the arena of
 * a program is at most a megabyte of bss (K_EXEC_BSS_MAX), and the release
 * wants about three at its defaults. The reason of each number is with the
 * call that reads it (`port/zstd_cli_os.c`).
 */
#define ZSTD_PORT_STREAM_SIZE (32u * 1024u)
#define ZSTD_PORT_WINDOWLOG 16
#define ZSTD_PORT_TABLELOG 14
#define ZSTD_PORT_HASHLOG 15
#define ZSTD_PORT_SRC_SIZE_HINT (32u * 1024u)
#define ZSTD_PORT_DECODE_WINDOWLOG 19

/* The size of a buffer of a stream, in the answer of the four calls of the
 * library that the release asks for the recommended one. */
size_t zstd_port_cstream_in_size(void);
size_t zstd_port_cstream_out_size(void);
size_t zstd_port_dstream_in_size(void);
size_t zstd_port_dstream_out_size(void);

/*
 * The call of the library that the build of the program renamed for this
 * port: `-Dopen=ulibc_open_call` is on the runtime of zstd, so the library
 * keeps its raw shape (-ENOENT) and the wrapper below gives the release the
 * shape of POSIX (-1 with `errno`), which `fileio.c` tests and hands to
 * `fdopen`. The wrapper is a definition of `open` and carries the name of the
 * release's call.
 */
int ulibc_open_call(const char *path, int flags, ...);
int open(const char *path, int flags, ...);

/* The calls of a Unix-like system a program of this system does not have. */
long sysconf(int name);
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int fchmod(int fd, mode_t mode);

/* The name of the call of a POSIX machine that asks for the number of
 * processors; the value is the one the release reads, and the answer is 1. */
#define _SC_NPROCESSORS_ONLN 84

/* The mode of a file a decompressor writes, spelled the way the release
 * does (the library of a POSIX host has it in `<sys/stat.h>`). */
#ifndef _S_IWRITE
#define _S_IWRITE 0200
#endif

#endif /* MICRO_OS_ZSTD_CLI_OS_H */
