/*
 * Micro OS — the sockets under the HTTP client and the clock of NTP.
 *
 * One request at a time, one socket: the shell is one task and the board is
 * not a server of its own. Every wait has a timeout, so a network that went
 * away answers an error to the command instead of holding the console.
 */
#include "net/net_esp32.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "drivers/internal.h"
#include "kernel/log.h"
#include "kshell/kshell.h"
#include "net/ntp.h"

/* The seconds a socket waits for its peer. */
#define NET_TIMEOUT_S 5

/* The socket of the request in flight: the shell runs one at a time. */
static int g_socket = -1;

static int set_timeout(int fd)
{
    struct timeval tv;

    tv.tv_sec = NET_TIMEOUT_S;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }

    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int resolve(const char *host, int port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[8];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;

    snprintf(service, sizeof(service), "%d", port);

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0 || result == NULL || result->ai_addr == NULL) {
        if (result != NULL) {
            freeaddrinfo(result);
        }
        kwarn("net: cannot resolve %s:%d (rc=%d)", host, port, rc);
        return -1;
    }

    /*
     * lwIP gives every address the room of a `sockaddr_storage` even when
     * the family is IPv4: the copy is the size of the address we asked for.
     */
    memset(out, 0, sizeof(*out));
    memcpy(out, result->ai_addr, sizeof(*out));
    freeaddrinfo(result);

    return 0;
}

/* --- the transport of the HTTP client --- */

static int sock_open(const char *host, int port, void **ctx)
{
    struct sockaddr_in address;
    int fd;

    (void)ctx;

    if (resolve(host, port, SOCK_STREAM, &address) != 0) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        kwarn("net: cannot make a socket (rc=%d)", (int)errno);
        return -1;
    }

    if (set_timeout(fd) != 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        kwarn("net: cannot connect to %s:%d (rc=%d)", host, port, (int)errno);
        close(fd);
        return -1;
    }

    g_socket = fd;

    return 0;
}

static int sock_send(void *ctx, const void *data, k_size_t size)
{
    const char *p = data;
    k_size_t sent = 0;

    (void)ctx;

    while (sent < size) {
        int n = send(g_socket, p + sent, size - sent, 0);

        if (n <= 0) {
            return -1;
        }
        sent += (k_size_t)n;
    }

    return (int)size;
}

static int sock_recv(void *ctx, void *data, k_size_t size)
{
    (void)ctx;

    return recv(g_socket, data, size, 0);
}

static void sock_close(void *ctx)
{
    (void)ctx;

    if (g_socket >= 0) {
        close(g_socket);
        g_socket = -1;
    }
}

static const struct http_transport s_transport = {
    .open = sock_open,
    .send = sock_send,
    .recv = sock_recv,
    .close = sock_close,
};

int net_http_get(const char *url, char *body, k_size_t size, struct http_response *out)
{
    return http_get(&s_transport, url, body, size, out);
}

/* --- the clock --- */

int net_ntp_sync(const char *server, k_u64_t *unix_seconds)
{
    struct sockaddr_in address;
    k_u8_t request[NTP_PACKET_SIZE];
    k_u8_t answer[NTP_PACKET_SIZE];
    int fd;
    int n;

    if (server == NULL || unix_seconds == NULL) {
        return -1;
    }

    if (resolve(server, NTP_PORT, SOCK_DGRAM, &address) != 0) {
        kwarn("net: cannot resolve the NTP server %s", server);
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        kwarn("net: cannot make a UDP socket (rc=%d)", (int)errno);
        return -1;
    }

    if (set_timeout(fd) != 0) {
        close(fd);
        return -1;
    }

    ntp_build_request(request);

    n = sendto(fd, request, sizeof(request), 0, (struct sockaddr *)&address, sizeof(address));
    if (n != (int)sizeof(request)) {
        kwarn("net: cannot send to the NTP server %s (rc=%d)", server, (int)errno);
        close(fd);
        return -1;
    }

    n = recv(fd, answer, sizeof(answer), 0);
    if (n != (int)sizeof(answer)) {
        kwarn("net: no answer from the NTP server %s (rc=%d, errno=%d)", server, n, (int)errno);
    }
    close(fd);

    if (n != (int)sizeof(answer)) {
        return -1;
    }

    return ntp_parse_response(answer, unix_seconds);
}

/* --- the shell --- */

static int cmd_http(int argc, char **argv)
{
    char body[257];
    struct http_response response;
    int rc;

    if (argc != 3 || strcmp(argv[1], "get") != 0) {
        kshell_printf("usage: http get <url>\n");
        return KSHELL_USAGE;
    }

    rc = net_http_get(argv[2], body, sizeof(body), &response);
    if (rc != HTTP_OK) {
        kshell_printf("http: %s: %s\n", argv[2],
                      rc == HTTP_ERROR ? "the connection failed" : "the URL is not one we speak");
        return KSHELL_ERROR;
    }

    kshell_printf("HTTP %d, %u byte(s)\n", response.status, (unsigned)response.length);
    if (response.length > 0) {
        kshell_printf("%s\n", body);
    }

    return KSHELL_OK;
}

static int cmd_ntp(int argc, char **argv)
{
    const char *server = argc > 1 ? argv[1] : "pool.ntp.org";
    struct rtc_datetime dt;
    k_u64_t seconds;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int rc;

    rc = net_ntp_sync(server, &seconds);
    if (rc != 0) {
        kshell_printf("ntp: no answer from %s\n", server);
        return KSHELL_ERROR;
    }

    if (ntp_unix_to_datetime(seconds, &year, &month, &day, &hour, &minute, &second) != 0) {
        kshell_printf("ntp: the server says a year the clock cannot keep\n");
        return KSHELL_ERROR;
    }

    memset(&dt, 0, sizeof(dt));
    dt.year = year;
    dt.month = month;
    dt.day = day;
    dt.hour = hour;
    dt.minute = minute;
    dt.second = second;
    dt.valid = true;

    kshell_printf("ntp: %04d-%02d-%02d %02d:%02d:%02d UTC\n", year, month, day, hour, minute,
                  second);

    if (drv_rtc_set_time(&dt) != 0) {
        kshell_printf("ntp: the clock did not take the time\n");
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

int net_register(void)
{
    int rc = 0;

    rc |= kshell_register("http", "the HTTP client: http get <url>", cmd_http);
    rc |= kshell_register("ntp", "set the clock from NTP: ntp [server]", cmd_ntp);

    return rc;
}
