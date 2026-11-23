/*
 * Micro OS — big, a program of 4 MiB that measures the memory it costs.
 *
 * The image of a program used to be read into the heap of the kernel and
 * copied from there into the frames of the program: a program of 4 MiB cost
 * 4 MiB of the heap *and* 4 MiB of the pages. From 1.6 the image is mapped
 * from the file and the pages come on the fault, so the heap of the kernel
 * does not grow with the program at all.
 *
 * The program is the measurement of that: its bss is 4 MiB, it writes one
 * byte of every page of it (so that every page of the image is a page of the
 * machine), and it prints the free memory of the kernel while it runs. The
 * smoke test reads the same line before the program and after it: a
 * difference of megabytes would be the second copy of the plan of 1.6.
 */
#include "ulibc.h"

#include <fcntl.h>

#define BIG_BYTES (4u * 1024u * 1024u)
#define BIG_PAGE 4096u
#define BIG_LINE 160

/*
 * The image of a program of 4 MiB. The bytes are in the file and not in the
 * bss, because the format of an image caps the zeroed part at
 * K_EXEC_BSS_MAX (1 MiB, a frozen number of the contract): a program of 4 MiB
 * is a program whose data is 4 MiB, which is what the plan of 1.6 measures.
 */
static unsigned char g_big[BIG_BYTES] = {1};

static long parse_kb(const char *line)
{
    long value = 0;
    int seen = 0;

    while (*line != '\0') {
        if (*line >= '0' && *line <= '9') {
            value = value * 10 + (*line - '0');
            seen = 1;
        } else if (seen) {
            break;
        }

        line++;
    }

    return seen ? value : -1;
}

/* The kernel heap line of /proc/meminfo, the number of the free KB. */
static long heap_free_kb(char *buffer, unsigned long size)
{
    int fd = open("/proc/meminfo", O_RDONLY);
    long got;
    long value = -1;

    if (fd < 0) {
        return -1;
    }

    got = read(fd, buffer, size - 1);
    (void)close(fd);

    if (got <= 0) {
        return -1;
    }

    buffer[got] = '\0';
    value = parse_kb(buffer);

    return value;
}

int main(int argc, char *const argv[], char *const envp[])
{
    char line[BIG_LINE];
    unsigned long at;
    long before;
    long during;

    (void)argc;
    (void)argv;
    (void)envp;

    before = heap_free_kb(line, sizeof(line));

    /* Every page of the bss, written: the pages of the image are the pages of
     * the machine, and no one of them was in the heap of the kernel. */
    for (at = 0; at < BIG_BYTES; at += BIG_PAGE) {
        g_big[at] = (unsigned char)(at / BIG_PAGE);
    }

    during = heap_free_kb(line, sizeof(line));

    snprintf(line, sizeof(line), "big: %u bytes, heap free before %ld KB, during %ld KB\n",
             (unsigned)BIG_BYTES, before, during);
    (void)write(1, line, strlen(line));

    /* The last page is the one that proves the whole of the bss is there. */
    if (g_big[BIG_BYTES - BIG_PAGE] != (unsigned char)((BIG_BYTES - BIG_PAGE) / BIG_PAGE)) {
        (void)write(1, "big: the bss did not read back\n", 31);
        return 1;
    }

    (void)write(1, "big: done\n", 10);

    return 0;
}
