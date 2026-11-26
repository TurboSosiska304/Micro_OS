#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/task.h"
#include "kshell/kshell.h"
#include "net/dns.h"
#include "net/http.h"
#include "net/mqtt.h"
#include "net/net.h"
#include "net/net_http.h"
#include "net/ntp.h"

/*
 * A port of the loopback for this process, taken once. Two worktrees of a
 * host run this suite at the same time and a number written in the tree
 * belongs to whichever of them bound it first; the kernel is asked for a
 * listener and the port it takes is the one this process uses.
 */
static k_u16_t test_free_port(void)
{
    static k_u16_t s_candidate;

    if (s_candidate == 0) {
        s_candidate = (k_u16_t)(18100u + (k_u32_t)(k_sysinfo_uptime_ms() % 5000u));
    }

    {
        unsigned attempt;

        for (attempt = 0; attempt < 500; attempt++) {
            k_server_t *probe;

            if (s_candidate < 18100u || s_candidate >= 24000u) {
                s_candidate = 18100u;
            }

            probe = k_server_listen(s_candidate);

            if (probe != NULL) {
                k_server_close(probe);
                return s_candidate++;
            }

            s_candidate++;
        }
    }

    /* A machine with every number of the range taken is a machine this test
     * cannot use; the caller reads the zero and fails with its own words. */
    return 0;
}

/* --- the transport that replays a canned answer of a server --- */

static const char *g_answer;
static k_size_t g_answer_len;
static k_size_t g_answer_pos;
static k_size_t g_answer_chunk;
static char g_request[256];
static char g_host[HTTP_HOST_MAX];
static int g_port;

static int fake_open(const struct http_transport *self, const char *host, int port, void **ctx)
{
    k_size_t len = strlen(host);

    (void)self;

    if (len >= sizeof(g_host)) {
        return -1;
    }

    memcpy(g_host, host, len + 1);
    g_port = port;
    g_answer_pos = 0;

    (void)ctx;

    return 0;
}

static int fake_send(void *ctx, const void *data, k_size_t size)
{
    k_size_t len = size < sizeof(g_request) - 1 ? size : sizeof(g_request) - 1;

    memcpy(g_request, data, len);
    g_request[len] = '\0';

    (void)ctx;

    return (int)size;
}

static int fake_recv(void *ctx, void *data, k_size_t size)
{
    k_size_t take = g_answer_len - g_answer_pos;

    (void)ctx;

    if (take > size) {
        take = size;
    }
    if (take > g_answer_chunk) {
        take = g_answer_chunk;
    }
    if (take == 0) {
        return 0;
    }

    memcpy(data, g_answer + g_answer_pos, take);
    g_answer_pos += take;

    return (int)take;
}

static void fake_close(void *ctx)
{
    (void)ctx;
}

static const struct http_transport g_transport = {
    .open = fake_open,
    .send = fake_send,
    .recv = fake_recv,
    .close = fake_close,
};

/* --- the tests --- */

static void test_url(void)
{
    struct http_url url;

    ktest_begin("http: the URL gives the host, the port and the path");

    KT_CHECK_INT_EQ(http_parse_url("http://example.com", &url), HTTP_OK);
    KT_CHECK_STR_EQ(url.host, "example.com");
    KT_CHECK_INT_EQ(url.port, 80);
    KT_CHECK_STR_EQ(url.path, "/");

    KT_CHECK_INT_EQ(http_parse_url("http://example.com:8080/a/b?x=1", &url), HTTP_OK);
    KT_CHECK_INT_EQ(url.port, 8080);
    KT_CHECK_STR_EQ(url.path, "/a/b?x=1");
    KT_CHECK(!url.tls);

    KT_CHECK_INT_EQ(http_parse_url("https://example.com/", &url), HTTP_OK);
    KT_CHECK_INT_EQ(url.port, 443);
    KT_CHECK_STR_EQ(url.host, "example.com");
    KT_CHECK_STR_EQ(url.path, "/");
    KT_CHECK(url.tls);

    KT_CHECK_INT_EQ(http_parse_url("https://example.com:8443/a", &url), HTTP_OK);
    KT_CHECK_INT_EQ(url.port, 8443);
    KT_CHECK(url.tls);

    KT_CHECK_INT_EQ(http_parse_url("https://", &url), HTTP_INVALID);
    KT_CHECK_INT_EQ(http_parse_url("https://example.com:0/", &url), HTTP_INVALID);
    KT_CHECK_INT_EQ(http_parse_url("ftp://example.com/", &url), HTTP_INVALID);
    KT_CHECK_INT_EQ(http_parse_url("http://", &url), HTTP_INVALID);
    KT_CHECK_INT_EQ(http_parse_url("http://example.com:0/", &url), HTTP_INVALID);
    KT_CHECK_INT_EQ(http_parse_url("http://example.com:99999/", &url), HTTP_INVALID);
}

static void test_get(void)
{
    char body[64];
    struct http_response response;

    ktest_begin("http: the answer of a server becomes the status and the body");

    g_answer = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    g_answer_len = strlen(g_answer);
    g_answer_chunk = 7; /* the answer arrives in pieces, headers split across them */
    g_request[0] = '\0';

    KT_CHECK_INT_EQ(http_get(&g_transport, "http://example.com/x", body, sizeof(body), &response),
                    HTTP_OK);
    KT_CHECK_INT_EQ(response.status, 200);
    KT_CHECK_INT_EQ(response.length, 5);
    KT_CHECK_STR_EQ(body, "hello");
    KT_CHECK(strstr(g_request, "GET /x HTTP/1.0") != NULL);
    KT_CHECK(strstr(g_request, "Host: example.com") != NULL);
    KT_CHECK_STR_EQ(g_host, "example.com");
    KT_CHECK_INT_EQ(g_port, 80);

    /* Without Content-Length the body ends with the connection. */
    g_answer = "HTTP/1.0 404 Not Found\r\n\r\nnot found";
    g_answer_len = strlen(g_answer);
    g_answer_chunk = 3;

    KT_CHECK_INT_EQ(http_get(&g_transport, "http://example.com/", body, sizeof(body), &response),
                    HTTP_OK);
    KT_CHECK_INT_EQ(response.status, 404);
    KT_CHECK_STR_EQ(body, "not found");

    /* A body longer than the buffer is cut, not overflowed, and the
     * answer names the cut instead of lying about the file. */
    g_answer = "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n0123456789";
    g_answer_len = strlen(g_answer);
    g_answer_chunk = 16;

    KT_CHECK_INT_EQ(http_get(&g_transport, "http://example.com/", body, 8, &response), HTTP_OK);
    KT_CHECK_INT_EQ(response.length, 7);
    KT_CHECK(response.truncated);
    KT_CHECK_INT_EQ(response.total, 10);
    KT_CHECK_STR_EQ(body, "0123456");

    /* A 600-byte body fits in a buffer that is large enough, and a 256-byte
     * buffer reports the cut with the total the server named - the console
     * must not lie about the size of the file. */
    {
        char big[700];
        char small[257];
        char payload[601];
        struct http_response big_r;
        struct http_response small_r;
        unsigned i;

        for (i = 0; i < 600; i++) {
            payload[i] = (char)('a' + (int)(i % 26));
        }
        payload[600] = '\0';

        snprintf(big, sizeof(big), "HTTP/1.0 200 OK\r\nContent-Length: 600\r\n\r\n%s", payload);
        g_answer = big;
        g_answer_len = strlen(g_answer);
        g_answer_chunk = 64;

        KT_CHECK_INT_EQ(
            http_get(&g_transport, "http://example.com/big", small, sizeof(small), &small_r),
            HTTP_OK);
        KT_CHECK_INT_EQ(small_r.length, 256);
        KT_CHECK(small_r.truncated);
        KT_CHECK_INT_EQ(small_r.total, 600);

        g_answer = big;
        g_answer_len = strlen(g_answer);
        g_answer_chunk = 64;

        KT_CHECK_INT_EQ(
            http_get(&g_transport, "http://example.com/big", payload, sizeof(payload), &big_r),
            HTTP_OK);
        KT_CHECK_INT_EQ(big_r.length, 600);
        KT_CHECK(!big_r.truncated);
        KT_CHECK_INT_EQ(big_r.total, 600);
    }
}

