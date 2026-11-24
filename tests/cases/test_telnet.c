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

/*
 * The property: the stream of an interactive session hands the bytes of the
 * person back one at a time, keeps the negotiation out of them, and turns
 * the option of echo into the answer of who repeats what.
 */
static void feed_stream(struct telnet_stream *stream, const uint8_t *bytes, size_t n, int *data,
                        size_t data_max, size_t *data_len, char *reply, size_t *reply_len)
{
    size_t i;

    *data_len = 0;
    *reply_len = 0;

    for (i = 0; i < n; i++) {
        int byte = telnet_stream_feed(stream, bytes[i], reply, TELNET_REPLY_MAX, reply_len);

        if (byte >= 0 && *data_len < data_max) {
            data[(*data_len)++] = byte;
        }
    }
}

static void test_stream(void)
{
    struct telnet_stream stream;
    int data[16];
    size_t data_len;
    char reply[TELNET_REPLY_MAX];
    size_t reply_len;

    ktest_begin("telnet: the stream of a session keeps the negotiation out");

    telnet_stream_init(&stream);
    {
        static const uint8_t typed[] = {'l', 's', '\r', '\n'};

        feed_stream(&stream, typed, sizeof(typed), data, 16, &data_len, reply, &reply_len);
        KT_CHECK_INT_EQ((int)data_len, 3);
        KT_CHECK_INT_EQ(data[0], 'l');
        KT_CHECK_INT_EQ(data[1], 's');
        KT_CHECK_INT_EQ(data[2], '\n');
        KT_CHECK_INT_EQ((int)reply_len, 0);
    }

    /* A negotiation in the middle of the line: not one of its bytes is data,
     * and the refusal of the option is answered. */
    telnet_stream_init(&stream);
    {
        static const uint8_t mixed[] = {'a',        TELNET_IAC, TELNET_DO, 3,    'b',
                                        TELNET_IAC, TELNET_IAC, 'c',       '\r', '\n'};

        feed_stream(&stream, mixed, sizeof(mixed), data, 16, &data_len, reply, &reply_len);
        KT_CHECK_INT_EQ((int)data_len, 5);
        KT_CHECK_INT_EQ(data[0], 'a');
        KT_CHECK_INT_EQ(data[1], 'b');
        KT_CHECK_INT_EQ((int)(uint8_t)data[2], 0xFF);
        KT_CHECK_INT_EQ(data[3], 'c');
        KT_CHECK_INT_EQ(data[4], '\n');
        KT_CHECK_INT_EQ((int)reply_len, 3);
        KT_CHECK_INT_EQ((uint8_t)reply[0], TELNET_IAC);
        KT_CHECK_INT_EQ((uint8_t)reply[1], TELNET_WONT);
        KT_CHECK_INT_EQ((uint8_t)reply[2], 3);
    }

    ktest_begin("telnet: the echo is the one the client agreed to");

    telnet_stream_init(&stream);
    KT_CHECK(!stream.echo);

    /* DO ECHO is the client accepting the offer: the server echoes now, and
     * the acceptance is not answered with the generic WONT. */
    {
        static const uint8_t do_echo[] = {TELNET_IAC, TELNET_DO, TELNET_OPT_ECHO};

        feed_stream(&stream, do_echo, sizeof(do_echo), data, 16, &data_len, reply, &reply_len);
        KT_CHECK(stream.echo);
        KT_CHECK_INT_EQ((int)data_len, 0);
        KT_CHECK_INT_EQ((int)reply_len, 0);
    }

    /* DONT ECHO takes it back. */
    {
        static const uint8_t dont_echo[] = {TELNET_IAC, TELNET_DONT, TELNET_OPT_ECHO};

        feed_stream(&stream, dont_echo, sizeof(dont_echo), data, 16, &data_len, reply, &reply_len);
        KT_CHECK(!stream.echo);
        KT_CHECK_INT_EQ((int)reply_len, 0);
    }

    /* A client that wants to echo itself is told not to: one of the two
     * halves is the echo, and it is the server. */
    {
        static const uint8_t will_echo[] = {TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO};

        feed_stream(&stream, will_echo, sizeof(will_echo), data, 16, &data_len, reply, &reply_len);
        KT_CHECK(!stream.echo);
        KT_CHECK_INT_EQ((int)reply_len, 3);
        KT_CHECK_INT_EQ((uint8_t)reply[1], TELNET_DONT);
        KT_CHECK_INT_EQ((uint8_t)reply[2], TELNET_OPT_ECHO);
    }

    ktest_begin("telnet: a stream that is cut anywhere is still the same stream");

    telnet_stream_init(&stream);
    {
        static const uint8_t bytes[] = {TELNET_IAC, TELNET_WILL, 3, 'o', 'k'};

        /* Two calls in the middle of the option: the state survives. */
        feed_stream(&stream, bytes, 2, data, 16, &data_len, reply, &reply_len);
        KT_CHECK_INT_EQ((int)data_len, 0);
        feed_stream(&stream, bytes + 2, sizeof(bytes) - 2, data, 16, &data_len, reply, &reply_len);
        KT_CHECK_INT_EQ((int)data_len, 2);
        KT_CHECK_INT_EQ(data[0], 'o');
        KT_CHECK_INT_EQ(data[1], 'k');
        KT_CHECK_INT_EQ((int)reply_len, 3);
        KT_CHECK_INT_EQ((uint8_t)reply[1], TELNET_DONT);
        KT_CHECK_INT_EQ((uint8_t)reply[2], 3);
    }
}

void test_telnet(void)
{
    test_lines();
    test_negotiation();
    test_subnegotiation();
    test_special_bytes();
    test_stream();
}
