/*
 * The SGR state of a terminal (kernel/ansi.h): the colour, the bold and the
 * reverse of a line, and the attribute of a cell.
 *
 * The screen of x86-64 reads these sequences and the board of the panel reads
 * them one day; the parser is portable and the test is the host's, so a
 * number of the table that is wrong is a failed test and not a screen nobody
 * reads. The vectors are the sequences a terminal of the tree writes, the
 * ones it ignores, and the empty list of ESC[m.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/ansi.h"

/* The pair of a console of the dark theme: light grey on black. */
#define PLAIN_FG 7u
#define PLAIN_BG 0u

/* The attribute of a pair, the byte a screen of the port stores. */
#define PORT_ATTR(fg, bg) ((k_u8_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)))

static k_u8_t attr_of(const struct k_ansi *state)
{
    return k_ansi_attr(state, PLAIN_FG, PLAIN_BG);
}

static void test_ansi_reset(void)
{
    struct k_ansi state;

    ktest_begin("ansi: the reset is the pair of the console");

    k_ansi_reset(&state);
    KT_CHECK_INT_EQ((int)state.fg, (int)K_ANSI_DEFAULT);
    KT_CHECK_INT_EQ((int)state.bg, (int)K_ANSI_DEFAULT);
    KT_CHECK(state.bold == false);
    KT_CHECK(state.reverse == false);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));

    /* A line that asked for a bright ink and the reverse goes back to the
     * pair of the console with a zero. 97 is the bright white and 44 the
     * blue, which is the index 1 of the palette. */
    (void)k_ansi_sgr_text(&state, "1;7;97;44", strlen("1;7;97;44"));
    KT_CHECK(state.bold == true);
    KT_CHECK(state.reverse == true);
    KT_CHECK_INT_EQ((int)state.fg, 15);
    KT_CHECK_INT_EQ((int)state.bg, 1);

    (void)k_ansi_sgr_text(&state, "0", 1);
    KT_CHECK(state.bold == false);
    KT_CHECK(state.reverse == false);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));

    /* ESC[m is ESC[0m: an empty parameter list is the reset. */
    (void)k_ansi_sgr_text(&state, "31", 2);
    KT_CHECK_INT_EQ((int)state.fg, 4);
    (void)k_ansi_sgr_text(&state, "", 0);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));
}

static void test_ansi_colors(void)
{
    struct k_ansi state;

    ktest_begin("ansi: the eight colours, the bright eight and the default");

    k_ansi_reset(&state);

    /* 31 is red, which the palette of a VGA numbers 4: the order of a
     * terminal is black, red, green, yellow, blue, ... and the order of the
     * attribute is black, blue, green, cyan, red, ... */
    (void)k_ansi_sgr_text(&state, "31", 2);
    KT_CHECK_INT_EQ((int)state.fg, 4);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(4, PLAIN_BG));

    /* 44 is the blue paper, the index 1. */
    (void)k_ansi_sgr_text(&state, "44", 2);
    KT_CHECK_INT_EQ((int)state.bg, 1);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(4, 1));

    /* 39 and 49 give the pair of the console back, one at a time. */
    (void)k_ansi_sgr_text(&state, "39", 2);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, 1));
    (void)k_ansi_sgr_text(&state, "49", 2);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));

    /* 92 is the bright green and 103 the bright yellow: 10 and 14. */
    (void)k_ansi_sgr_text(&state, "92", 2);
    KT_CHECK_INT_EQ((int)state.fg, 10);
    (void)k_ansi_sgr_text(&state, "103", 3);
    KT_CHECK_INT_EQ((int)state.bg, 14);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(10, 14));

    /* Every number of the two ranges, one after the other, is the table of
     * the palette: black 0, red 4, green 2, yellow 6, blue 1, magenta 5,
     * cyan 3, white 7. */
    {
        static const k_u8_t palette[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        int value;

        for (value = 30; value <= 37; value++) {
            char text[4];
            int length = 0;

            if (value >= 100) {
                text[length++] = (char)('0' + value / 100);
            }
            if (value >= 10) {
                text[length++] = (char)('0' + (value / 10) % 10);
            }
            text[length++] = (char)('0' + value % 10);
            text[length] = '\0';

            (void)k_ansi_sgr_text(&state, text, (k_size_t)length);
            KT_CHECK_INT_EQ((int)state.fg, palette[value - 30]);
        }
    }
}