static void put_be32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value >> 24);
    p[1] = (k_u8_t)(value >> 16);
    p[2] = (k_u8_t)(value >> 8);
    p[3] = (k_u8_t)value;
}

/* --- the fake server of the stream: one answer per connection --- */

#define STREAM_ANSWERS 8

struct fake_server {
    const char *answers[STREAM_ANSWERS];
    k_size_t count;
    k_size_t opened;
    k_size_t pos;
    k_size_t chunk;
    char request[512];
    int closes;
};

static struct fake_server g_server;

static int stream_open(const struct http_transport *self, const char *host, int port, void **ctx)
{
    (void)self;
    if (g_server.opened >= g_server.count) {
        return -1;
    }

    g_server.opened++;
    g_server.pos = 0;

    (void)host;
    (void)port;
    (void)ctx;

    return 0;
}

static int stream_send(void *ctx, const void *data, k_size_t size)
{
    k_size_t len = size < sizeof(g_server.request) - 1u ? size : sizeof(g_server.request) - 1u;

    memcpy(g_server.request, data, len);
    g_server.request[len] = '\0';

    (void)ctx;

    return (int)size;
}

static int stream_recv(void *ctx, void *data, k_size_t size)
{
    const char *answer = g_server.answers[g_server.opened - 1u];
    k_size_t total = strlen(answer);
    k_size_t take = total - g_server.pos;

    (void)ctx;

    if (take > size) {
        take = size;
    }
    if (take > g_server.chunk) {
        take = g_server.chunk;
    }
    if (take == 0) {
        return 0;
    }

    memcpy(data, answer + g_server.pos, take);
    g_server.pos += take;

    return (int)take;
}

static void stream_close(void *ctx)
{
    g_server.closes++;

    (void)ctx;
}

static const struct http_transport g_stream_transport = {
    .open = stream_open,
    .send = stream_send,
    .recv = stream_recv,
    .close = stream_close,
};

/* The sink of the stream: a buffer, with the reset of a file that starts
 * again. */
struct stream_sink {
    char text[256];
    k_size_t used;
    int resets;
};

static int sink_write(void *ctx, const void *data, k_size_t size)
{
    struct stream_sink *sink = (struct stream_sink *)ctx;

    if (sink->used + size >= sizeof(sink->text)) {
        return -1;
    }

    memcpy(sink->text + sink->used, data, size);
    sink->used += size;
    sink->text[sink->used] = '\0';

    return (int)size;
}

static int sink_reset(void *ctx)
{
    struct stream_sink *sink = (struct stream_sink *)ctx;

    sink->used = 0;
    sink->text[0] = '\0';
    sink->resets++;

    return 0;
}

static void server_reset(void)
{
    memset(&g_server, 0, sizeof(g_server));
    g_server.chunk = 1024;
}

static void stream_init(struct http_stream *stream, struct stream_sink *sink)
{
    memset(stream, 0, sizeof(*stream));
    memset(sink, 0, sizeof(*sink));

    stream->transport = &g_stream_transport;
    stream->sink.ctx = sink;
    stream->sink.write = sink_write;
    stream->sink.reset = sink_reset;
    stream->max_redirects = HTTP_REDIRECT_MAX;
}

