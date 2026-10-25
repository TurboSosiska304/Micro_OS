/*
 * Micro OS — the line of the console cut into the rows of the screen.
 *
 * The cases a person can compute by hand: a line that fits in one
 * row, a line split into pieces at an exact boundary, the empty line
 * (it has no rows — the console keeps it as a row of its own), and
 * the buffer that is smaller than the piece asked for.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "graphics/text_wrap.h"

static void test_fits(void)
{
    char piece[32];

    ktest_begin("wrap: a line that fits comes out in one row");

    memset(piece, 'x', sizeof(piece));

    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "hello", 10, 0), 5);
    KT_CHECK_STR_EQ(piece, "hello");
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "hello", 10, 1), 0);
    KT_CHECK_STR_EQ(piece, "");

    /* The line that is exactly the width of the row. */
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcd", 4, 0), 4);
    KT_CHECK_STR_EQ(piece, "abcd");
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcd", 4, 1), 0);
}

static void test_split(void)
{
    char piece[32];

    ktest_begin("wrap: the tail of a long line goes to the row below");

    /* Ten letters, four to a row: three rows, the last one short. */
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcdefghij", 4, 0), 4);
    KT_CHECK_STR_EQ(piece, "abcd");
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcdefghij", 4, 1), 4);
    KT_CHECK_STR_EQ(piece, "efgh");
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcdefghij", 4, 2), 2);
    KT_CHECK_STR_EQ(piece, "ij");
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abcdefghij", 4, 3), 0);

    /* The case of the console: a line of the landscape (40) on the
     * portrait (22): the tail is 18 letters, nothing is lost. */
    {
        char line[41];
        int i;

        for (i = 0; i < 40; i++) {
            line[i] = (char)('a' + (i % 26));
        }
        line[40] = '\0';

        KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), line, 22, 0), 22);
        KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), line, 22, 1), 18);
        KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), line, 22, 2), 0);
    }
}

static void test_edges(void)
{
    char piece[32];
    char small[3];

    ktest_begin("wrap: the empty line, the small buffer and the bad numbers");

    /* The empty line has no rows: the console keeps it as a row of
     * its own, the cutting itself says there is nothing to cut. */
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "", 10, 0), 0);

    /* The buffer smaller than the piece: what fits comes out. */
    memset(small, 'x', sizeof(small));
    KT_CHECK_INT_EQ(text_wrap_take(small, sizeof(small), "abcdefgh", 10, 0), 2);
    KT_CHECK_STR_EQ(small, "ab");

    /* Nonsense numbers answer with nothing instead of writing. */
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abc", 10, -1), 0);
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), "abc", 0, 0), 0);
    KT_CHECK_INT_EQ(text_wrap_take(NULL, 32, "abc", 10, 0), 0);
    KT_CHECK_INT_EQ(text_wrap_take(piece, 0, "abc", 10, 0), 0);
    KT_CHECK_INT_EQ(text_wrap_take(piece, sizeof(piece), NULL, 10, 0), 0);
}

void test_text_wrap(void)
{
    test_fits();
    test_split();
    test_edges();
}
