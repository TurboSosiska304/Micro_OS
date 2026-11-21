/*
 * Micro OS — echo, the program that writes its arguments.
 *
 * The smallest useful program of the library: it walks the vector the port
 * built, puts one space between the words and a newline after the last one.
 * `-n` leaves the newline out, as the command of the same name does.
 *
 * The environment is not printed: `hello` is the program that shows it,
 * because the entry ABI is the subject there. This one is a line of text and
 * a status, which is what a shell command of the tree needs.
 */
#include "ulibc.h"

int main(int argc, char *const argv[], char *const envp[])
{
    int first = 1;
    int newline = 1;
    int i;

    (void)envp;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        first = 2;
    }

    for (i = first; i < argc; i++) {
        size_t length = strlen(argv[i]);

        if (i > first) {
            (void)write(ULIBC_STDOUT, " ", 1);
        }
        if (write(ULIBC_STDOUT, argv[i], length) != (long)length) {
            return ULIBC_EXIT_FAILURE;
        }
    }

    if (newline && write(ULIBC_STDOUT, "\n", 1) != 1) {
        return ULIBC_EXIT_FAILURE;
    }

    return ULIBC_EXIT_SUCCESS;
}
