/*
 * Micro OS — mmap, the memory a program maps.
 *
 * The program is the gate of the second half of the first lane of 1.6. It
 * reads a file through a mapping instead of a buffer, writes a private page
 * and proves the file did not change, writes a shared page and proves the
 * file did, moves the heap with `brk`, and reads a file larger than the
 * window of the machine through a window of mappings that slides over it.
 *
 * The file of the sliding window is the one the caller names, or a file the
 * program makes: 512 KiB of a pattern, which is more than the window of a
 * program (16 MiB) only when the caller has one of those, so the program
 * prints the size it read and the sum of its bytes and the smoke test of the
 * machine compares the sum with the one of `cat` and `wc -c`. A volume of
 * 5 GiB runs the same program on a file of 64 MiB (`make x86-volume-smoke`).
 *
 * The lines are the answer of the call and not a claim about it: a mapping
 * that read the wrong page, a `msync` that wrote nothing and a heap that grew
 * into the stack all end as a line this program does not print.
 */
#include "ulibc.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MMAP_PAGE 4096u
#define MMAP_WINDOW (256u * 1024u)
#define MMAP_LINE 160

static void say(const char *text)
{
    (void)write(1, text, strlen(text));
}

static void say_number(const char *label, long value)
{
    char line[MMAP_LINE];
    int length = snprintf(line, sizeof(line), "%s%ld\n", label, value);

    if (length > 0) {
        (void)write(1, line, (unsigned long)length);
    }
}

/* The sum of the bytes of a file read through a window of mappings. */
static long sum_through_mmap(int fd, unsigned long size)
{
    unsigned long at = 0;
    long sum = 0;

    while (at < size) {
        unsigned long chunk = size - at;
        unsigned char *window;

        if (chunk > MMAP_WINDOW) {
            chunk = MMAP_WINDOW;
        }

        window = (unsigned char *)mmap(NULL, chunk, PROT_READ, MAP_PRIVATE, fd, (off_t)at);

        if (window == MAP_FAILED) {
            return -1;
        }

        for (unsigned long i = 0; i < chunk; i++) {
            sum += window[i];
        }

        if (munmap(window, chunk) != 0) {
            return -1;
        }

        at += chunk;
    }

    return sum;
}

/* Make a file of `size` bytes of a pattern: a letter of the alphabet that
 * moves with the offset and a newline every 64 bytes. A wrong page is a wrong
 * sum, and a line of the file is text a person can read after the shared
 * write - the smoke test looks at the first line of the file. */
#define MMAP_LINE_BYTES 64u

static int make_file(const char *path, unsigned long size)
{
    unsigned char page[MMAP_PAGE];
    int fd;
    unsigned long written = 0;
    unsigned long i;

    for (i = 0; i < sizeof(page); i++) {
        page[i] = (i % MMAP_LINE_BYTES == MMAP_LINE_BYTES - 1) ? (unsigned char)'\n'
                                                               : (unsigned char)('a' + (i % 26u));
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0) {
        return -1;
    }

    while (written < size) {
        unsigned long chunk = size - written;
        long put;

        if (chunk > sizeof(page)) {
            chunk = sizeof(page);
        }

        put = write(fd, page, chunk);

        if (put <= 0) {
            (void)close(fd);
            return -1;
        }

        written += (unsigned long)put;
    }

    (void)close(fd);

    return 0;
}

