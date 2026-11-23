/*
 * Micro OS — the door of the program for the tests of the library on the host.
 *
 * The pure part of the library of a program is compiled for the host and runs
 * as an ordinary process: there is no kernel behind it, no window of a
 * program and no vector 128. The functions of the library that ask the
 * kernel still have to link, and this file is the door they find. Most calls
 * answer zero, which is the "success with the number zero" of the calls the
 * test exercises (`getuid`, `geteuid`, `getgid`, `setuid`, `setgid`).
 *
 * Three of them answer a file instead: `open`, `read` and `close` serve the
 * text the test put in with userland_door_set_passwd(), so that the reader of
 * /etc/passwd of the library (src/pwd.c) is covered on the host by a real
 * record and a real line that is not one. The real door is
 * `components/libc/user/src/syscall.c`, and its crossing is covered by
 * `make x86-smoke`, where a program of the machine calls the kernel.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"

/* The seam of the library (`src/ulibc_internal.h`), declared here because the
 * header of the seam is private to the directory of the library. */
long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);
long __ulibc_result(long result);

/* The text of /etc/passwd the test answers, the position in it and whether
 * the file is open. NULL means "there is no such file". */
static const char *g_passwd_text;
static size_t g_passwd_at;
static int g_passwd_open;

/* Hand the reader of the library a file of accounts, or take it away with
 * NULL. Declared by the test that calls it (tests/cases/test_userland.c). */
void userland_door_set_passwd(const char *text);

/*
 * The heap of the call 47, for the test of the allocator of a program
 * (tests/cases/test_heap.c): a ceiling the test may lower, a count of the
 * calls and the way back to the whole heap.
 */
void userland_door_brk_limit(unsigned long limit);
void userland_door_brk_reset(void);
unsigned long userland_door_brk_calls(void);

void userland_door_set_passwd(const char *text)
{
    g_passwd_text = text;
    g_passwd_at = 0;
    g_passwd_open = 0;
}

/*
 * The heap the call 47 answers with, for the test of the allocator of a
 * program (tests/cases/test_heap.c). The door is the machine behind `brk`:
 * a block of the bss that grows and never shrinks, a ceiling the test may
 * lower to see the refusal of ENOMEM, and a count of the calls so that a test
 * can prove the heap does not ask twice for the same bytes.
 *
 * The address of the block is the break, so the library may align it and the
 * `brk` of the caller is a number inside the block. A request outside the
 * block - or above the ceiling - is the ENOMEM of a machine that cannot grow
 * the heap any further.
 */
#define USERLAND_BRK_SIZE (8u * 1024u * 1024u)

_Alignas(16) static unsigned char g_brk_heap[USERLAND_BRK_SIZE];
static unsigned long g_brk_used;
static unsigned long g_brk_ceiling = USERLAND_BRK_SIZE;
static unsigned long g_brk_calls;

void userland_door_brk_limit(unsigned long limit)
{
    g_brk_ceiling = (limit > USERLAND_BRK_SIZE) ? USERLAND_BRK_SIZE : limit;
}

void userland_door_brk_reset(void)
{
    g_brk_ceiling = USERLAND_BRK_SIZE;
}

unsigned long userland_door_brk_calls(void)
{
    return g_brk_calls;
}

static long door_brk(unsigned long address)
{
    unsigned long base = (unsigned long)(uintptr_t)g_brk_heap;
    unsigned long target;

    /* A zero asks for the break and does not move it. */
    if (address == 0) {
        return (long)(base + g_brk_used);
    }

    if (address < base) {
        return KERR_NOMEM;
    }

    target = address - base;

    if (target > g_brk_ceiling || target > USERLAND_BRK_SIZE) {
        return KERR_NOMEM;
    }

    g_brk_used = target;
    g_brk_calls++;

    return (long)address;
}

long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
    (void)a1;
    (void)a3;
    (void)a4;
    (void)a5;

    switch (number) {
    case KSYS_BRK:
        return door_brk((unsigned long)a0);
    case KSYS_OPEN:
        if (g_passwd_text == NULL) {
            return KERR_NOENT;
        }
        g_passwd_at = 0;
        g_passwd_open = 1;
        return 3; /* a descriptor that is not the console */
    case KSYS_READ: {
        size_t count = (size_t)a2;
        size_t left;

        if (!g_passwd_open) {
            return KERR_BADF;
        }

        left = strlen(g_passwd_text) - g_passwd_at;
        if (count > left) {
            count = left;
        }

        memcpy((void *)(uintptr_t)a1, g_passwd_text + g_passwd_at, count);
        g_passwd_at += count;

        return (long)count;
    }
    case KSYS_CLOSE:
        g_passwd_open = 0;
        return 0;
    default:
        return 0;
    }
}

long __ulibc_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
    }

    return result;
}
