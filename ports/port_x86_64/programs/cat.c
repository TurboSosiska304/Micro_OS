/*
 * Micro OS — cat, the first program of the system written against the library
 * of a program and not against the raw header of the port.
 *
 * It opens every path of its line, copies the bytes to the console in pieces
 * of 512, and closes the descriptor. A line with no name is the input of the
 * program: the bytes of descriptor 0 up to the end of the stream, which is
 * what a filter of a shell is. The shell of this milestone does not pass its
 * table to a program yet (docs/debts.md), so descriptor 0 of a fresh task is
 * open only when the program opened it itself - and then the answer of the
 * call is EBADF and not a hang. The name `-` means the same input, as in the
 * commands of a Unix-like system.
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

static int cat_stream(int fd, const char *name)
{
    char buffer[CAT_CHUNK];

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));

        if (got < 0) {
            complain(name, got);
            return 1;
        }

        if (got == 0) {
            return 0; /* the end of the stream is an empty answer */
        }

        if (write(ULIBC_STDOUT, buffer, (size_t)got) != got) {
            complain(name, KERR_IO);
            return 1;
        }
    }
}

static int cat_one(const char *path)
{
    int status;

    if (strcmp(path, "-") == 0) {
        return cat_stream(ULIBC_STDIN, "stdin");
    }

    {
        int fd = open(path, ULIBC_O_RDONLY);

        if (fd < 0) {
            complain(path, fd);
            return 1;
        }

        status = cat_stream(fd, path);
        (void)close(fd);
    }

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    /*
     * No name is the input of the program and not a usage line: a program
     * that is the second half of a pipe reads descriptor 0, and a program
     * without a name in a line is the one a shell asks for that.
     */
    if (argc < 2) {
        return cat_stream(ULIBC_STDIN, "stdin");
    }

    for (i = 1; i < argc; i++) {
        if (cat_one(argv[i]) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
