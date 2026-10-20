#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/version.h"

void test_version(void) {
    ktest_begin("version: identification strings are sane");

    KT_CHECK_STR_NE(kernel_name(), "");
    KT_CHECK_STR_NE(kernel_version(), "");
    KT_CHECK_STR_NE(kernel_build_id(), "");
    KT_CHECK_STR_NE(kernel_compiler(), "");
    KT_CHECK_STR_NE(kernel_arch(), "");

    /* The version must look like "major.minor.patch". */
    KT_CHECK(strchr(kernel_version(), '.') != NULL);
    KT_CHECK(strchr(kernel_version(), '.') != strrchr(kernel_version(), '.'));
}
