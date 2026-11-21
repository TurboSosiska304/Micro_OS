/*
 * Micro OS — cat, the first program of the system written against the library
 * of a program and not against the raw header of the port.
 *
 * It opens every path of its line, copies the bytes to the console in pieces
 * of 512, and closes the descriptor. There is no stdin: a program of this
 * milestone is a task with a table of descriptors of its own, and a descriptor
 * is not inherited from the shell yet (docs/debts.md), so `cat` with no name
 * explains itself instead of waiting for a line nobody can type into it.
 *
 * The program is also the proof of the library: open, read, write, close and
 * printf are the calls of the ABI reached through `int $0x80`, and the status
 * it returns is the status the shell waits for.
 */
#include "ulibc.h"

#include "kernel/errno.h"

#define CAT_CHUNK 512

static void complain(const char *path, long error)
{
    printf("cat: %s: error %ld\n", path, error < 0 ? -error : error);
}

static int cat_one(const char *path)
{
    char buffer[CAT_CHUNK];
    int fd;
    int status = 0;

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        complain(path, fd);
        return 1;
    }

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));

        if (got < 0) {
            complain(path, got);
            status = 1;
            break;
        }

        if (got == 0) {
            break; /* the end of the file is an empty answer */
        }

        if (write(ULIBC_STDOUT, buffer, (size_t)got) != got) {
            complain(path, KERR_IO);
            status = 1;
            break;
        }
    }

    (void)close(fd);

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    if (argc < 2) {
        static const char usage[] = "usage: cat <file>...\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    for (i = 1; i < argc; i++) {
        if (cat_one(argv[i]) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
