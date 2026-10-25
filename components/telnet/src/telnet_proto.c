/*
 * Micro OS — the line part of telnet, as arithmetic.
 *
 * See telnet_proto.h for why this file exists on its own. The states
 * of the parser are the states of the wire: the data, the command that
 * follows an IAC, the option that follows WILL/DO, and the
 * subnegotiation that has to be swallowed whole until the next IAC SE.
 */
#include "telnet/telnet_proto.h"

#include <string.h>

#define ST_DATA 0
#define ST_IAC 1
#define ST_OPT 2
#define ST_SUB 3
#define ST_SUB_IAC 4

static void reply_byte(char *reply, size_t reply_max, size_t *reply_len, uint8_t byte)
{
    if (reply == NULL || reply_len == NULL || *reply_len >= reply_max) {
        return;
    }

    reply[*reply_len] = (char)byte;
    (*reply_len)++;
}

void telnet_parser_init(struct telnet_parser *p)
{
    memset(p, 0, sizeof(*p));
}

static void finish_line(struct telnet_parser *p)
{
    if (!p->too_long) {
        /* The bytes stay in the buffer until take_line takes them:
         * the caller takes the line right after the feed that
         * finished it, before the bytes of the next one arrive. An
         * empty line comes out as an empty line — that is the Enter
         * of a person who typed nothing, and the client wants its
         * prompt back after it. */
        p->line_ready = true;
        return;
    }

    /* Too long: nothing comes out, the buffer starts anew. */
    p->len = 0;
    p->too_long = false;
}

static void append_byte(struct telnet_parser *p, uint8_t byte)
{
    if (p->len + 1 >= TELNET_LINE_MAX) {
        p->too_long = true;
        p->len = 0;
        return;
    }

    p->line[p->len] = (char)byte;
    p->len++;
}

static void feed_data(struct telnet_parser *p, uint8_t byte)
{
    if (byte == TELNET_IAC) {
        p->state = ST_IAC;
        return;
    }

    if (byte == '\r') {
        return; /* CR LF of telnet: the LF is the end of the line */
    }

    if (byte == '\n') {
        finish_line(p);
        return;
    }

    if (byte == 0) {
        return; /* a NUL in a line would end it early for the shell */
    }

    append_byte(p, byte);
}

void telnet_parser_feed(struct telnet_parser *p, uint8_t byte, char *reply, size_t reply_max,
                        size_t *reply_len)
{
    switch (p->state) {
    case ST_IAC:
        if (byte == TELNET_IAC) {
            /* IAC IAC: a byte of 0xFF of the data itself. */
            p->state = ST_DATA;
            append_byte(p, TELNET_IAC);
        } else if (byte == TELNET_SB) {
            p->state = ST_SUB;
        } else if (byte == TELNET_WILL || byte == TELNET_WONT || byte == TELNET_DO ||
                   byte == TELNET_DONT) {
            p->command = byte;
            p->state = ST_OPT;
        } else {
            /* Anything else (NOP, BREAK, ...): swallowed. */
            p->state = ST_DATA;
        }
        break;

    case ST_OPT:
        /*
         * The answer is the mirror with the other sign: WILL is
         * answered with DONT, DO with WONT. WONT and DONT need no
         * answer — the client has already said it will not do it.
         */
        if (p->command == TELNET_WILL) {
            reply_byte(reply, reply_max, reply_len, TELNET_IAC);
            reply_byte(reply, reply_max, reply_len, TELNET_DONT);
            reply_byte(reply, reply_max, reply_len, byte);
        } else if (p->command == TELNET_DO) {
            reply_byte(reply, reply_max, reply_len, TELNET_IAC);
            reply_byte(reply, reply_max, reply_len, TELNET_WONT);
            reply_byte(reply, reply_max, reply_len, byte);
        }

        p->state = ST_DATA;
        break;

    case ST_SUB:
        if (byte == TELNET_IAC) {
            p->state = ST_SUB_IAC;
        }
        break;

    case ST_SUB_IAC:
        p->state = (byte == TELNET_SE) ? ST_DATA : ST_SUB;
        break;

    case ST_DATA:
    default:
        feed_data(p, byte);
        break;
    }
}

bool telnet_parser_take_line(struct telnet_parser *p, char *line, size_t max)
{
    size_t n;

    if (!p->line_ready || line == NULL || max == 0) {
        return false;
    }

    n = p->len;
    if (n > max - 1) {
        n = max - 1;
    }

    memcpy(line, p->line, n);
    line[n] = '\0';

    p->line_ready = false;
    p->len = 0;

    return true;
}
