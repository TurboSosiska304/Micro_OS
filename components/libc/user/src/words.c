/*
 * Micro OS — the words of a command line, split the way a shell splits them.
 *
 * `popen` and `system` run a command without a shell behind them: this system
 * has no `/bin/sh` (docs/porting-programs.md, docs/debts.md), so the library
 * has to do the one part of a shell the calls cannot live without - turn the
 * text into the vector of words `execve` takes. The rules are the ones of the
 * shell of this machine (ADR 0017): a word ends at a blank, `'...'` and
 * `"..."` keep their blanks and lose their quotes, and `\` takes the meaning
 * of the next byte away. There is no variable, no glob, no `|`, `>` or `;`:
 * a command that needs them is a command this system cannot run yet, and the
 * debt says so.
 *
 * The text is split in place and the words point into it, so a caller copies
 * the command into a buffer of its own first and keeps that buffer alive for
 * the life of the vector. `__ulibc_command_words` is that copy and the split
 * in one call: the vector of one `execve` of the ABI is what it fills.
 */
#include <errno.h>
#include <string.h>

#include "ulibc_internal.h"

int __ulibc_split_words(char *text, char **words, int max)
{
    int count = 0;
    int started = 0;
    char *out = text;

    if (text == NULL || words == NULL || max <= 0) {
        errno = EINVAL;
        return -1;
    }

    for (const char *at = text; *at != '\0';) {
        if (*at == ' ' || *at == '\t') {
            *out++ = '\0';
            started = 0;
            at++;
            continue;
        }

        if (!started) {
            if (count >= max) {
                errno = E2BIG;
                return -1;
            }
            words[count++] = out;
            started = 1;
        }

        if (*at == '\'' || *at == '"') {
            char quote = *at++;

            while (*at != '\0' && *at != quote) {
                *out++ = *at++;
            }
            if (*at == quote) {
                at++;
            }
            continue;
        }

        if (*at == '\\' && at[1] != '\0') {
            at++;
        }

        *out++ = *at++;
    }

    *out = '\0';

    return count;
}

int __ulibc_command_words(const char *command, char *block, char **words, int max)
{
    size_t length;
    int count;

    if (command == NULL || block == NULL || words == NULL || max < 1) {
        errno = EINVAL;
        return -1;
    }

    length = strlen(command);
    if (length >= ULIBC_COMMAND_BYTES) {
        errno = E2BIG;
        return -1;
    }

    memcpy(block, command, length + 1);

    count = __ulibc_split_words(block, words, max);
    if (count < 0) {
        return -1;
    }

    words[count] = NULL;

    return count;
}