static void test_stream(void)
{
    struct http_stream stream;
    struct stream_sink sink;

    ktest_begin("http: the stream reads a body of a length, of chunks and of a redirect");

    /* --- Content-Length --- */
    server_reset();
    g_server.answers[0] =
        "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world";
    g_server.count = 1;
    g_server.chunk = 3; /* the body arrives in pieces of three */

    stream_init(&stream, &sink);
    stream.url = "http://example.com/file";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK_INT_EQ(stream.status, 200);
    KT_CHECK_INT_EQ((long long)stream.total, 11);
    KT_CHECK_INT_EQ((long long)stream.length, 11);
    KT_CHECK_STR_EQ(sink.text, "hello world");
    KT_CHECK(strstr(g_server.request, "GET /file HTTP/1.1") != NULL);
    KT_CHECK(strstr(g_server.request, "Host: example.com") != NULL);
    KT_CHECK_INT_EQ(g_server.closes, 1);

    /* --- chunked --- */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n";
    g_server.count = 1;
    g_server.chunk = 4;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/chunked";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK(stream.chunked);
    KT_CHECK_INT_EQ((long long)stream.length, 11);
    KT_CHECK_STR_EQ(sink.text, "hello world");

    /* A chunk that says more than it carries is a body that ended early. */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/chunked";
    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_SHORT);

    /* A size that is not hexadecimal is not a chunk. */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/chunked";
    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_PROTOCOL);

    /* --- a redirect, then the page --- */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n";
    g_server.answers[1] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nmoved";
    g_server.count = 2;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/old";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK_INT_EQ(stream.status, 200);
    KT_CHECK_INT_EQ((int)stream.redirects, 1);
    KT_CHECK_STR_EQ(sink.text, "moved");
    KT_CHECK(strstr(g_server.request, "GET /next HTTP/1.1") != NULL);

    /* A redirect to another host is a whole URL. */
    server_reset();
    g_server.answers[0] =
        "HTTP/1.1 301 Moved\r\nLocation: http://mirror.example.com:8080/x\r\n\r\n";
    g_server.answers[1] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    g_server.count = 2;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/";
    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK_STR_EQ(sink.text, "ok");
    KT_CHECK(strstr(g_server.request, "Host: mirror.example.com") != NULL);

    /* --- a chain that does not end --- */
    server_reset();
    for (int i = 0; i < STREAM_ANSWERS; i++) {
        g_server.answers[i] = "HTTP/1.1 302 Found\r\nLocation: /again\r\n\r\n";
    }
    g_server.count = STREAM_ANSWERS;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/loop";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_REDIRECT);
    KT_CHECK_INT_EQ((int)stream.redirects, HTTP_REDIRECT_MAX);

    /* --- a body that ends before its Content-Length --- */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/cut";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_SHORT);
    KT_CHECK_STR_EQ(sink.text, "short");

    /* --- a refusal is not a file --- */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot there";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/missing";

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK_INT_EQ(stream.status, 404);
    KT_CHECK_INT_EQ((long long)stream.length, 0);
    KT_CHECK_STR_EQ(sink.text, "");

    /* --- Range: a 206 continues the file, a 200 starts it again --- */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 206 Partial\r\nContent-Length: 5\r\n\r\nworld";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/file";
    stream.resume_from = 6;
    sink.used = 6;
    memcpy(sink.text, "hello ", 6);
    sink.text[6] = '\0';

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK(stream.resumed);
    KT_CHECK_STR_EQ(sink.text, "hello world");
    KT_CHECK(strstr(g_server.request, "Range: bytes=6-") != NULL);

    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/file";
    stream.resume_from = 6;

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_OK);
    KT_CHECK(!stream.resumed);
    KT_CHECK_INT_EQ(sink.resets, 1);
    KT_CHECK_STR_EQ(sink.text, "again");

    /* A sink that cannot start again makes the answer an error. */
    server_reset();
    g_server.answers[0] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain";
    g_server.count = 1;

    stream_init(&stream, &sink);
    stream.url = "http://example.com/file";
    stream.resume_from = 6;
    stream.sink.reset = NULL;

    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_ERROR); /* the sink cannot start again */

    /* --- the transport that is not there --- */
    stream_init(&stream, &sink);
    stream.transport = NULL;
    stream.url = "http://example.com/";
    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_INVALID);

    /*
     * https without a trust store or without the TLS layer: the answer is
     * the same code, and the reason is in net_tls_last_error() - the image
     * either has no mbedTLS or has no roots of /etc/ssl/certs.pem (this
     * test volume has none).
     */
    stream_init(&stream, &sink);
    stream.url = "https://example.com/";
    KT_CHECK_INT_EQ(http_stream(&stream), HTTP_TLS);
}

static void test_ntp(void)
{
    k_u8_t packet[NTP_PACKET_SIZE];
    k_u64_t seconds;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    ktest_begin("ntp: the packet, the answer and the calendar of the server");

    ntp_build_request(packet);
    KT_CHECK_INT_EQ(packet[0], 0x23); /* version 4, mode 3 */
    KT_CHECK_INT_EQ(packet[1], 0);
    KT_CHECK_INT_EQ(packet[40], 0); /* the transmit stamp is ours to leave empty */

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x24; /* version 4, mode 4: the server answers */
    packet[1] = 1;    /* stratum 1: a real time, not a kiss */
    put_be32(packet + 40, 4003216496u);

    KT_CHECK_INT_EQ(ntp_parse_response(packet, &seconds), 0);
    KT_CHECK_INT_EQ((long long)seconds, 1794227696LL);

    KT_CHECK_INT_EQ(ntp_unix_to_datetime(seconds, &year, &month, &day, &hour, &minute, &second), 0);
    KT_CHECK_INT_EQ(year, 2026);
    KT_CHECK_INT_EQ(month, 11);
    KT_CHECK_INT_EQ(day, 9);
    KT_CHECK_INT_EQ(hour, 12);
    KT_CHECK_INT_EQ(minute, 34);
    KT_CHECK_INT_EQ(second, 56);

    /* The epoch and the last year the clock of the board keeps. */
    KT_CHECK_INT_EQ(ntp_unix_to_datetime(0, &year, &month, &day, &hour, &minute, &second), 0);
    KT_CHECK_INT_EQ(year, 1970);
    KT_CHECK_INT_EQ(month, 1);
    KT_CHECK_INT_EQ(day, 1);

    KT_CHECK_INT_EQ(
        ntp_unix_to_datetime(4102444799ULL, &year, &month, &day, &hour, &minute, &second), 0);
    KT_CHECK_INT_EQ(year, 2099);
    KT_CHECK_INT_EQ(month, 12);
    KT_CHECK_INT_EQ(day, 31);
    KT_CHECK_INT_EQ(hour, 23);
    KT_CHECK_INT_EQ(minute, 59);
    KT_CHECK_INT_EQ(second, 59);

    /* A kiss-o'-death, a zero stamp and a request are not a time. */
    packet[1] = 0;
    KT_CHECK(ntp_parse_response(packet, &seconds) != 0);

    packet[1] = 1;
    put_be32(packet + 40, 0);
    KT_CHECK(ntp_parse_response(packet, &seconds) != 0);

    packet[0] = 0x23;
    put_be32(packet + 40, 4003216496u);
    KT_CHECK(ntp_parse_response(packet, &seconds) != 0);
}

/*
 * The port under the two clients, exercised for real: a server on a port of
 * the loopback, a client of this very task, a line in both directions. The
 * port is a fixed one and SO_REUSEADDR is the business of the port, so a
 * session of the run before does not stand in the way.
 */

/*
 * A listener on a port that is free: another test of the same tree (two
 * worktrees, two runs of `make test`) may hold the fixed one, and a test
 * that fails because of a neighbour is not a test of this code.
 */
static k_server_t *test_listen(int *port)
{
    int candidate;

    for (candidate = *port; candidate < *port + 100; candidate++) {
        k_server_t *server = k_server_listen(candidate);

        if (server != NULL) {
            *port = candidate;
            return server;
        }
    }

    return NULL;
}

/*
 * A session to the door of the test. The port takes the connection as soon
 * as its listener is up and the task of the server may be scheduled late on
 * a host that builds three worktrees at once: the connect is tried a few
 * times before it is a failure of the case.
 */
static k_socket_t *connect_to_door(k_u16_t port)
{
    unsigned attempt;

    for (attempt = 0; attempt < 20; attempt++) {
        k_socket_t *client = k_socket_connect("127.0.0.1", port, 2000);

        if (client != NULL) {
            return client;
        }

        k_sched_sleep_ms(100);
    }

    return NULL;
}

