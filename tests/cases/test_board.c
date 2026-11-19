#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "board/board.h"

/*
 * The registry is what the rest of the system reads: the kernel keeps the
 * pointer it is given and hands back exactly that one. The other tests run
 * a boot of their own, so the previous answer is put back at the end.
 */
static void test_board_registry(void)
{
    static const struct kernel_board fake = {
        .name = "fake",
        .desc = "fake board",
    };
    const struct kernel_board *saved = k_board_get();

    ktest_begin("board: the registry keeps the registered description");

    k_board_register(&fake);
    KT_CHECK(k_board_get() == &fake);
    KT_CHECK(k_board_get() != NULL);
    KT_CHECK_STR_EQ(k_board_get()->name, "fake");

    k_board_register(NULL);
    KT_CHECK(k_board_get() == NULL);

    k_board_register(saved);
    KT_CHECK(k_board_get() == saved);
}

void test_board(void)
{
    const struct kernel_board *info;

    test_board_registry();

    info = board_get_info();

    ktest_begin("board: the description is complete");

    KT_CHECK(info != NULL);
    if (info == NULL) {
        return;
    }

    KT_CHECK_STR_NE(info->name, "");
    KT_CHECK_STR_NE(info->desc, "");
    KT_CHECK_STR_NE(info->soc, "");
    KT_CHECK_STR_NE(info->lcd, "");
    KT_CHECK_STR_NE(info->buttons, "");
}
