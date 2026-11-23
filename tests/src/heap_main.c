/*
 * Micro OS — the runner of the tests of the allocator of a program.
 *
 * `components/libc/user/src/malloc.c` defines `malloc`, `calloc`, `realloc`
 * and `free`, so it cannot be linked into a test binary that also uses the
 * allocator of the host: the two would be one function and the `free` of the
 * test would walk a block of the C library as if it were a block of the arena.
 * The test is therefore a binary of its own, and this is its `main`.
 */
#include "ktest.h"

void test_heap(void);

int main(void)
{
    test_heap();

    return ktest_summary();
}