/* The ports the two doors of this case took: found once, from the kernel. */
static k_u16_t page_port;
static k_u16_t command_port;

static void test_loopback(void)
{
    static const char question[] = "ping";
    static const char answer_text[] = "pong";
    char answer[16];
    k_server_t *server;
    k_socket_t *client = NULL;
    k_socket_t *session = NULL;
    int port = 45123;

    ktest_begin("net: a session of the loopback carries a line both ways");

    server = test_listen(&port);
    KT_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }

    client = k_socket_connect("127.0.0.1", port, 2000);
    KT_CHECK(client != NULL);

    /* The handshake is complete once connect() returns: the listener of the
     * port accepts the client that is already waiting. */
    if (client != NULL) {
        session = k_server_accept(server, 2000);
        KT_CHECK(session != NULL);
    }

    if (client != NULL && session != NULL) {
        KT_CHECK_INT_EQ(k_socket_send(client, question, sizeof(question) - 1, true),
                        (int)sizeof(question) - 1);
        KT_CHECK_INT_EQ(k_socket_recv(session, answer, sizeof(answer)), (int)sizeof(question) - 1);
        KT_CHECK_INT_EQ(memcmp(answer, question, sizeof(question) - 1), 0);

        KT_CHECK_INT_EQ(k_socket_send(session, answer_text, sizeof(answer_text) - 1, true),
                        (int)sizeof(answer_text) - 1);
        KT_CHECK_INT_EQ(k_socket_recv(client, answer, sizeof(answer)),
                        (int)sizeof(answer_text) - 1);
        KT_CHECK_INT_EQ(memcmp(answer, answer_text, sizeof(answer_text) - 1), 0);
    }

    /* Closing a session is also the test of the close itself: the reader on
     * the other side sees the end of it. */
    k_socket_close(session);
    if (client != NULL) {
        KT_CHECK_INT_EQ(k_socket_recv(client, answer, sizeof(answer)), 0);
    }
    k_socket_close(client);
    k_server_close(server);
}

/* --- the fake broker of the MQTT test --- */

/*
 * The broker is a task of its own: the client waits inside `mqtt_connect`
 * and `mqtt_publish` for the answer of the broker, so somebody has to answer
 * while the task that calls them sleeps. It speaks only the packets of the
 * test — one session at a time, no store, no wildcards — and lives for as
 * long as the test needs it.
 */
#define TEST_MQTT_PORT 45884
#define TEST_MQTT_BODY_MAX 512
#define TEST_BROKER_TIMEOUT_MS 2000

static char g_mqtt_topic[MQTT_TOPIC_MAX + 1];
static char g_mqtt_payload[MQTT_PAYLOAD_MAX + 1];
static int g_mqtt_payload_len;
static int g_mqtt_connects;
static int g_mqtt_publishes;
static int g_mqtt_subscribes;
static int g_mqtt_pubacks;
static int g_mqtt_pings;
static int g_mqtt_stop;

static k_size_t broker_varint(k_u8_t *buf, k_size_t value)
{
    k_size_t used = 0;

    do {
        k_u8_t byte = (k_u8_t)(value % 128u);

        value /= 128u;
        if (value > 0) {
            byte |= 0x80;
        }

        buf[used++] = byte;
    } while (value > 0);

    return used;
}

static int broker_read_exact(k_socket_t *session, k_u8_t *buf, k_size_t length)
{
    k_u32_t waited = 0;
    k_size_t got = 0;

    while (got < length) {
        int n = k_socket_recv(session, buf + got, length - got);

        if (n > 0) {
            got += (k_size_t)n;
            continue;
        }

        if (n == 0 || waited >= TEST_BROKER_TIMEOUT_MS) {
            return -1;
        }

        k_sched_sleep_ms(10);
        waited += 10;
    }

    return 0;
}

static int broker_read_packet(k_socket_t *session, k_u8_t *type, k_u8_t *flags, k_u8_t *body,
                              k_size_t *length)
{
    k_u8_t header;
    k_u8_t byte;
    k_size_t remaining = 0;
    k_size_t multiplier = 1;
    int i;

    if (broker_read_exact(session, &header, 1) != 0) {
        return -1;
    }

    for (i = 0; i < 4; i++) {
        if (broker_read_exact(session, &byte, 1) != 0) {
            return -1;
        }

        remaining += (k_size_t)(byte & 0x7f) * multiplier;
        if ((byte & 0x80) == 0) {
            break;
        }

        multiplier *= 128u;
    }

    if (i == 4 || remaining > TEST_MQTT_BODY_MAX) {
        return -1;
    }

    if (remaining > 0 && broker_read_exact(session, body, remaining) != 0) {
        return -1;
    }

    *type = (k_u8_t)(header >> 4);
    *flags = (k_u8_t)(header & 0x0f);
    *length = remaining;

    return 0;
}

static int broker_send(k_socket_t *session, const k_u8_t *data, k_size_t length)
{
    return k_socket_send(session, data, length, true) == (int)length ? 0 : -1;
}

