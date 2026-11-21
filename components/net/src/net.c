/*
 * Micro OS — the network services of the board: HTTP, NTP and MQTT.
 *
 * One request at a time, one session: the shell is one task and the board
 * is not a server of its own. Every wait has a timeout (NET_TIMEOUT_MS), so
 * a network that went away answers an error to the command instead of
 * holding the console.
 *
 * The sockets are not made here: this file names a host and a port, and the
 * kernel opens the session (kernel/net.h). That is what lets the same client
 * run over lwIP on the board and over the sockets of the host in the tests.
 */
#include "net/net.h"

#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"
#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/net.h"
#include "kshell/kshell.h"
#include "net/mqtt.h"
#include "net/net_http.h"
#include "net/ntp.h"

#if MICRO_OS_NET_HTTP
/* --- the transport of the HTTP client --- */

static int sock_open(const char *host, int port, void **ctx)
{
    k_socket_t *session;

    if (host == NULL || ctx == NULL || port <= 0 || port > 65535) {
        return -1;
    }

    session = k_socket_connect(host, (k_u16_t)port, NET_TIMEOUT_MS);
    if (session == NULL) {
        kwarn("net: cannot connect to %s:%d", host, port);
        return -1;
    }

    *ctx = session;

    return 0;
}

static int sock_send(void *ctx, const void *data, k_size_t size)
{
    int sent = k_socket_send(ctx, data, size, true);

    if (sent < 0 || (k_size_t)sent != size) {
        return -1;
    }

    return sent;
}

static int sock_recv(void *ctx, void *data, k_size_t size)
{
    return k_socket_recv(ctx, data, size);
}

static void sock_close(void *ctx)
{
    k_socket_close(ctx);
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
#endif /* MICRO_OS_NET_HTTP */

#if MICRO_OS_NET_NTP
/* --- the clock --- */

int net_ntp_sync(const char *server, k_u64_t *unix_seconds)
{
    k_u8_t request[NTP_PACKET_SIZE];
    k_u8_t answer[NTP_PACKET_SIZE];
    int n;

    if (server == NULL || unix_seconds == NULL) {
        return -1;
    }

    ntp_build_request(request);

    /*
     * One request and one answer is the whole of the exchange: the port
     * sends the packet, waits for the answer and closes the socket, so the
     * board never holds a UDP socket for longer than the call.
     */
    n = k_net_udp_exchange(server, NTP_PORT, request, sizeof(request), answer, sizeof(answer),
                           NET_TIMEOUT_MS);
    if (n != (int)sizeof(answer)) {
        kwarn("net: no answer from the NTP server %s (%d byte(s))", server, n);
        return -1;
    }

    return ntp_parse_response(answer, unix_seconds);
}
#endif /* MICRO_OS_NET_NTP */

#if MICRO_OS_NET_HTTP_SERVER || MICRO_OS_NET_MQTT
/*
 * A port of the shell: digits and nothing else, and no more than 65535. The
 * parser is these few lines and not a call of strtoul, because the component
 * keeps its dependencies to the kernel.
 */
static bool parse_port(const char *text, k_u16_t *out)
{
    k_u32_t value = 0;
    k_size_t i;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        value = value * 10u + (k_u32_t)(text[i] - '0');
        if (value > 65535u) {
            return false;
        }
    }

    if (value == 0) {
        return false;
    }

    *out = (k_u16_t)value;

    return true;
}
#endif /* MICRO_OS_NET_HTTP_SERVER || MICRO_OS_NET_MQTT */

#if MICRO_OS_NET_HTTP_SERVER
/* --- the page of the state --- */

/*
 * The command does not start the server by itself at the boot: a machine of
 * this size opens a port when its owner says so, from a script of init or
 * from the shell. The policy is the platform's; the door is here.
 */
static int cmd_httpd(int argc, char **argv)
{
    k_u16_t port = NET_HTTP_PORT;

    if (argc > 2) {
        kshell_printf("usage: httpd [port|off]\n");
        return KSHELL_USAGE;
    }

    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        if (!net_http_server_running()) {
            kshell_printf("httpd: it is not on\n");
            return KSHELL_OK;
        }

        net_http_server_stop();
        kshell_printf("httpd: stopped\n");

        return KSHELL_OK;
    }

    /*
     * The argument is read before the state of the server: a mistyped port is
     * a usage whatever the door is doing.
     */
    if (argc == 2 && !parse_port(argv[1], &port)) {
        kshell_printf("usage: httpd [port|off]\n");
        return KSHELL_USAGE;
    }

    /*
     * One server is one port: a second `httpd` says where the page is and
     * leaves the server alone. Moving it is `httpd off` and then `httpd`.
     */
    if (net_http_server_running()) {
        kshell_printf("httpd: already on port %u\n", (unsigned)net_http_server_port());
        return KSHELL_OK;
    }

    if (net_http_server_start(port) != 0) {
        kshell_printf("httpd: the port %u is not ours\n", (unsigned)port);
        return KSHELL_ERROR;
    }

    kshell_printf("httpd: the page is on port %u\n", (unsigned)port);

    return KSHELL_OK;
}
#endif /* MICRO_OS_NET_HTTP_SERVER */

