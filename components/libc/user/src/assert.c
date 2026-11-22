/*
 * Micro OS — the failure of an assertion.
 *
 * A program of this system has no signal of the kernel to raise for an abort
 * and no shell above it to print the frame; the sentence names the expression
 * and the file, and the task ends with 255, which is the status the port gives
 * a program that faulted. The function is here and not in `<assert.h>` because
 * a header that defines a function in every file that includes it is a
 * function per file, and the assertion of a library is the same one.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __ulibc_assert_fail(const char *expression, const char *file, unsigned line)
{
    (void)fprintf(stderr, "assertion failed: %s, file %s, line %u\n", expression, file, line);
    abort();
}
