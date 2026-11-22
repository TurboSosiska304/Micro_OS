/*
 * Micro OS — hello, the first program of the board (ADR 0018).
 *
 * This is the same program shape as its x86-64 cousin in
 * ports/port_x86_64/programs: it is built against components/libc/user, it
 * has `main(argc, argv, envp)`, and every line it prints goes through the
 * door of the port. The difference is the machine: there is no ring 3 and no
 * address space, so the lines say so instead of printing a privilege level
 * that would always be zero.
 *
 * The program is also the proof of the loader in tools/device-test.sh: the
 * text runs from the instruction RAM, the data from DRAM, the arguments and
 * the environment crossed into the data window, and exit() ends the task and
 * wakes the caller of execve. A stat() of its own file proves that a path
 * crosses the door and the VFS answers.
 */
#include "ulibc.h"

#define PROGRAM_LINE "PROGRAM: hello from /bin\n"

int main(int argc, char *const argv[], char *const envp[])
{
    struct vfs_stat info;
    int envs = 0;
    int index;

    printf(PROGRAM_LINE);

    /*
     * The price of the board, written by the program itself: the text is
     * code of the kernel's own address space and there is no page that could
     * be made read-only or absent (ADR 0018 §4).
     */
    printf("PROGRAM: arch xtensa, no protection\n");

    if (ulibc_stat_call("/bin/hello", &info) != 0) {
        printf("PROGRAM: stat /bin/hello failed\n");
        return 1;
    }

    printf("PROGRAM: /bin/hello is %lu bytes, mode %lu\n", (unsigned long)info.size,
           (unsigned long)info.mode);

    /*
     * The vector the kernel built for this program. `exec /bin/hello one two`
     * is three words, and the name of the program is the first of them, as
     * in Linux.
     */
    printf("PROGRAM: argc %d\n", argc);
    for (index = 0; index < argc; index++) {
        printf("PROGRAM: argv[%d] %s\n", index, argv[index]);
    }

    /*
     * The environment of the system (k_exec_environ) when the call carried
     * none: the pointer crossed the loader and the strings are in the data
     * window the port filled.
     */
    if (envp != NULL) {
        while (envp[envs] != NULL) {
            envs++;
        }
    }

    printf("PROGRAM: envp %d", envs);
    if (envs > 0) {
        printf(", %s", envp[0]);
    }
    printf("\n");

    printf("PROGRAM: pid %d, up %ld ms\n", getpid(), uptime());

    /* The last line before exit: the status the caller of execve sees in the
     * log of the port, and the reason the shell is alive after this. */
    printf("PROGRAM: exit %d\n", 0);

    return 0;
}
