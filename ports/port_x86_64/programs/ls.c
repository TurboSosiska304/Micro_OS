/*
 * Micro OS — ls, a directory through the readdir call.
 *
 * The program opens each path as a directory (ULIBC_O_DIRECTORY), reads the
 * entries in batches of eight - the size of an entry is a fact of the ABI and
 * not of the library, and the kernel is the one that fills the names - and
 * writes them on one line, two spaces apart. A path is always absolute: the
 * kernel of this system has no relative path, and a name that does not begin
 * at the root is refused by the call before a filesystem sees it.
 *
 * This is the second half of the proof of the library: the first is `cat` and
 * a file, and this one is a directory, which is a different descriptor with
 * a position of its own.
 */
#include "ulibc.h"

#define LS_BATCH 8

static int list_one(const char *path)
{
    struct vfs_dirent entries[LS_BATCH];
    int fd;
    int status = 0;

    fd = open(path, ULIBC_O_RDONLY | ULIBC_O_DIRECTORY);
    if (fd < 0) {
        printf("ls: %s: error %d\n", path, -fd);
        return 1;
    }

    for (;;) {
        long got = readdir(fd, entries, sizeof(entries));
        long count;
        long i;

        if (got < 0) {
            printf("ls: %s: error %ld\n", path, -got);
            status = 1;
            break;
        }

        if (got == 0) {
            break; /* the end of the directory */
        }

        count = got / (long)sizeof(entries[0]);
        for (i = 0; i < count; i++) {
            size_t length = strlen(entries[i].name);

            if (write(ULIBC_STDOUT, entries[i].name, length) != (long)length) {
                status = 1;
                break;
            }
            if (write(ULIBC_STDOUT, "  ", 2) != 2) {
                status = 1;
                break;
            }
        }

        if (status != 0) {
            break;
        }
    }

    (void)close(fd);

    if (status == 0 && write(ULIBC_STDOUT, "\n", 1) != 1) {
        status = 1;
    }

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    if (argc < 2) {
        static const char usage[] = "usage: ls <directory>...\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    for (i = 1; i < argc; i++) {
        if (list_one(argv[i]) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
