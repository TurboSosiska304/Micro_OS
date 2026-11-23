/*
 * Micro OS — the streams of the library, as a program of the machine.
 *
 * The two defects of 1.2.0 lived here and neither the host tests nor the
 * smoke could see them, because a stream over a descriptor needs a kernel
 * under it: `fread` after `ungetc` wrote the rest of the block one byte past
 * where the caller kept it, and `fseek(SEEK_CUR)` counted from the descriptor
 * instead of the stream, which the block of read-ahead puts out of step.
 *
 * This program is the test: it reads a file whose first line is known, gives
 * a byte back, reads a word, and asks for the position twice with a relative
 * seek between. It says `streams: ok` and exits 0 when all of it holds, and
 * names the first thing that did not.
 */
#include "ulibc.h"

#include <stdio.h>
#include <string.h>

#define STREAMS_FILE "/etc/setting"

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

    printf("streams: ok\n");
    return 0;
}
