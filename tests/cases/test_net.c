#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/net.h"
#include "kshell/kshell.h"
#include "net/http.h"
#include "net/net.h"
#include "net/net_http.h"
#include "net/ntp.h"

/* --- the transport that replays a canned answer of a server --- */

static const char *g_answer;
static k_size_t g_answer_len;
static k_size_t g_answer_pos;
static k_size_t g_answer_chunk;
static char g_request[256];
static char g_host[HTTP_HOST_MAX];
static int g_port;

static int fake_open(const char *host, int port, void **ctx)
{
    k_size_t len = strlen(host);

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

    KT_CHECK_INT_EQ(http_parse_url("https://example.com/", &url), HTTP_INVALID);
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

    /* A body longer than the buffer is cut, not overflowed. */
    g_answer = "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n0123456789";
    g_answer_len = strlen(g_answer);
    g_answer_chunk = 16;

    KT_CHECK_INT_EQ(http_get(&g_transport, "http://example.com/", body, 8, &response), HTTP_OK);
    KT_CHECK_INT_EQ(response.length, 7);
    KT_CHECK_STR_EQ(body, "0123456");
}

static void put_be32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value >> 24);
    p[1] = (k_u8_t)(value >> 16);
    p[2] = (k_u8_t)(value >> 8);
    p[3] = (k_u8_t)value;
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
static void test_loopback(void)
{
    static const char question[] = "ping";
    static const char answer_text[] = "pong";
    char answer[16];
    k_server_t *server;
    k_socket_t *client = NULL;
    k_socket_t *session = NULL;

    ktest_begin("net: a session of the loopback carries a line both ways");

    server = k_server_listen(45123);
    KT_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }

    client = k_socket_connect("127.0.0.1", 45123, 2000);
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
    test_ntp();
    test_loopback();

    /*
     * The server of the page of the state, over the loopback of the port and
     * for real: the test is the client of its own board. The port is fixed and
     * SO_REUSEADDR is the business of the port, so a session of the run before
     * does not stand in the way.
     */
    ktest_begin("http: the server answers the page of the state, health and the refusals");

    KT_CHECK_INT_EQ(net_http_server_start(18080), 0);
    KT_CHECK(net_http_server_running());

    for (i = 0; i < K_ARRAY_SIZE(cases); i++) {
        k_socket_t *client = k_socket_connect("127.0.0.1", 18080, 2000);
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
    KT_CHECK_INT_EQ(kshell_execute("httpd 18082"), KSHELL_OK);
    KT_CHECK(net_http_server_running());
    KT_CHECK_INT_EQ(net_http_server_port(), 18082);

    /* A second call keeps the server where it is and says so. */
    KT_CHECK_INT_EQ(kshell_execute("httpd"), KSHELL_OK);
    KT_CHECK_INT_EQ(net_http_server_port(), 18082);

    KT_CHECK_INT_EQ(kshell_execute("httpd 99999"), KSHELL_USAGE);
    KT_CHECK_INT_EQ(kshell_execute("httpd off"), KSHELL_OK);
    KT_CHECK(!net_http_server_running());

    /* Off when it is already off is an answer, not an error. */
    KT_CHECK_INT_EQ(kshell_execute("httpd off"), KSHELL_OK);
}
