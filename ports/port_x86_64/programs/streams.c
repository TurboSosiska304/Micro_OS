/*
 * Micro OS — the streams of the library, as a program of the machine.
 *
 * The two defects of 1.2.0 lived here and neither the host tests nor the
 * smoke could see them, because a stream over a descriptor needs a kernel
 * under it: `fread` after `ungetc` wrote the rest of the block one byte past
 * where the caller kept it, and `fseek(SEEK_CUR)` counted from the descriptor
 * instead of the stream, which the block of read-ahead puts out of step.
 *
 * The buffering of 1.6 is checked here too, because a block between the
 * stream and the kernel is only visible from a program with a kernel under
 * it: a write of a block-buffered stream reaches the file when the block is
 * full or flushed, a line-buffered stream gives the line at the newline, and
 * `exit` gives the last block of the program to the kernel.
 *
 * This program is the test: it reads a file whose first line is known, gives
 * a byte back, reads a word, asks for the position twice with a relative
 * seek between, and writes three files through the three modes. It says
 * `streams: ok` and exits 0 when all of it holds, and names the first thing
 * that did not.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STREAMS_FILE "/etc/setting"
#define STREAM_TEST_FILE "/streams.tmp"
#define STREAM_TEST_BLOCK 4096

/* The size of a file of the volume, through a descriptor of its own: the
 * only way to see what a stream has given to the kernel and what it holds
 * after the file is closed. A file of MicroFS keeps its size in the record
 * and the record is written at the close, so a second opening of a file that
 * is still open reads the size it had before. */
static long file_size(const char *path)
{
    int fd = open(path, ULIBC_O_RDONLY);
    long size;

    if (fd < 0) {
        return -1;
    }

    size = lseek(fd, 0, SEEK_END);
    close(fd);

    return size;
}

/* The position of the descriptor of a stream: the bytes the kernel has. The
 * block of the stream is ahead of this number until a flush gives it away,
 * which is the property of the buffering that a program can see. */
static long stream_position(FILE *stream)
{
    return lseek(fileno(stream), 0, SEEK_CUR);
}

/*
 * A block-buffered stream: the bytes of a write that does not fill the block
 * stay in the arena of the program, and the file is empty until `fflush` or
 * the block itself. This is the property that turns a filter from a call of
 * the kernel per line into a call per block.
 */
