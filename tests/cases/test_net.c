#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "net/http.h"
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

void test_net(void)
{
    test_url();
    test_get();
    test_ntp();
}