static void broker_session(k_socket_t *session)
{
    k_u8_t body[TEST_MQTT_BODY_MAX];
    k_u8_t out[TEST_MQTT_BODY_MAX];
    k_u8_t type;
    k_u8_t flags;
    k_size_t length;
    k_size_t topic_length;
    k_size_t payload_length;
    k_size_t pos;
    k_u16_t id;
    int qos;

    /* CONNECT: the protocol name, level 4 and a clean session are what we speak. */
    if (broker_read_packet(session, &type, &flags, body, &length) != 0 || type != 1) {
        return;
    }

    if (length < 12 || body[1] != 4 || memcmp(body + 2, "MQTT", 4) != 0 || body[6] != 4) {
        return;
    }

    __atomic_add_fetch(&g_mqtt_connects, 1, __ATOMIC_RELEASE);

    out[0] = 0x20; /* CONNACK */
    out[1] = 0x02;
    out[2] = 0x00;
    out[3] = 0x00;
    if (broker_send(session, out, 4) != 0) {
        return;
    }

    for (;;) {
        if (broker_read_packet(session, &type, &flags, body, &length) != 0) {
            return;
        }

        switch (type) {
        case 3: /* PUBLISH */
            if (length < 2) {
                return;
            }

            topic_length = ((k_size_t)body[0] << 8) | body[1];
            pos = 2;
            qos = (flags >> 1) & 0x03;

            if (topic_length == 0 || topic_length > MQTT_TOPIC_MAX || pos + topic_length > length) {
                return;
            }

            if (qos == 1) {
                if (pos + topic_length + 2 > length) {
                    return;
                }
                id = (k_u16_t)(((k_u16_t)body[pos + topic_length] << 8) |
                               body[pos + topic_length + 1]);
            } else {
                id = 0;
            }

            /* The packet id sits between the topic and the payload. */
            pos += topic_length + (qos == 1 ? 2 : 0);
            payload_length = length - pos;
            if (payload_length > MQTT_PAYLOAD_MAX) {
                return;
            }

            memcpy(g_mqtt_topic, body + 2, topic_length);
            g_mqtt_topic[topic_length] = '\0';
            memcpy(g_mqtt_payload, body + pos, payload_length);
            g_mqtt_payload[payload_length] = '\0';
            g_mqtt_payload_len = (int)payload_length;

            if (qos == 1) {
                out[0] = 0x40; /* PUBACK */
                out[1] = 0x02;
                out[2] = (k_u8_t)(id >> 8);
                out[3] = (k_u8_t)id;
                if (broker_send(session, out, 4) != 0) {
                    return;
                }
            }

            /* The count is the last word about the message: every field above
             * is complete before the test task can see it. */
            __atomic_add_fetch(&g_mqtt_publishes, 1, __ATOMIC_RELEASE);
            break;

        case 8: /* SUBSCRIBE */
            if (length < 5) {
                return;
            }

            id = (k_u16_t)(((k_u16_t)body[0] << 8) | body[1]);
            topic_length = ((k_size_t)body[2] << 8) | body[3];

            if (topic_length == 0 || topic_length > MQTT_TOPIC_MAX ||
                4 + topic_length + 1 > length) {
                return;
            }

            qos = body[4 + topic_length];
            __atomic_add_fetch(&g_mqtt_subscribes, 1, __ATOMIC_RELEASE);

            out[0] = 0x90; /* SUBACK */
            out[1] = 0x03;
            out[2] = (k_u8_t)(id >> 8);
            out[3] = (k_u8_t)id;
            out[4] = (k_u8_t)qos;
            if (broker_send(session, out, 5) != 0) {
                return;
            }

            /* One message of the broker on the filter just subscribed: the
             * client reads it from `mqtt_poll`, acknowledges it and only then
             * does the broker see the PUBACK in the loop below. */
            pos = 0;
            out[pos++] = 0x32; /* PUBLISH, QoS 1 */
            pos += broker_varint(out + pos, 2 + topic_length + 2 + 11);
            out[pos++] = (k_u8_t)(topic_length >> 8);
            out[pos++] = (k_u8_t)topic_length;
            memcpy(out + pos, body + 4, topic_length);
            pos += topic_length;
            out[pos++] = 0x00; /* the packet id of the broker's message */
            out[pos++] = 0x07;
            memcpy(out + pos, "from-broker", 11);
            pos += 11;
            if (broker_send(session, out, pos) != 0) {
                return;
            }
            break;

        case 4: /* PUBACK of the message above */
            if (length != 2) {
                return;
            }
            __atomic_add_fetch(&g_mqtt_pubacks, 1, __ATOMIC_RELEASE);
            break;

        case 12: /* PINGREQ */
            if (length != 0) {
                return;
            }
            __atomic_add_fetch(&g_mqtt_pings, 1, __ATOMIC_RELEASE);
            out[0] = 0xd0; /* PINGRESP */
            out[1] = 0x00;
            if (broker_send(session, out, 2) != 0) {
                return;
            }
            break;

        case 14: /* DISCONNECT */
            return;

        default:
            return;
        }
    }
}

static void mqtt_broker_task(void *arg)
{
    k_server_t *server = arg;

    while (!__atomic_load_n(&g_mqtt_stop, __ATOMIC_ACQUIRE)) {
        k_socket_t *session = k_server_accept(server, 100);

        if (session == NULL) {
            continue;
        }

        broker_session(session);
        k_socket_close(session);
    }

    k_server_close(server);
}

static bool mqtt_wait_for(int *value, int want, k_u32_t timeout_ms)
{
    k_u32_t waited = 0;

    while (waited <= timeout_ms) {
        if (__atomic_load_n(value, __ATOMIC_ACQUIRE) >= want) {
            return true;
        }

        k_sched_sleep_ms(10);
        waited += 10;
    }

    return false;
}

static void mqtt_broker_stop(struct task *broker)
{
    __atomic_store_n(&g_mqtt_stop, 1, __ATOMIC_RELEASE);

    if (broker != NULL) {
        (void)task_wait(broker->pid, NULL, 5000);
    }
}

/*
 * The whole client over the loopback of the port: a broker of the test says
 * CONNACK, takes the two publishes, answers SUBSCRIBE with a message of its
 * own and answers the pings. The command of the shell is then the second
 * session of the same broker, so the one-shot path is covered too.
 */
