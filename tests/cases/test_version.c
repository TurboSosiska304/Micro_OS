#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/version.h"

static int _kernel_build_id_is_a_date(void)
{
    const char *id = kernel_build_id();

    /* "mmm dd yyyy hh:mm:ss": a month of three letters, a space, two digits. */
    return id[0] != '\0' && id[3] == ' ' && id[6] == ' ' && id[11] == ' ' && id[14] == ':';
}

void test_version(void)
{
    ktest_begin("version: identification strings are sane");

    KT_CHECK_STR_NE(kernel_name(), "");
    KT_CHECK_STR_NE(kernel_version(), "");
    /* A reproducible build (SOURCE_DATE_EPOCH=0) has no date to stamp, and the
     * string is empty on purpose: `uname` prints no parenthesis then, instead
     * of "Jan  1 1970" (kernel/version.c). A build with a real date answers
     * the date. */
    KT_CHECK(_kernel_build_id_is_a_date() || kernel_build_id()[0] == '\0');
    KT_CHECK_STR_NE(kernel_compiler(), "");
    KT_CHECK_STR_NE(kernel_arch(), "");

    /* The version must look like "major.minor.patch". */
    KT_CHECK(strchr(kernel_version(), '.') != NULL);
    KT_CHECK(strchr(kernel_version(), '.') != strrchr(kernel_version(), '.'));
}
