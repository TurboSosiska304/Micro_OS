#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/kmod.h"

static int s_init_calls;
static int s_check_calls;
static int s_check_result;

static int good_init(void)
{
    s_init_calls++;
    return 0;
}

static int bad_init(void)
{
    return -5;
}

static int good_check(void)
{
    s_check_calls++;
    return s_check_result;
}

static const char *detail(void)
{
    return "all good";
}

static void test_register(void)
{
    static const struct kmod mod = {
        .name = "testmod",
        .desc = "a module used by the unit tests",
        .init = good_init,
        .check = good_check,
        .detail = detail,
    };

    ktest_begin("kmod: a module is initialised on registration");

    s_init_calls = 0;
    s_check_calls = 0;
    s_check_result = 0;

    KT_CHECK_INT_EQ(kmod_register(&mod), 0);
    KT_CHECK_INT_EQ(s_init_calls, 1);

    if (kmod_count() > 0) {
        const struct kmod *m = kmod_get(0);
        KT_CHECK(m != NULL);
        if (m != NULL) {
            KT_CHECK_STR_EQ(m->name, "testmod");
            KT_CHECK_INT_EQ(m->state, KMOD_OK);
        }
    }
}

static void test_doctor(void)
{
    ktest_begin("kmod: the doctor checks every module");

    s_check_calls = 0;
    s_check_result = 0;

    KT_CHECK_INT_EQ(kmod_doctor(), 0);
    KT_CHECK(s_check_calls > 0);

    if (kmod_count() > 0) {
        const struct kmod *m = kmod_get(0);
        if (m != NULL) {
            KT_CHECK_STR_EQ(m->info, "all good");
            KT_CHECK_INT_EQ(m->state, KMOD_OK);
        }
    }
}

static void test_doctor_problem(void)
{
    static const struct kmod broken = {
        .name = "broken",
        .desc = "a module that does not work",
        .init = bad_init,
        .check = good_check,
    };

    ktest_begin("kmod: a module that fails to initialise is reported");

    KT_CHECK_INT_EQ(kmod_register(&broken), 0);

    if (kmod_count() > 0) {
        const struct kmod *m = kmod_get(kmod_count() - 1);
        KT_CHECK(m != NULL);
        if (m != NULL) {
            KT_CHECK_INT_EQ(m->state, KMOD_FAIL);
        }
    }

    /* The broken module must be counted as a problem. */
    KT_CHECK(kmod_doctor() >= 1);
}

void test_kmod(void)
{
    test_register();
    test_doctor();
    test_doctor_problem();
}