static void test_mqtt(void)
{
    static const char topic[] = "test/one";
    static const char acked_topic[] = "test/two";
    static const char filter[] = "test/three";
    struct mqtt_client client;
    struct mqtt_message message;
    struct task *broker;
    k_server_t *server;
    char command[96];
    int rc;
    int port = TEST_MQTT_PORT;

    ktest_begin("mqtt: a fake broker takes CONNECT, PUBLISH, SUBSCRIBE, PUBACK and PINGREQ");

    memset(g_mqtt_topic, 0, sizeof(g_mqtt_topic));
    memset(g_mqtt_payload, 0, sizeof(g_mqtt_payload));
    __atomic_store_n(&g_mqtt_payload_len, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_connects, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_publishes, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_subscribes, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_pubacks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_pings, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mqtt_stop, 0, __ATOMIC_RELEASE);

    server = test_listen(&port);
    KT_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }

    broker = task_create("mqtt-broker", mqtt_broker_task, server, TASK_PRIO_DEFAULT);
    KT_CHECK(broker != NULL);
    if (broker == NULL) {
        k_server_close(server);
        return;
    }

    rc = mqtt_connect(&client, "127.0.0.1", port, "microos-test", 1);
    KT_CHECK_INT_EQ(rc, MQTT_OK);
    if (rc != MQTT_OK) {
        mqtt_broker_stop(broker);
        return;
    }

    KT_CHECK(mqtt_wait_for(&g_mqtt_connects, 1, 2000));

    /* QoS 0: the command of the shell is the same path. */
    KT_CHECK_INT_EQ(mqtt_publish(&client, topic, "hello", 5, 0), MQTT_OK);
    KT_CHECK(mqtt_wait_for(&g_mqtt_publishes, 1, 2000));
    KT_CHECK_STR_EQ(g_mqtt_topic, topic);
    KT_CHECK_STR_EQ(g_mqtt_payload, "hello");
    KT_CHECK_INT_EQ(g_mqtt_payload_len, 5);

    /* QoS 1: the call does not return before the PUBACK of the broker. */
    KT_CHECK_INT_EQ(mqtt_publish(&client, acked_topic, "with-ack", 8, 1), MQTT_OK);
    KT_CHECK(mqtt_wait_for(&g_mqtt_publishes, 2, 2000));
    KT_CHECK_STR_EQ(g_mqtt_topic, acked_topic);
    KT_CHECK_STR_EQ(g_mqtt_payload, "with-ack");

    /* SUBSCRIBE QoS 1, and the message the broker sends on the new filter. */
    KT_CHECK_INT_EQ(mqtt_subscribe(&client, filter, 1), MQTT_OK);
    KT_CHECK(mqtt_wait_for(&g_mqtt_subscribes, 1, 2000));

    rc = mqtt_poll(&client, &message);
    KT_CHECK_INT_EQ(rc, 1);
    if (rc == 1) {
        KT_CHECK_STR_EQ(message.topic, filter);
        KT_CHECK_STR_EQ((const char *)message.payload, "from-broker");
        KT_CHECK_INT_EQ(message.length, 11);
        KT_CHECK_INT_EQ(message.qos, 1);
    }

    /* The reader acknowledged the message of the broker by itself. */
    KT_CHECK(mqtt_wait_for(&g_mqtt_pubacks, 1, 2000));

    /* Half of the keepalive has passed: the clock sends the PINGREQ itself. */
    k_sched_sleep_ms(600);
    KT_CHECK_INT_EQ(mqtt_keepalive(&client), 1);
    KT_CHECK(mqtt_wait_for(&g_mqtt_pings, 1, 2000));
    KT_CHECK_INT_EQ(mqtt_keepalive(&client), 0);

    KT_CHECK_INT_EQ(mqtt_ping(&client), MQTT_OK);
    KT_CHECK(mqtt_wait_for(&g_mqtt_pings, 2, 2000));

    mqtt_disconnect(&client);
    KT_CHECK(!client.connected);

    /* The one-shot command, against a second session of the same broker. */
    KT_CHECK_INT_EQ(kshell_execute("mqtt"), KSHELL_USAGE);

    snprintf(command, sizeof(command), "mqtt 127.0.0.1 %d cmd/topic hello", port);
    KT_CHECK_INT_EQ(kshell_execute(command), KSHELL_OK);
    KT_CHECK(mqtt_wait_for(&g_mqtt_publishes, 3, 2000));
    KT_CHECK_STR_EQ(g_mqtt_topic, "cmd/topic");
    KT_CHECK_STR_EQ(g_mqtt_payload, "hello");

    mqtt_broker_stop(broker);
    KT_CHECK_INT_EQ(__atomic_load_n(&g_mqtt_connects, __ATOMIC_ACQUIRE), 2);
}

/*
 * The resolver: the query of a name, the record of an answer and the cache.
 * The packets are made here byte by byte, because the wire format is the
 * contract and a server of the internet is not in the test.
 */
static k_size_t dns_test_name(k_u8_t *buffer, const char *name)
{
    k_size_t length = 0;
    const char *cursor = name;

    while (*cursor != '\0') {
        const char *dot = strchr(cursor, '.');
        k_size_t label = dot == NULL ? strlen(cursor) : (k_size_t)(dot - cursor);

        buffer[length++] = (k_u8_t)label;
        memcpy(buffer + length, cursor, label);
        length += label;
        cursor += label;

        if (*cursor == '.') {
            cursor++;
        }
    }

    buffer[length++] = 0u;

    return length;
}

/* The header and the question of an answer, for `answers` records to come. */
static k_size_t dns_test_begin(k_u8_t *packet, k_u16_t identifier, const char *name, int answers)
{
    k_size_t length = 12u;

    memset(packet, 0, DNS_PACKET_MAX);
    packet[0] = (k_u8_t)(identifier >> 8);
    packet[1] = (k_u8_t)identifier;
    packet[2] = 0x81u; /* an answer, recursion available */
    packet[3] = 0x80u; /* no error */
    packet[5] = 1u;    /* one question */
    packet[7] = (k_u8_t)answers;

    length += dns_test_name(packet + length, name);
    packet[length++] = 0u;
    packet[length++] = 1u; /* type A */
    packet[length++] = 0u;
    packet[length++] = 1u; /* class IN */

    return length;
}

static k_size_t dns_test_record(k_u8_t *packet, k_size_t length, k_u16_t type, k_u32_t ttl,
                                const k_u8_t *data, k_size_t data_length)
{
    packet[length++] = 0xC0u; /* the name of the question */
    packet[length++] = 0x0Cu;
    packet[length++] = (k_u8_t)(type >> 8);
    packet[length++] = (k_u8_t)type;
    packet[length++] = 0u;
    packet[length++] = 1u;
    packet[length++] = (k_u8_t)(ttl >> 24);
    packet[length++] = (k_u8_t)(ttl >> 16);
    packet[length++] = (k_u8_t)(ttl >> 8);
    packet[length++] = (k_u8_t)ttl;
    packet[length++] = (k_u8_t)(data_length >> 8);
    packet[length++] = (k_u8_t)data_length;
    memcpy(packet + length, data, data_length);

    return length + data_length;
}

static k_size_t dns_test_a(k_u8_t *packet, k_size_t length, k_u32_t address, k_u32_t ttl)
{
    k_u8_t data[4];

    put_be32(data, address);

    return dns_test_record(packet, length, 1u, ttl, data, sizeof(data));
}

