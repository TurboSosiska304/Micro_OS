/*
 * Micro OS — cat on the board (ADR 0018).
 *
 * The same program as ports/port_x86_64/programs/cat.c: it opens every path
 * of its line and copies the bytes to the console in pieces of 512, or copies
 * descriptor 0 when it is given no name. On the board every one of those
 * calls is the door of the port, and a path goes through MicroFS, so the
 * program proves the read side of the machine and not only the write of a
 * line.
 *
 * There is no `int $0x80` here: the library calls the address of
 * port_mox_syscall() that the loader handed to _start, with the same numbers
 * and the same arguments. A file of the RAM volume is a real test of the
 * copy of a path and of the data in the DRAM window.
 */
#include "ulibc.h"

#include "kernel/errno.h"

#define CAT_CHUNK 512

static void complain(const char *path, long error)
{
    printf("cat: %s: error %ld\n", path, error < 0 ? -error : error);
}

/* Write every byte, waiting out a pipe that is full. */
static int write_all(int fd, const char *data, size_t count)
{
    while (count > 0) {
        long wrote = write(fd, data, count);

        if (wrote < 0) {
            if (wrote == KERR_AGAIN) {
                continue;
            }
            return -1;
        }

        if (wrote == 0) {
            return -1;
        }

        data += wrote;
        count -= (size_t)wrote;
    }

    return 0;
}

static int cat_fd(int fd, const char *label)
{
    char buffer[CAT_CHUNK];

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));

        if (got < 0) {
            if (got == KERR_AGAIN) {
                continue;
            }
            complain(label, got);
            return 1;
        }

        if (got == 0) {
            return 0; /* the end of the file, or the last writer gone */
        }

        if (write_all(ULIBC_STDOUT, buffer, (size_t)got) != 0) {
            complain(label, KERR_IO);
            return 1;
        }
    }
}

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    /*
     * No name is the standard input. A name of "-" is the same, as in the
     * cat of every Unix, so a line can put the input among the files.
     */
    if (argc < 2) {
        return cat_fd(ULIBC_STDIN, "(stdin)");
    }

    for (i = 1; i < argc; i++) {
        int fd;
        int rc;

        if (strcmp(argv[i], "-") == 0) {
            if (cat_fd(ULIBC_STDIN, "(stdin)") != 0) {
                status = ULIBC_EXIT_FAILURE;
            }
            continue;
        }

        fd = open(argv[i], ULIBC_O_RDONLY);
        if (fd < 0) {
            complain(argv[i], fd);
            status = ULIBC_EXIT_FAILURE;
            continue;
        }

        rc = cat_fd(fd, argv[i]);
        if (rc != 0) {
            status = ULIBC_EXIT_FAILURE;
        }

        (void)close(fd);
    }

    return status;
}
