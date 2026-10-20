#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "board/board.h"

void test_board(void)
{
    const struct board_info *info = board_get_info();

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