static void test_dns(void)
{
    k_u8_t packet[DNS_PACKET_MAX];
    k_u8_t second[DNS_PACKET_MAX];
    struct dns_answer answer;
    k_u32_t address;
    k_u32_t left;
    k_size_t length;
    k_size_t i;
    char long_name[DNS_NAME_MAX + 8];

    ktest_begin("dns: the query of a name, the address of an answer and the cache");

    /* --- the query: a header, the labels, the root and the question */
    length = (k_size_t)dns_build_query("example.com", 0x1234, packet, sizeof(packet));
    KT_CHECK_INT_EQ((int)length, 29);
    KT_CHECK_INT_EQ(packet[0], 0x12);
    KT_CHECK_INT_EQ(packet[1], 0x34);
    KT_CHECK_INT_EQ(packet[2], 0x01); /* a query */
    KT_CHECK_INT_EQ(packet[3], 0x00); /* recursion wanted, no error */
    KT_CHECK_INT_EQ(packet[5], 1);    /* one question */
    KT_CHECK_INT_EQ(packet[7], 0);    /* no answer came with the query */
    KT_CHECK_INT_EQ(packet[12], 7);   /* the label of "example" */
    KT_CHECK(memcmp(packet + 13, "example", 7) == 0);
    KT_CHECK_INT_EQ(packet[20], 3);
    KT_CHECK(memcmp(packet + 21, "com", 3) == 0);
    KT_CHECK_INT_EQ(packet[24], 0); /* the root */
    KT_CHECK_INT_EQ(packet[25], 0);
    KT_CHECK_INT_EQ(packet[26], 1); /* the type: an address */
    KT_CHECK_INT_EQ(packet[27], 0);
    KT_CHECK_INT_EQ(packet[28], 1); /* the class: the internet */

    /* A trailing dot is the root and not an empty label. */
    KT_CHECK_INT_EQ((int)dns_build_query("example.com.", 0x1234, second, sizeof(second)), 29);
    KT_CHECK(memcmp(packet, second, 29) == 0);

    /* Names that are not names, and a packet that is too small. */
    KT_CHECK_INT_EQ(dns_build_query("", 1, packet, sizeof(packet)), -1);
    KT_CHECK_INT_EQ(dns_build_query(NULL, 1, packet, sizeof(packet)), -1);
    KT_CHECK_INT_EQ(dns_build_query("a..b", 1, packet, sizeof(packet)), -1);
    KT_CHECK_INT_EQ(dns_build_query(".a", 1, packet, sizeof(packet)), -1);
    KT_CHECK_INT_EQ(dns_build_query("example.com", 1, packet, 20), -1);
    KT_CHECK_INT_EQ(dns_build_query("example.com", 1, NULL, 64), -1);

    memset(long_name, 'a', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';
    KT_CHECK_INT_EQ(dns_build_query(long_name, 1, packet, sizeof(packet)), -1);

    /* A label of 64 characters is not a label of the protocol. */
    memset(long_name, 'a', 64);
    memcpy(long_name + 64, ".com", 5);
    KT_CHECK_INT_EQ(dns_build_query(long_name, 1, packet, sizeof(packet)), -1);

    /* --- the answer: the first address of type A and its time to live */
    length = dns_test_begin(packet, 0x4321, "example.com", 1);
    length = dns_test_a(packet, length, 0x5DB8D822u, 300u);

    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4321, &answer), 0);
    KT_CHECK_INT_EQ(answer.address, 0x5DB8D822u); /* 93.184.216.34 */
    KT_CHECK_INT_EQ(answer.ttl_s, 300);

    /* The identifier belongs to the exchange, not to the name. */
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4322, &answer), -1);

    /* An error of the server, a query and a message that stops early are not
     * an address. */
    packet[3] = 0x83u;
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4321, &answer), -1);
    packet[3] = 0x80u;

    packet[2] = 0x01u;
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4321, &answer), -1);
    packet[2] = 0x81u;

    KT_CHECK_INT_EQ(dns_parse_answer(packet, 15, 0x4321, &answer), -1);
    KT_CHECK_INT_EQ(dns_parse_answer(packet, 0, 0x4321, &answer), -1);
    KT_CHECK_INT_EQ(dns_parse_answer(NULL, length, 0x4321, &answer), -1);
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4321, NULL), -1);

    /* No answer at all, a record of another type, and a record of four bytes
     * that says it is five. */
    packet[7] = 0u;
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x4321, &answer), -1);

    {
        static const k_u8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

        length = dns_test_begin(packet, 0x0044, "v6.example", 1);
        length = dns_test_record(packet, length, 28u, 60u, v6, sizeof(v6));
        KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x0044, &answer), -1);
    }

    {
        static const k_u8_t five[5] = {1, 2, 3, 4, 5};

        length = dns_test_begin(packet, 0x0045, "odd.example", 1);
        length = dns_test_record(packet, length, 1u, 60u, five, sizeof(five));

        /* The type says address and the length says otherwise: the record is
         * skipped and there is nothing left. */
        KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x0045, &answer), -1);

        /* The same record, cut by a byte: the packet is not read past it. */
        KT_CHECK_INT_EQ(dns_parse_answer(packet, length - 1u, 0x0045, &answer), -1);
    }

    /* A CNAME on the way to the address is skipped. */
    {
        static const k_u8_t alias[2] = {0xC0u, 0x0Cu};

        length = dns_test_begin(packet, 0x0046, "www.example.com", 2);
        length = dns_test_record(packet, length, 5u, 120u, alias, sizeof(alias));
        length = dns_test_a(packet, length, 0x09090909u, 60u);

        KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x0046, &answer), 0);
        KT_CHECK_INT_EQ(answer.address, 0x09090909u);
        KT_CHECK_INT_EQ(answer.ttl_s, 60);
    }

    /* A week is cut to a day: the cache is a few hundred bytes of SRAM. */
    length = dns_test_begin(packet, 0x0047, "long.example", 1);
    length = dns_test_a(packet, length, 0x0A000202u, 604800u);
    KT_CHECK_INT_EQ(dns_parse_answer(packet, length, 0x0047, &answer), 0);
    KT_CHECK_INT_EQ(answer.ttl_s, (int)DNS_TTL_MAX_S);

    /* --- the cache */
    dns_cache_clear();
    KT_CHECK_INT_EQ((int)dns_cache_count(), 0);

    KT_CHECK_INT_EQ(dns_cache_store("example.com", 0x5DB8D822u, 60u, 1000u), 0);
    KT_CHECK_INT_EQ((int)dns_cache_count(), 1);

    KT_CHECK(dns_cache_lookup("example.com", 2000u, &address, &left));
    KT_CHECK_INT_EQ(address, 0x5DB8D822u);
    KT_CHECK_INT_EQ(left, 59);

    /* The names of the protocol are not case sensitive, and the rest of the
     * time is what the caller asks for. */
    KT_CHECK(dns_cache_lookup("EXAMPLE.Com", 3000u, &address, &left));
    KT_CHECK_INT_EQ(left, 58);
    KT_CHECK(!dns_cache_lookup("example.org", 3000u, &address, &left));

    /* The entry lives 60 seconds and not 61. */
    KT_CHECK(dns_cache_lookup("example.com", 60999u, &address, NULL));
    KT_CHECK(!dns_cache_lookup("example.com", 61000u, &address, NULL));
    KT_CHECK_INT_EQ((int)dns_cache_count(), 0); /* the expired name is gone */

    /* A time to live of zero is not a cache: "ask me again". */
    KT_CHECK_INT_EQ(dns_cache_store("fresh.example", 0x01020304u, 0u, 0u), 1);
    KT_CHECK(!dns_cache_lookup("fresh.example", 0u, &address, NULL));

    memset(long_name, 'a', DNS_NAME_MAX);
    long_name[DNS_NAME_MAX] = '\0';

    KT_CHECK_INT_EQ(dns_cache_store("", 1u, 60u, 0u), -1);
    KT_CHECK_INT_EQ(dns_cache_store(NULL, 1u, 60u, 0u), -1);
    KT_CHECK_INT_EQ(dns_cache_store(long_name, 1u, 60u, 0u), -1);

    /* A time to live of a year is kept for a day. */
    KT_CHECK_INT_EQ(dns_cache_store("day.example", 0x08080808u, 31536000u, 0u), 0);
    KT_CHECK(dns_cache_lookup("day.example", 0u, NULL, &left));
    KT_CHECK_INT_EQ(left, (int)DNS_TTL_MAX_S);

    /* The ninth name makes room for the one that expires first. */
    dns_cache_clear();
    for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
        char name[16];

        snprintf(name, sizeof(name), "host%u.example", (unsigned)i);
        KT_CHECK_INT_EQ(dns_cache_store(name, (k_u32_t)i, (k_u32_t)(100 + i), 0u), 0);
    }

    KT_CHECK_INT_EQ((int)dns_cache_count(), (int)DNS_CACHE_ENTRIES);
    KT_CHECK(dns_cache_lookup("host0.example", 0u, &address, NULL));
    KT_CHECK_INT_EQ(dns_cache_store("new.example", 0xA0A0A0A0u, 400u, 0u), 0);
    KT_CHECK_INT_EQ((int)dns_cache_count(), (int)DNS_CACHE_ENTRIES);
    KT_CHECK(!dns_cache_lookup("host0.example", 0u, &address, NULL));
    KT_CHECK(dns_cache_lookup("host7.example", 0u, &address, NULL));
    KT_CHECK(dns_cache_lookup("new.example", 0u, &address, NULL));

    /* The listing of the cache is a walk of its slots. */
    KT_CHECK(dns_cache_name(DNS_CACHE_ENTRIES, 0u, &address, &left) == NULL);
    KT_CHECK_INT_EQ(dns_cache_store("list.example", 0x0B0B0B0Bu, 90u, 0u), 0);
    for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
        const char *name = dns_cache_name(i, 0u, &address, &left);

        if (name != NULL && strcmp(name, "list.example") == 0) {
            break;
        }
    }
    KT_CHECK(i < DNS_CACHE_ENTRIES);

    /* The name found above is gone after its time to live. */
    if (i < DNS_CACHE_ENTRIES) {
        KT_CHECK(dns_cache_name(i, 200000u, &address, &left) == NULL);
    }

    dns_cache_clear();
    KT_CHECK_INT_EQ((int)dns_cache_count(), 0);
}

