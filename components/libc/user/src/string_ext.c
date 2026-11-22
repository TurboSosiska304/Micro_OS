/*
 * Micro OS — the strings a command of a Unix-like system uses.
 *
 * The core of the strings is `src/string.c` and is linked into every program;
 * this file is named by the programs that need a copy of a word in the arena,
 * because a copy is a call of the allocator and a program without one does not
 * pay for it. The file is the one `feature/sbase` wrote for the same reason;
 * the rest of that file (the compare without case, the search of a needle)
 * comes when a program of this tree asks for it.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdlib.h>

char *strdup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        errno = EINVAL;
        return NULL;
    }

    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, text, length);

    return copy;
}
