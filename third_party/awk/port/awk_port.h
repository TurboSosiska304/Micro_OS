/*
 * Micro OS — the configuration of the One True Awk for this system.
 *
 * The header is force-included into every source of the release (`-include`)
 * and answers the questions the source asks of a C library that this system
 * does not have. Nothing of `third_party/awk` is edited; the file is the port
 * layer, and it is read before the headers of `components/libc/user`.
 *
 * The system is byte-oriented and has one locale, the C one: MB_CUR_MAX is 1,
 * so the UTF-8 paths of the 2024 release (`length`, `substr`, `match`, the
 * case of a letter) work on bytes, which is what POSIX says of awk in the C
 * locale. `mbtowc`/`wctomb` below are the identity of a byte and a wide
 * character for the same reason; see `awk_os.c`.
 *
 * The functions of the C library the release asks for and the library of a
 * program does not have - `strdup`, `strncasecmp`, `putc`, `bsearch`, the
 * random numbers of POSIX, the flag of `fcntl` - are declared and implemented
 * here rather than in `components/libc/user`: only this source asks for them
 * today, and the layer is the place for a function of one program.
 * `docs/debts.md` keeps the row of the move into the library when a second
 * source wants them.
 */
#ifndef MICRO_OS_THIRD_PARTY_AWK_PORT_AWK_PORT_H
#define MICRO_OS_THIRD_PARTY_AWK_PORT_AWK_PORT_H

#include <stddef.h>

/* The library of the program, for the two types the declarations below name:
 * FILE and wchar_t. It is the header every source of the layer includes, and
 * including it here keeps the port to one definition of a stream. */
#include "ulibc.h"

/* One locale, one byte to a character: the C locale of the system. */
#define MB_CUR_MAX 1

/* The release grows its table of open files in steps of FOPEN_MAX; a program
 * of this machine has four descriptors, so eight names room for the three
 * standard streams and five files a script keeps open at once. */
#ifndef FOPEN_MAX
#define FOPEN_MAX 8
#endif

/* The four signals of the ABI (SIGINT, SIGPIPE, SIGTERM, SIGCHLD) are the
 * only ones `signal` accepts. awk installs a handler of SIGFPE to report a
 * division by zero; the machine has no such trap for a program, and the call
 * answers SIG_ERR, which the source ignores. The number is the POSIX one so
 * that the source compiles and reads as the release wrote it. */
#ifndef SIGFPE
#define SIGFPE 8
#endif

/* The flag of `fcntl` awk sets on a file it opened: there is no close-on-exec
 * in the table of a program, and `awk_os.c` accepts the call and does
 * nothing. */
#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

/* --- the functions of the C library that this port answers ---
 *
 * `putc`, `strdup`, `strncasecmp` and `bsearch` are the library of a program
 * (`components/libc/user`) and not this layer: a name has one implementation
 * in the tree, and the program of awk links the file that holds it. What is
 * here is what has no other home - the two calls of a shell, the flag of
 * `fcntl` that this table of four descriptors does not have, the generator of
 * POSIX and the wide character of the C locale.
 */

/* The generator of POSIX (`random`/`srandom`), the one of the C library of a
 * Unix-like system: the answer is 0 .. 2^31-1, the algorithm is the additive
 * one of POSIX, and the same seed gives the same sequence on every run
 * (`rand` of a script is repeatable after `srand(n)`). */
long random(void);
void srandom(unsigned int seed);

/* The call awk makes on a file it opened: the flag is accepted and there is
 * nothing to do (there is no descriptor that survives an exec). */
int fcntl(int descriptor, int command, ...);

/* The call of the shell of a Unix-like system. This machine has no shell and
 * no second process a program can wait for: `system` reports the refusal and
 * answers -1. A command of a pipe asks the `popen` of the library of a
 * program, which answers NULL with `ENOSYS` and gives the -1 of `getline`.
 * The debt is the row of `docs/debts.md`, not a silent success. */
int system(const char *command);

/* A byte and a wide character: the identity of the C locale, for the paths
 * of the release that are compiled but never taken with MB_CUR_MAX 1. */
int mbtowc(wchar_t *wide, const char *text, size_t count);
int wctomb(char *text, wchar_t wide);

#endif /* MICRO_OS_THIRD_PARTY_AWK_PORT_AWK_PORT_H */
