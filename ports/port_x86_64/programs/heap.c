/*
 * Micro OS — a program that needs the memory of a window.
 *
 * The window of the desktop of this machine is 800 by 600 pixels and a pixel
 * is four bytes: 1 920 000 bytes, more than the arena of the library of a
 * program used to have (256 KiB, a constant of the build). The block comes
 * from the heap of the program through `brk` now, so this program asks for the
 * window without defining `ULIBC_HEAP_SIZE` at all, writes it and gives it
 * back.
 *
 * The block is freed and taken again: the second answer comes from the heap of
 * the program and not from a second call of the machine.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 800u
#define WINDOW_HEIGHT 600u
#define PIXEL_SIZE 4u
#define WINDOW_BYTES (WINDOW_WIDTH * WINDOW_HEIGHT * PIXEL_SIZE)

int main(int argc, char *const argv[], char *const envp[])
{
    unsigned char *pixels;
    unsigned char *again;
    unsigned sum = 0;

    (void)argc;
    (void)argv;
    (void)envp;

    printf("heap: a window is %u bytes\n", (unsigned)WINDOW_BYTES);

    pixels = (unsigned char *)malloc(WINDOW_BYTES);
    if (pixels == NULL) {
        printf("heap: the window is refused with %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    memset(pixels, 0, WINDOW_BYTES);
    pixels[0] = 0x11;
    pixels[WINDOW_BYTES / 2] = 0x22;
    pixels[WINDOW_BYTES - 1] = 0x33;

    sum = (unsigned)(pixels[0] + pixels[WINDOW_BYTES / 2] + pixels[WINDOW_BYTES - 1]);
    printf("heap: the corners of the window say %u\n", sum);

    free(pixels);

    again = (unsigned char *)malloc(WINDOW_BYTES);
    if (again == NULL) {
        printf("heap: the window is gone with %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }
    again[WINDOW_BYTES - 1] = 0x44;
    printf("heap: the second window ends with %u\n", (unsigned)again[WINDOW_BYTES - 1]);
    free(again);

    printf("heap: done\n");

    return ULIBC_EXIT_SUCCESS;
}