#if MICRO_OS_NET_HTTP
/* --- the HTTP client --- */

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
#endif /* MICRO_OS_NET_HTTP */

#if MICRO_OS_NET_NTP
/*
 * Give the moment to the clock of the board. The chip that keeps the date
 * is a driver, the kernel keeps the door to it, and the network only knows
 * the seconds of Unix: the three do not have to meet in the same component.
 */
static int ntp_store_time(k_u64_t unix_seconds)
{
    return k_clock_set_unix(unix_seconds);
}

static int cmd_ntp(int argc, char **argv)
{
    const char *server = argc > 1 ? argv[1] : "pool.ntp.org";
    k_u64_t seconds;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    if (net_ntp_sync(server, &seconds) != 0) {
        kshell_printf("ntp: no answer from %s\n", server);
        return KSHELL_ERROR;
    }

    if (ntp_unix_to_datetime(seconds, &year, &month, &day, &hour, &minute, &second) != 0) {
        kshell_printf("ntp: the server says a year the clock cannot keep\n");
        return KSHELL_ERROR;
    }

    kshell_printf("ntp: %04d-%02d-%02d %02d:%02d:%02d UTC\n", year, month, day, hour, minute,
                  second);

    if (ntp_store_time(seconds) != 0) {
        kshell_printf("ntp: the clock did not take the time\n");
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}
#endif /* MICRO_OS_NET_NTP */

#if MICRO_OS_NET_MQTT
/*
 * One message and out: the command is a way to send a word to a broker from a
 * script or from the console, not a client that lives in the board. The
 * keepalive is zero because the session lasts exactly one packet.
 */
static int cmd_mqtt(int argc, char **argv)
{
    struct mqtt_client client;
    k_u16_t port;
    k_size_t length;
    int rc;

    if (argc != 5 || !parse_port(argv[2], &port)) {
        kshell_printf("usage: mqtt <host> <port> <topic> <message>\n");
        return KSHELL_USAGE;
    }

    length = strlen(argv[4]);
    if (length > MQTT_PAYLOAD_MAX) {
        kshell_printf("mqtt: the message is longer than %d byte(s)\n", MQTT_PAYLOAD_MAX);
        return KSHELL_ERROR;
    }

    rc = mqtt_connect(&client, argv[1], port, "microos", 0);
    if (rc != MQTT_OK) {
        kshell_printf("mqtt: %s: %s\n", argv[1],
                      rc == MQTT_REFUSED ? "the broker refused the client"
                                         : "no session with the broker");
        return KSHELL_ERROR;
    }

    rc = mqtt_publish(&client, argv[3], argv[4], length, 0);
    mqtt_disconnect(&client);

    if (rc != MQTT_OK) {
        kshell_printf("mqtt: the broker did not take the message\n");
        return KSHELL_ERROR;
    }

    kshell_printf("mqtt: %u byte(s) to %s on %s:%u\n", (unsigned)length, argv[3], argv[1],
                  (unsigned)port);

    return KSHELL_OK;
}
#endif /* MICRO_OS_NET_MQTT */

int net_register(void)
{
    int rc = 0;

    /*
     * One command per feature: a build that leaves the client out has no
     * command to answer with, and `help` does not list it (ADR 0010).
     */
#if MICRO_OS_NET_HTTP
    rc |= kshell_register("http", "the HTTP client: http get <url>", cmd_http);
#endif
#if MICRO_OS_NET_NTP
    rc |= kshell_register("ntp", "set the clock from NTP: ntp [server]", cmd_ntp);
#endif
#if MICRO_OS_NET_HTTP_SERVER
    rc |= kshell_register("httpd", "the page of the state: httpd [port|off]", cmd_httpd);
#endif
#if MICRO_OS_NET_MQTT
    rc |= kshell_register("mqtt", "publish over MQTT: mqtt <host> <port> <topic> <message>",
                          cmd_mqtt);
#endif

    return rc;
}
