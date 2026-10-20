/*
 * Micro OS — unit test runner.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>

int main(void) {
    printf("Micro OS unit tests\n\n");

    test_version();
    test_board();
    test_log();
    test_boot();

    return ktest_summary();
}
