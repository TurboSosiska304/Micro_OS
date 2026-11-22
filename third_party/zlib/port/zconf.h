/*
 * Micro OS — the configuration zlib is compiled with on this system.
 *
 * zlib asks the platform which of the headers of a C library it may use and
 * which of its conveniences it has. The tree answers for a program of Micro
 * OS: `components/libc/user/include` carries the shims of `<stdlib.h>`,
 * `<string.h>`, `<stdio.h>`, `<unistd.h>`, `<fcntl.h>` and `<errno.h>`, and
 * the allocator is the arena of the program, not the heap of the kernel.
 *
 * The header is force-included before every source of the library
 * (`-include port/zconf.h`), so the answers are in one place and the upstream
 * `zconf.h` is not edited. A file of the library that is added later gets
 * them without a change.
 *
 * `HAVE_UNISTD_H` makes `z_off_t` the `off_t` of `<sys/types.h>` (a long) and
 * turns on the `read`, `write`, `lseek` and `close` declarations of a file;
 * `HAVE_STDARG_H` is the `va_list` of `gzprintf`. `MY_ZCALLOC` says that the
 * two allocation hooks live in `port/zlib_os.c` and not in `zutil.c`.
 */
#ifndef MICRO_OS_THIRD_PARTY_ZLIB_PORT_ZCONF_H
#define MICRO_OS_THIRD_PARTY_ZLIB_PORT_ZCONF_H

#define HAVE_UNISTD_H 1
#define HAVE_STDARG_H 1
#define MY_ZCALLOC 1

/*
 * The calls of a file are not the shape zlib expects. The library of a
 * program answers the negative error itself (-2 is ENOENT), which is the ABI
 * of this system and the shape its utilities read; zlib, like every program
 * of a Unix-like system, expects -1 and `errno`, and tests `open` and `lseek`
 * against -1. The port maps the two shapes in `port/zlib_os.c`, and the same
 * wrappers wait out the EAGAIN of a pipe whose producer is still there
 * (ADR 0014) instead of reporting it as the end of the stream.
 *
 * The source of the port itself is compiled with ZLIB_OS_NO_RENAME, so its
 * wrappers reach the real calls and not themselves.
 */
#ifndef ZLIB_OS_NO_RENAME
#define open zlib_os_open
#define lseek zlib_os_lseek
#define read zlib_os_read
#define write zlib_os_write
#endif

#endif /* MICRO_OS_THIRD_PARTY_ZLIB_PORT_ZCONF_H */