static void test_ansi_bold_reverse(void)
{
    struct k_ansi state;

    ktest_begin("ansi: the bold brightens an ink of a command, the reverse trades the pair");

    k_ansi_reset(&state);

    /* A bold green is the bright green: 32 becomes 10. */
    (void)k_ansi_sgr_text(&state, "1;32", 4);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(10, PLAIN_BG));

    /* 22 takes the bold away, so the ink is the plain green again. */
    (void)k_ansi_sgr_text(&state, "22", 2);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(2, PLAIN_BG));

    /* The bold does not brighten the pair of the console: it is the theme a
     * person chose and not a colour a command asked for. 39 gives the ink of
     * the console back, so this is the pair itself with the bold on it. */
    (void)k_ansi_sgr_text(&state, "1;39", 4);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));

    /* 7 trades the two, 27 puts them back; the bold is still on, and the
     * pair of the console is not brightened by it. */
    (void)k_ansi_sgr_text(&state, "7", 1);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_BG, PLAIN_FG));
    (void)k_ansi_sgr_text(&state, "27", 2);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));
}

static void test_ansi_ignored(void)
{
    struct k_ansi state;

    ktest_begin("ansi: a parameter the system does not know is skipped, the rest counts");

    k_ansi_reset(&state);

    /* 2 (dim), 4 (underline), 5 (blink) and 53 (overline) are not colours of
     * this screen: the state is what it was and the 31 after them still
     * counts. */
    KT_CHECK_INT_EQ(k_ansi_sgr_text(&state, "2;4;5;53", 8), 0);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(PLAIN_FG, PLAIN_BG));

    KT_CHECK(!k_ansi_sgr(&state, 53));
    KT_CHECK_INT_EQ(k_ansi_sgr_text(&state, "2;31;53", 7), 1);
    KT_CHECK_INT_EQ((int)attr_of(&state), PORT_ATTR(4, PLAIN_BG));

    /* An empty parameter between two semicolons is a zero: 31;;44 is the
     * reset, the red ink and the blue paper - so the ink of the reset is the
     * one that stays. */
    (void)k_ansi_sgr_text(&state, "31;;44", 6);
    KT_CHECK_INT_EQ((int)state.fg, K_ANSI_DEFAULT);
    KT_CHECK_INT_EQ((int)state.bg, 1);

    /* A number that is not a colour leaves the state alone as well. */
    KT_CHECK_INT_EQ(k_ansi_sgr_text(&state, "300", 3), 0);
    KT_CHECK_INT_EQ((int)state.bg, 1);

    /* A list that ends with a semicolon has a zero at its end: ESC[31;m is
     * the red and then the reset, so the state is the pair of the console. */
    (void)k_ansi_sgr_text(&state, "31;", 3);
    KT_CHECK_INT_EQ((int)state.fg, K_ANSI_DEFAULT);
    KT_CHECK_INT_EQ((int)state.bg, K_ANSI_DEFAULT);
}

static void test_ansi_attr_edges(void)
{
    struct k_ansi state;

    ktest_begin("ansi: the attribute is one byte of two nibbles");

    k_ansi_reset(&state);

    /* A NULL state is the pair of the console: the screens call the helper
     * before the first sequence and it must not fault. */
    KT_CHECK_INT_EQ((int)k_ansi_attr(NULL, 7, 0), 0x07);
    KT_CHECK_INT_EQ((int)k_ansi_attr(NULL, 0, 7), 0x70);

    /* The light theme: black on light grey, and a red word on it. */
    (void)k_ansi_sgr_text(&state, "31", 2);
    KT_CHECK_INT_EQ((int)k_ansi_attr(&state, 0, 7), 0x74);

    /* The reverse of the light pair: 0x70 is the paper and 0x07 the ink, so
     * the reversed pair is light grey on black. */
    (void)k_ansi_sgr_text(&state, "0;7", 3);
    KT_CHECK_INT_EQ((int)k_ansi_attr(&state, 0, 7), 0x07);
}

void test_ansi(void)
{
    test_ansi_reset();
    test_ansi_colors();
    test_ansi_bold_reverse();
    test_ansi_ignored();
    test_ansi_attr_edges();
}
