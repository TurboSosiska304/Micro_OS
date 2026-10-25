/*
 * Micro OS — the line part of telnet, as arithmetic.
 *
 * Telnet wraps its control in the byte 0xFF (IAC): options are
 * negotiated before the session and the client expects the server to
 * have an opinion about them. The opinion of this server is the
 * simplest one there is — everything is refused, because a shell over
 * the air needs no options: the client keeps its own echo and its own
 * line editing, and the bytes it sends come out as they are.
 *
 * So the parser has three jobs: throw the negotiation away and answer
 * it with a refusal, keep the lines of a person apart (CR LF, and a
 * line that grew too long is thrown away whole, like the shell does),
 * and never let a byte of the protocol leak into a command. All of it
 * is pure arithmetic with no sockets in it, which is why the host
 * tests compile the file themselves and push bytes through it.
 */
#ifndef MICRO_OS_TELNET_TELNET_PROTO_H
#define MICRO_OS_TELNET_TELNET_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TELNET_IAC 0xFF
#define TELNET_DONT 0xFE
#define TELNET_DO 0xFD
#define TELNET_WONT 0xFC
#define TELNET_WILL 0xFB
#define TELNET_SB 0xFA
#define TELNET_SE 0xF0

/* The same line the shell reads from the UART. */
#define TELNET_LINE_MAX 128

/* The most a single byte can answer: IAC, the command, the option. */
#define TELNET_REPLY_MAX 8

/*
 * The parser is fed byte by byte: the answers of the negotiation come
 * out in reply, and a line of a person is taken out with
 * telnet_parser_take_line() after it is finished. The parser keeps
 * nothing but its own state — a stream can be split anywhere between
 * the calls.
 */
struct telnet_parser {
    char line[TELNET_LINE_MAX];
    size_t len;
    bool line_ready;
    bool too_long;
    uint8_t state;
    uint8_t command; /* the last IAC command, for the option that follows */
};

void telnet_parser_init(struct telnet_parser *p);

/*
 * Feed one byte. The answer of the refusal (up to TELNET_REPLY_MAX
 * bytes, usually none) is appended to reply; *reply_len grows by the
 * number of bytes written. The buffers may be the same in every call —
 * the caller sends what came out before the next byte is fed.
 */
void telnet_parser_feed(struct telnet_parser *p, uint8_t byte, char *reply, size_t reply_max,
                        size_t *reply_len);

/*
 * Take the line that is ready: true when one was — NUL-terminated and
 * copied into line (at most max bytes, the rest is not carried over).
 * The line is taken right after the feed of the byte that finished it
 * — that is the order the server keeps. A line that grew past
 * TELNET_LINE_MAX is thrown away whole: the command of a person who
 * typed too much is not a command at all.
 */
bool telnet_parser_take_line(struct telnet_parser *p, char *line, size_t max);

#endif /* MICRO_OS_TELNET_TELNET_PROTO_H */
