/*
 * Micro OS — the line part of telnet.
 *
 * The bytes of a client are pushed through the parser the way they
 * come off the wire: a line with CR LF, a line cut between two calls,
 * the negotiation that a telnet client opens the session with (every
 * option of it is refused, with the mirror of the command), a
 * subnegotiation swallowed whole, and a line that grew too long —
 * thrown away whole, like the shell throws away a line that does not
 * fit, without losing the line after it.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "telnet/telnet_proto.h"

/* Feed a chunk of bytes and take the lines it finished, in order. */
static int feed(struct telnet_parser *p, const uint8_t *bytes, size_t n,
                char lines[][TELNET_LINE_MAX], int max_lines)
{
    char reply[TELNET_REPLY_MAX];
    int lines_taken = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        size_t reply_len = 0;

        telnet_parser_feed(p, bytes[i], reply, sizeof(reply), &reply_len);
        KT_CHECK_INT_EQ(reply_len, 0); /* no answer for plain data */

        if (lines != NULL && lines_taken < max_lines &&
            telnet_parser_take_line(p, lines[lines_taken], TELNET_LINE_MAX)) {
            lines_taken++;
        }
    }

    return lines_taken;
}

static void test_lines(void)
{
    struct telnet_parser p;
    char lines[4][TELNET_LINE_MAX];
    static const uint8_t two_lines[] = "date\r\nhelp\r\n";
    static const uint8_t empty_line[] = "\r\n";

    ktest_begin("telnet: the lines come out as a person typed them");

    telnet_parser_init(&p);
    KT_CHECK_INT_EQ(feed(&p, two_lines, sizeof(two_lines) - 1, lines, 4), 2);
    KT_CHECK_STR_EQ(lines[0], "date");
    KT_CHECK_STR_EQ(lines[1], "help");

    telnet_parser_init(&p);
    KT_CHECK_INT_EQ(feed(&p, empty_line, sizeof(empty_line) - 1, lines, 4), 1);
    KT_CHECK_STR_EQ(lines[0], ""); /* the Enter of nobody: the prompt comes back */

    ktest_begin("telnet: a line cut between two calls comes out whole");

    telnet_parser_init(&p);
    KT_CHECK_INT_EQ(feed(&p, (const uint8_t *)"he", 2, NULL, 0), 0);
    KT_CHECK_INT_EQ(feed(&p, (const uint8_t *)"lp\r\n", 4, lines, 4), 1);
    KT_CHECK_STR_EQ(lines[0], "help");
}

static void test_negotiation(void)
{
    struct telnet_parser p;
    char reply[TELNET_REPLY_MAX];
    size_t reply_len;
    char lines[2][TELNET_LINE_MAX];
    static const uint8_t will_echo[] = {TELNET_IAC, TELNET_WILL, 0x01};
    static const uint8_t do_sga[] = {TELNET_IAC, TELNET_DO, 0x03};
    static const uint8_t wont_nothing[] = {TELNET_IAC, TELNET_WONT, 0x1F};
    static const uint8_t dont_nothing[] = {TELNET_IAC, TELNET_DONT, 0x1F};
    static const uint8_t after[] = "ok\r\n";

    ktest_begin("telnet: every option of the client is refused");

    /* WILL is answered with DONT, DO with WONT, WONT and DONT in silence. */
    telnet_parser_init(&p);
    reply_len = 0;
    telnet_parser_feed(&p, will_echo[0], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, will_echo[1], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, will_echo[2], reply, sizeof(reply), &reply_len);
    KT_CHECK_INT_EQ(reply_len, 3);
    KT_CHECK_INT_EQ((uint8_t)reply[0], TELNET_IAC);
    KT_CHECK_INT_EQ((uint8_t)reply[1], TELNET_DONT);
    KT_CHECK_INT_EQ((uint8_t)reply[2], 0x01);

    telnet_parser_init(&p);
    reply_len = 0;
    telnet_parser_feed(&p, do_sga[0], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, do_sga[1], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, do_sga[2], reply, sizeof(reply), &reply_len);
    KT_CHECK_INT_EQ(reply_len, 3);
    KT_CHECK_INT_EQ((uint8_t)reply[1], TELNET_WONT);

    telnet_parser_init(&p);
    reply_len = 0;
    telnet_parser_feed(&p, wont_nothing[0], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, wont_nothing[1], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, wont_nothing[2], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, dont_nothing[0], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, dont_nothing[1], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, dont_nothing[2], reply, sizeof(reply), &reply_len);
    KT_CHECK_INT_EQ(reply_len, 0);

    ktest_begin("telnet: the negotiation never leaks into a command");

    telnet_parser_init(&p);
    reply_len = 0;
    telnet_parser_feed(&p, will_echo[0], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, will_echo[1], reply, sizeof(reply), &reply_len);
    telnet_parser_feed(&p, will_echo[2], reply, sizeof(reply), &reply_len);
    KT_CHECK_INT_EQ(feed(&p, after, sizeof(after) - 1, lines, 2), 1);
    KT_CHECK_STR_EQ(lines[0], "ok");
}

