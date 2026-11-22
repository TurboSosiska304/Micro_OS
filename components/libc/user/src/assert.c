/*
 * Micro OS — the failed assertion of a program.
 *
 * The sentence goes to the console of the errors as a line of the program,
 * then the program ends with the status `SIGABRT` of a Unix-like system. The
 * image has no debugger and no names of its functions in a table, so the line
 * is the expression, the file and the number of the line and nothing more.
 */
#include "ulibc.h"

#include <stdio.h>

void __assert_fail(const char *expression, const char *file, unsigned line, const char *function)
{
    (void)function;

    fprintf(stderr, "assert: %s: %u: %s\n", file, line, expression);
    exit(134);
}