int main(int argc, char *const argv[], char *const envp[])
{
    const char *path = "/mm.txt";
    struct stat info;
    unsigned char *mapping;
    unsigned char *anon;
    unsigned char buffer[MMAP_PAGE];
    unsigned long size;
    long sum;
    int fd;

    (void)envp;

    if (argc > 1) {
        path = argv[1];
    }

    /* --- a file through a window of mappings ---------------------------- */

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        if (make_file(path, MMAP_WINDOW * 2) != 0) {
            say("mmap: no file\n");
            return 1;
        }

        fd = open(path, O_RDONLY);

        if (fd < 0) {
            say("mmap: no file\n");
            return 1;
        }
    }

    if (fstat(fd, &info) != 0) {
        say("mmap: no size\n");
        return 1;
    }

    size = (unsigned long)info.st_size;
    say_number("mmap: file bytes ", (long)size);

    sum = sum_through_mmap(fd, size);

    if (sum < 0) {
        say("mmap: the window did not map\n");
        return 1;
    }

    say_number("mmap: sum through mmap ", sum);

    /* The same bytes through a read: the two sums have to be one number. */
    {
        unsigned long at = 0;
        long read_sum = 0;

        while (at < size) {
            long got = read(fd, buffer, sizeof(buffer));

            if (got <= 0) {
                break;
            }

            for (long i = 0; i < got; i++) {
                read_sum += buffer[i];
            }

            at += (unsigned long)got;
        }

        say_number("mmap: sum through read ", read_sum);
    }

    (void)close(fd);

    /* --- a private mapping is a copy ------------------------------------ */

    fd = open(path, O_RDWR);

    if (fd < 0) {
        say("mmap: no file for the private page\n");
        return 1;
    }

    mapping = (unsigned char *)mmap(NULL, MMAP_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

    if (mapping == MAP_FAILED) {
        say("mmap: the private page did not map\n");
        return 1;
    }

    say_number("mmap: the private page begins with ", mapping[0]);
    mapping[0] = 0x5A;

    if (mapping[0] != 0x5A) {
        say("mmap: the private page did not take the write\n");
        return 1;
    }

    if (munmap(mapping, MMAP_PAGE) != 0) {
        say("mmap: the private page did not go\n");
        return 1;
    }

    if (read(fd, buffer, 1) < 0) {
        say("mmap: the file did not read\n");
        return 1;
    }

    (void)lseek(fd, 0, SEEK_SET);

    if (read(fd, buffer, 1) == 1) {
        say_number("mmap: the file still begins with ", buffer[0]);
    }

    /* --- a shared mapping is the file ----------------------------------- */

    mapping = (unsigned char *)mmap(NULL, MMAP_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (mapping == MAP_FAILED) {
        say("mmap: the shared page did not map\n");
        return 1;
    }

    mapping[0] = 'H';
    mapping[1] = 'E';
    mapping[2] = 'L';
    mapping[3] = 'L';
    mapping[4] = 'O';

    if (msync(mapping, MMAP_PAGE) != 0) {
        say("mmap: msync refused the page\n");
        return 1;
    }

    if (munmap(mapping, MMAP_PAGE) != 0) {
        say("mmap: the shared page did not go\n");
        return 1;
    }

    (void)lseek(fd, 0, SEEK_SET);

    if (read(fd, buffer, 5) == 5) {
        char line[32];

        (void)snprintf(line, sizeof(line), "mmap: the file says %.5s\n", buffer);
        say(line);
    }

    (void)close(fd);

    /* --- anonymous memory and the heap ---------------------------------- */

    anon = (unsigned char *)mmap(NULL, MMAP_PAGE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (anon == MAP_FAILED) {
        say("mmap: the anonymous page did not map\n");
        return 1;
    }

    say_number("mmap: the anonymous page begins with ", anon[0]);
    anon[MMAP_PAGE - 1] = 0x7E;

    if (anon[MMAP_PAGE - 1] != 0x7E) {
        say("mmap: the anonymous page did not take the write\n");
        return 1;
    }

    (void)munmap(anon, MMAP_PAGE);

    {
        unsigned char *heap = (unsigned char *)sbrk(0);

        say_number("mmap: the heap begins at ", (long)((unsigned long)heap & 0xFFFFu));

        heap = (unsigned char *)sbrk(MMAP_PAGE * 4);

        if (heap == (unsigned char *)-1) {
            say("mmap: the heap did not grow\n");
            return 1;
        }

        heap[0] = 'h';
        heap[MMAP_PAGE * 4 - 1] = 'e';

        if (heap[0] != 'h' || heap[MMAP_PAGE * 4 - 1] != 'e') {
            say("mmap: the heap did not take the write\n");
            return 1;
        }

        say("mmap: the heap grew by four pages\n");

        /* A heap past the stack of the program is the refusal of the call
         * and not a panic: the answer is ENOMEM and the break stays. */
        if (brk((void *)0x03000000ul) != 0) {
            say_number("mmap: the heap past the stack says ", errno);
        } else {
            say("mmap: the heap grew past the stack\n");
        }

        say("mmap: done\n");
    }

    return 0;
}