static int check_block_buffering(void)
{
    FILE *file = fopen(STREAM_TEST_FILE, "w");
    char block[STREAM_TEST_BLOCK];

    if (file == NULL) {
        printf("streams: block: %s: cannot open\n", STREAM_TEST_FILE);
        return -1;
    }

    if (setvbuf(file, NULL, _IOFBF, sizeof(block)) != 0) {
        printf("streams: block: setvbuf refused\n");
        fclose(file);
        return -1;
    }

    memset(block, 'b', sizeof(block));

    if (fputs("abc", file) != 0) {
        printf("streams: block: fputs failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 0) {
        printf("streams: block: the bytes left before the block was full\n");
        fclose(file);
        return -1;
    }

    if (fflush(file) != 0) {
        printf("streams: block: fflush failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 3) {
        printf("streams: block: fflush wrote %ld bytes, not 3\n", stream_position(file));
        fclose(file);
        return -1;
    }

    /* A full block leaves on its own, with no flush of the caller. */
    if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
        printf("streams: block: fwrite failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 3 + (long)sizeof(block)) {
        printf("streams: block: a full block did not leave\n");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        printf("streams: block: fclose failed\n");
        return -1;
    }

    return 0;
}

/*
 * A line-buffered stream: a line goes to the file when its newline is
 * written, and a last line without one waits for the flush. That is the mode
 * of a terminal and the one a caller asks for a log.
 */
static int check_line_buffering(void)
{
    FILE *file = fopen(STREAM_TEST_FILE, "w");

    if (file == NULL) {
        printf("streams: line: %s: cannot open\n", STREAM_TEST_FILE);
        return -1;
    }

    if (setvbuf(file, NULL, _IOLBF, 0) != 0) {
        printf("streams: line: setvbuf refused\n");
        fclose(file);
        return -1;
    }

    if (fputs("line\n", file) != 0) {
        printf("streams: line: fputs failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 5) {
        printf("streams: line: the line did not leave at its newline\n");
        fclose(file);
        return -1;
    }

    if (fputs("tail", file) != 0) {
        printf("streams: line: fputs failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 5) {
        printf("streams: line: the bytes without a newline left too soon\n");
        fclose(file);
        return -1;
    }

    if (fflush(file) != 0) {
        printf("streams: line: fflush failed\n");
        fclose(file);
        return -1;
    }

    if (stream_position(file) != 9) {
        printf("streams: line: fflush did not write the last line\n");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        printf("streams: line: fclose failed\n");
        return -1;
    }

    return 0;
}

/*
 * The end of the program: a child writes a line into a block-buffered stream
 * and ends with `exit` and no flush of its own, and the line is a file of the
 * volume after it. The stream is flushed before the fork so that the parent
 * does not hold a copy of a byte the child writes.
 */
static int check_exit_flush(void)
{
    FILE *file = fopen(STREAM_TEST_FILE, "w");
    int status = -1;
    long pid;

    if (file == NULL) {
        printf("streams: exit: %s: cannot open\n", STREAM_TEST_FILE);
        return -1;
    }

    if (setvbuf(file, NULL, _IOFBF, STREAM_TEST_BLOCK) != 0) {
        printf("streams: exit: setvbuf refused\n");
        fclose(file);
        return -1;
    }

    if (fflush(file) != 0) {
        printf("streams: exit: fflush failed\n");
        fclose(file);
        return -1;
    }

    /* Neither the file nor the console has a byte for the child to inherit:
     * the child's exit flushes every stream of its copy, and a byte that was
     * in the parent's console block would appear twice. */
    (void)fflush(stdout);

    pid = fork();
    if (pid < 0) {
        printf("streams: exit: fork failed\n");
        fclose(file);
        return -1;
    }

    if (pid == 0) {
        if (fputs("exit\n", file) != 0) {
            _exit(1);
        }
        exit(0); /* no fflush: the end of the program gives the block */
    }

    if (waitpid(pid, &status, 0) != pid || status != 0) {
        printf("streams: exit: the child did not end cleanly\n");
        fclose(file);
        return -1;
    }

    if (fclose(file) != 0) {
        printf("streams: exit: fclose failed\n");
        return -1;
    }

    if (file_size(STREAM_TEST_FILE) != 5) {
        printf("streams: exit: the block of the child was lost\n");
        return -1;
    }

    return 0;
}

int main(void)
{
    FILE *file;
    char word[8];
    int first;
    long at;

    file = fopen(STREAMS_FILE, "r");
    if (file == NULL) {
        printf("streams: %s: cannot open\n", STREAMS_FILE);
        return 1;
    }

    first = fgetc(file);
    if (first == EOF) {
        printf("streams: %s is empty\n", STREAMS_FILE);
        return 1;
    }

    /* The byte comes back, and the read of four starts with it. */
    if (ungetc(first, file) != first) {
        printf("streams: ungetc refused\n");
        return 1;
    }
    memset(word, 0, sizeof(word));
    if (fread(word, 1, 4, file) != 4) {
        printf("streams: fread short\n");
        return 1;
    }
    if ((unsigned char)word[0] != (unsigned char)first) {
        printf("streams: the first byte of the read is not the one given back\n");
        return 1;
    }

    /* Four bytes are read: the stream is at 4, not where the descriptor is. */
    at = ftell(file);
    if (at != 4) {
        printf("streams: ftell says %ld, the read was 4\n", at);
        return 1;
    }

    if (fseek(file, 2, SEEK_CUR) != 0) {
        printf("streams: a relative seek failed\n");
        return 1;
    }
    at = ftell(file);
    if (at != 6) {
        printf("streams: the relative seek landed at %ld, not 6\n", at);
        return 1;
    }

    if (fclose(file) != 0) {
        printf("streams: fclose failed\n");
        return 1;
    }

    /* The three modes of a block: the block, the line and the end of the
     * program. */
    if (check_block_buffering() != 0 || check_line_buffering() != 0 || check_exit_flush() != 0) {
        (void)remove(STREAM_TEST_FILE);
        return 1;
    }

    if (remove(STREAM_TEST_FILE) != 0) {
        printf("streams: %s: cannot remove\n", STREAM_TEST_FILE);
        return 1;
    }

    /* A relative name is resolved by the library, so a file that is not
     * there answers ENOENT: before 1.4 a relative `rename` answered EINVAL,
     * because the kernel has no current directory and the library did not
     * resolve the name. */
    errno = 0;
    if (rename("no-such-a", "no-such-b") != -1 || errno != ENOENT) {
        printf("streams: a relative rename answered %d\n", errno);
        return 1;
    }

    printf("streams: ok\n");
    return 0;
}