void test_net(void)
{
    static const struct {
        const char *request;
        const char *expect;
    } cases[] = {
        {"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", "200 OK"},
        {"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n", "200 OK"},
        {"GET /nope HTTP/1.1\r\nHost: localhost\r\n\r\n", "404 Not Found"},
        {"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n", "405 Method Not Allowed"},
    };
    char answer[1024];
    k_size_t i;

    test_url();
    test_get();
    test_stream();
    test_ntp();
    test_dns();
    test_loopback();

    /*
     * The server of the page of the state, over the loopback of the port and
     * for real: the test is the client of its own board. The port is fixed and
     * SO_REUSEADDR is the business of the port, so a session of the run before
     * does not stand in the way.
     */
    ktest_begin("http: the server answers the page of the state, health and the refusals");

    {
        unsigned attempt;
        k_u16_t port = 0;

        for (attempt = 0; attempt < 8; attempt++) {
            port = test_free_port();

            if (net_http_server_start(port) == 0) {
                break;
            }
        }

        page_port = port;
    }

    KT_CHECK(net_http_server_running());
    KT_CHECK_INT_EQ((int)net_http_server_port(), (int)page_port);

    for (i = 0; i < K_ARRAY_SIZE(cases); i++) {
        k_socket_t *client = connect_to_door(page_port);
        k_size_t used = 0;

        KT_CHECK(client != NULL);
        if (client == NULL) {
            continue;
        }

        KT_CHECK_INT_EQ(k_socket_send(client, cases[i].request, strlen(cases[i].request), true),
                        (int)strlen(cases[i].request));

        /* The answer ends with the close of the session: that is what
         * `Connection: close` promises and what the client relies on. */
        while (used < sizeof(answer) - 1) {
            int n = k_socket_recv(client, answer + used, sizeof(answer) - 1 - used);

            if (n <= 0) {
                break;
            }

            used += (k_size_t)n;
        }

        answer[used] = '\0';
        KT_CHECK(strstr(answer, cases[i].expect) != NULL);

        if (i == 0) {
            const char *head_end = strstr(answer, "\r\n\r\n");
            const char *promise = strstr(answer, "Content-Length: ");
            k_size_t promised = 0;

            KT_CHECK(strstr(answer, "MicroOS") != NULL);
            KT_CHECK(strstr(answer, "uptime") != NULL);

            /* The length the header promises is the length the body has: a
             * client that reads by it must not truncate the page. */
            KT_CHECK(head_end != NULL && promise != NULL);
            if (head_end != NULL && promise != NULL) {
                const char *p = promise + strlen("Content-Length: ");

                while (*p >= '0' && *p <= '9') {
                    promised = promised * 10 + (k_size_t)(*p - '0');
                    p++;
                }

                KT_CHECK_INT_EQ(promised, strlen(head_end + 4));
            }
        }

        if (i == 1) {
            KT_CHECK(strstr(answer, "\r\n\r\nok\n") != NULL);
        }

        k_socket_close(client);
    }

    net_http_server_stop();
    KT_CHECK(!net_http_server_running());

    /*
     * The command of the shell is the way a person and a script of init open
     * the door: the server itself never starts with the boot. The port is a
     * free one, so the test does not ask for the 80 of a real machine.
     */
    ktest_begin("http: the command httpd starts, reports and stops the server");

    KT_CHECK_INT_EQ(net_register(), 0);
    {
        unsigned attempt;
        char command[32];

        for (attempt = 0; attempt < 8; attempt++) {
            k_u16_t port = test_free_port();

            snprintf(command, sizeof(command), "httpd %u", (unsigned)port);

            if (kshell_execute(command) == KSHELL_OK && net_http_server_running() &&
                net_http_server_port() == port) {
                command_port = port;
                break;
            }
        }
    }

    KT_CHECK(net_http_server_running());
    KT_CHECK_INT_EQ((int)net_http_server_port(), (int)command_port);

    /* A second call keeps the server where it is and says so. */
    KT_CHECK_INT_EQ(kshell_execute("httpd"), KSHELL_OK);
    KT_CHECK_INT_EQ((int)net_http_server_port(), (int)command_port);

    KT_CHECK_INT_EQ(kshell_execute("httpd 99999"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("httpd off"), KSHELL_OK);
    KT_CHECK(!net_http_server_running());

    /* Off when it is already off is an answer, not an error. */
    KT_CHECK_INT_EQ(kshell_execute("httpd off"), KSHELL_OK);

    test_mqtt();
}
