/*
 * Micro OS — hello, a program built with the SDK and with nothing else.
 *
 * The file is the proof of the SDK and not a program of the machine: it
 * includes the headers of `<sdk>/include`, links `<sdk>/lib/crt0.o` and
 * `<sdk>/lib/libmicroos_user.a` at the window of the port, and becomes a
 * `MOX1` image without a checkout of the tree anywhere in the picture. Every
 * line below is a call of the library of a program (a door of `int $0x80`
 * under it) and not of a libc of the host: a `printf`, an arena, a stream of
 * the VFS and a number of the kernel.
 *
 * The build is `share/examples/hello/CMakeLists.txt`; the README of the SDK
 * has the two commands and the flags they set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HELLO_LINE 128

int main(int argc, char *const argv[], char *const envp[])
{
    char *line = malloc(HELLO_LINE);
    FILE *setting;
    int envs = 0;

    printf("hello: an image of the SDK of Micro OS\n");

    /* The vector of the arguments the kernel built for this image. */
    printf("hello: argc %d", argc);
    if (argc > 0 && argv != NULL) {
        printf(", argv[0] %s", argv[0]);
    }
    printf("\n");

    /* The third register of the entry: the environment of the program. */
    if (envp != NULL) {
        while (envp[envs] != NULL) {
            envs++;
        }
    }
    printf("hello: %d environment word(s)\n", envs);

    /* A number the kernel answers: the program is a task of the machine. */
    printf("hello: pid %d\n", (int)getpid());

    /*
     * A file of the volume through the streams of the library. The image of a
     * machine carries /etc/setting; a program that runs on a disk without it
     * says so instead of failing, because a test of the SDK must not depend on
     * the tree that built the disk.
     */
    if (line == NULL) {
        printf("hello: the arena of the program is empty\n");
        return 1;
    }

    setting = fopen("/etc/setting", "r");
    if (setting == NULL) {
        printf("hello: no /etc/setting on this volume\n");
    } else {
        while (fgets(line, HELLO_LINE, setting) != NULL) {
            printf("hello: /etc/setting: %s", line);
        }
        fclose(setting);
    }

    free(line);

    return 0;
}
