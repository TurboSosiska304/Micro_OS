/*
 * Micro OS — the runner of the tests of the POSIX edge of a program's library.
 *
 * The tests of the kernel run inside a kernel task (`tests/src/main.c`). The
 * tests of the library of a program that need nothing but the door of the
 * system calls (`tests/cases/libc_user_door_shim.c`) run as an ordinary
 * process, like the tests of the pure part (`userland`). The binary is a test
 * of its own because its include path is the one of a program, and because
 * the door of this one answers a tree of the fixture.
 */
#include "ktest.h"

void test_libc_user(void);

int main(void)
{
    test_libc_user();

    return ktest_summary();
}
