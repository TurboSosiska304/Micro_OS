/*
 * Micro OS — the runner of the tests of the library of a program.
 *
 * The tests of the kernel run inside a kernel task (`tests/src/main.c`). The
 * pure part of the library of a program is C without a kernel under it, so it
 * is compiled for the host and runs as an ordinary process; the binary is a
 * test of its own because its include path is the one of a program (the
 * headers of `components/libc/user`) and the kernel tests must keep the
 * headers of the host.
 */
#include "ktest.h"

void test_userland(void);

int main(void)
{
    test_userland();

    return ktest_summary();
}
