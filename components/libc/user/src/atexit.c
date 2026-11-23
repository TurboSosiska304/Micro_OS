/*
 * Micro OS — what a program asks to run when it ends.
 *
 * There is no `__cxa_atexit` and no library that ends a program cleanly: the
 * kernel takes the task down at `KSYS_EXIT`. A portable library still has
 * handlers it wants to run at the end (a buffer to flush, a file to close), so
 * the library keeps a short list of them and `exit` of `src/syscall.c` walks it
 * in the reverse order of the registration, as the C library promises.
 *
 * The list is part of the runtime of every program and not of a family of
 * functions a program may not link: a program that never calls `atexit` pays
 * sixteen bytes of bss and nothing else.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdlib.h>

#define ATEXIT_MAX_HANDLERS 8

/* The streams of the library, when it carries them (`src/file.c`): the
 * bytes a program wrote and did not flush leave with the program. The
 * definition is weak in `src/stdio.c`, so a program of the runtime without
 * the streams pays nothing for it. */
void __ulibc_stream_flush_all(void);

static void (*g_atexit_s[ATEXIT_MAX_HANDLERS])(void);
static int g_atexit_count_s;

int atexit(void (*function)(void))
{
    if (function == NULL || g_atexit_count_s >= ATEXIT_MAX_HANDLERS) {
        errno = ENOMEM;
        return -1;
    }

    g_atexit_s[g_atexit_count_s++] = function;

    return 0;
}

void __ulibc_atexit_run(void)
{
    while (g_atexit_count_s > 0) {
        g_atexit_s[--g_atexit_count_s]();
    }

    /*
     * A handler may have written a last line: the streams go after the
     * handlers and not before them, so the order of what the program said
     * is the order of the file.
     */
    __ulibc_stream_flush_all();
}