static void test_subnegotiation(void)
{
    struct telnet_parser p;
    char lines[2][TELNET_LINE_MAX];
    static const uint8_t sub_then_line[] = {
        TELNET_IAC, TELNET_SB, 0x18, 0x01, 0x02, TELNET_IAC, TELNET_SE, 'o', 'k', '\r', '\n',
    };

    ktest_begin("telnet: a subnegotiation is swallowed whole");

    telnet_parser_init(&p);
    KT_CHECK_INT_EQ(feed(&p, sub_then_line, sizeof(sub_then_line), lines, 2), 1);
    KT_CHECK_STR_EQ(lines[0], "ok");
}

static void test_special_bytes(void)
{
    struct telnet_parser p;
    char lines[2][TELNET_LINE_MAX];
    char line[TELNET_LINE_MAX];
    size_t i;

    ktest_begin("telnet: IAC IAC is one byte of data, a NUL is not a letter");

    telnet_parser_init(&p);
    {
        static const uint8_t escaped[] = {TELNET_IAC, TELNET_IAC, 'x', '\r', '\n'};

        KT_CHECK_INT_EQ(feed(&p, escaped, sizeof(escaped), lines, 2), 1);
        KT_CHECK_INT_EQ(strlen(lines[0]), 2);
        KT_CHECK_INT_EQ((uint8_t)lines[0][0], 0xFF);
        KT_CHECK_INT_EQ(lines[0][1], 'x');
    }

    telnet_parser_init(&p);
    {
        static const uint8_t with_nul[] = {'a', 0, 'b', '\r', '\n'};

        KT_CHECK_INT_EQ(feed(&p, with_nul, sizeof(with_nul), lines, 2), 1);
        KT_CHECK_STR_EQ(lines[0], "ab");
    }

    ktest_begin("telnet: a line that does not fit is thrown away whole");

    telnet_parser_init(&p);

    /* TELNET_LINE_MAX - 1 letters fit, the next one is too much. */
    for (i = 0; i + 1 < TELNET_LINE_MAX; i++) {
        telnet_parser_feed(&p, 'a', NULL, 0, NULL);
    }
    telnet_parser_feed(&p, 'a', NULL, 0, NULL);
    telnet_parser_feed(&p, '\r', NULL, 0, NULL);
    telnet_parser_feed(&p, '\n', NULL, 0, NULL);
    KT_CHECK(!telnet_parser_take_line(&p, line, sizeof(line)));

    /* The line after it is whole: the state of the parser survived. */
    KT_CHECK_INT_EQ(feed(&p, (const uint8_t *)"ok\r\n", 4, lines, 2), 1);
    KT_CHECK_STR_EQ(lines[0], "ok");
}

void test_telnet(void)
{
    test_lines();
    test_negotiation();
    test_subnegotiation();
    test_special_bytes();
}
