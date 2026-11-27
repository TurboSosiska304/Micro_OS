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
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/net.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "net/httpd.h"
#include "net/mqtt.h"
#include "net/net_http.h"
#include "net/ntp.h"
#include "net/ota.h"
#include "net/tls.h"
#include "net/tls_server.h"
#include "sha256.h"

#if MICRO_OS_NET_HTTP
/* --- the transport of the HTTP client --- */

static int sock_open(const struct http_transport *self, const char *host, int port, void **ctx)
{
    k_socket_t *session;

    /* A plain transport has no layer below it: the TLS layer is the one that
     * uses the field (net/tls.h). */
    (void)self;

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

const struct http_transport *net_http_transport(void)
{
    return &s_transport;
}

int net_http_get(const char *url, char *body, k_size_t size, struct http_response *out)
{
    return http_get(&s_transport, url, body, size, out);
}

/* --- the body of a download is a file of the VFS --- */

/*
 * The sink of a file. The descriptor is opened by the caller of the
 * download; `reset` closes it and opens the path again with a truncate, which
 * is what the whole object after a Range needs.
 */
struct file_sink {
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    const char *path;
};

static int file_sink_write(void *ctx, const void *data, k_size_t size)
{
    struct file_sink *sink = (struct file_sink *)ctx;
    int rc;

    rc = vfs_write(&sink->file, data, size);

    if (rc < 0) {
        return rc;
    }

    return rc;
}

static int file_sink_reset(void *ctx)
{
    struct file_sink *sink = (struct file_sink *)ctx;

    (void)vfs_close(&sink->file);
    sink->file.offset = 0;

    if (vfs_open(sink->path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &sink->file, sink->priv) !=
        0) {
        return -1;
    }

    return 0;
}

int net_http_download_to(const struct http_transport *transport, const char *url, const char *path,
                         bool resume, struct http_stream *out)
{
    struct file_sink sink;
    struct http_stream stream;
    k_u64_t have = 0;
    int rc;

    if (url == NULL || path == NULL) {
        return HTTP_INVALID;
    }

    memset(&sink, 0, sizeof(sink));
    sink.path = path;

    rc = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT, &sink.file, sink.priv);
    if (rc != 0) {
        return HTTP_ERROR;
    }

    if (resume) {
        struct vfs_stat info;

        if (vfs_stat(path, &info) == 0 && info.type == 1) {
            have = info.size;
        }

        /* The descriptor writes where the file ends: the bytes of the run
         * before are kept and the Range asks the server for the rest. */
        sink.file.offset = have;
    }

    memset(&stream, 0, sizeof(stream));
    stream.transport = (transport != NULL) ? transport : &s_transport;
    stream.url = url;
    stream.sink.ctx = &sink;
    stream.sink.write = file_sink_write;
    stream.sink.reset = file_sink_reset;
    stream.resume_from = have;
    stream.max_redirects = HTTP_REDIRECT_MAX;
    stream.timeout_ms = NET_DOWNLOAD_TIMEOUT_MS;

    rc = http_stream(&stream);

    (void)vfs_close(&sink.file);

    if (out != NULL) {
        *out = stream;
    }

    if (rc == HTTP_OK && stream.status != 200 && stream.status != 206) {
        return HTTP_ERROR;
    }

    return rc;
}

int net_http_download(const char *url, const char *path, bool resume, struct http_stream *out)
{
    return net_http_download_to(&s_transport, url, path, resume, out);
}

/* --- the digest of a download --- */

struct hashed_sink {
    struct file_sink file;
    struct microos_sha256_ctx hash;
};

static int hashed_sink_write(void *ctx, const void *data, k_size_t size)
{
    struct hashed_sink *sink = (struct hashed_sink *)ctx;
    int rc = vfs_write(&sink->file.file, data, size);

    if (rc < 0) {
        return rc;
    }

    microos_sha256_update(&sink->hash, (const uint8_t *)data, (size_t)rc);

    return rc;
}

static int hashed_sink_reset(void *ctx)
{
    struct hashed_sink *sink = (struct hashed_sink *)ctx;

    microos_sha256_init(&sink->hash);

    return file_sink_reset(&sink->file);
}

int net_http_download_hashed(const char *url, const char *path, bool resume,
                             struct http_stream *out, char hex[HTTP_SHA256_HEX])
{
    static const char digits[] = "0123456789abcdef";
    struct hashed_sink sink;
    struct http_stream stream;
    k_u64_t have = 0;
    k_u8_t digest[MICROOS_SHA256_SIZE];
    k_size_t i;
    int rc;

    if (url == NULL || path == NULL || hex == NULL) {
        return HTTP_INVALID;
    }

    memset(&sink, 0, sizeof(sink));
    sink.file.path = path;
    microos_sha256_init(&sink.hash);

    rc = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT, &sink.file.file, sink.file.priv);
    if (rc != 0) {
        return HTTP_ERROR;
    }

    if (resume) {
        struct vfs_stat info;

        if (vfs_stat(path, &info) == 0 && info.type == 1) {
            have = info.size;
        }

        sink.file.file.offset = have;
    }

    memset(&stream, 0, sizeof(stream));
    stream.transport = &s_transport;
    stream.url = url;
    stream.sink.ctx = &sink;
    stream.sink.write = hashed_sink_write;
    stream.sink.reset = hashed_sink_reset;
    stream.resume_from = have;
    stream.max_redirects = HTTP_REDIRECT_MAX;
    stream.timeout_ms = NET_DOWNLOAD_TIMEOUT_MS;

    rc = http_stream(&stream);

    (void)vfs_close(&sink.file.file);

    microos_sha256_final(&sink.hash, digest);

    for (i = 0; i < MICROOS_SHA256_SIZE; i++) {
        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0Fu];
    }

    hex[HTTP_SHA256_HEX - 1u] = '\0';

    if (out != NULL) {
        *out = stream;
    }

    if (rc == HTTP_OK && stream.status != 200 && stream.status != 206) {
        return HTTP_ERROR;
    }

    return rc;
}
#endif /* MICRO_OS_NET_HTTP */

#if MICRO_OS_NET_NTP
/* --- the clock --- */

int net_ntp_sync_port(const char *server, k_u16_t port, k_u64_t *unix_seconds)
{
    k_u8_t request[NTP_PACKET_SIZE];
    k_u8_t answer[NTP_PACKET_SIZE];
    int n;

    if (server == NULL || unix_seconds == NULL || port == 0) {
        return -1;
    }

    ntp_build_request(request);

    /*
     * One request and one answer is the whole of the exchange: the port
     * sends the packet, waits for the answer and closes the socket, so the
     * board never holds a UDP socket for longer than the call.
     */
    n = k_net_udp_exchange(server, port, request, sizeof(request), answer, sizeof(answer),
                           NET_TIMEOUT_MS);
    if (n != (int)sizeof(answer)) {
        kwarn("net: no answer from the NTP server %s (%d byte(s))", server, n);
        return -1;
    }

    return ntp_parse_response(answer, unix_seconds);
}

int net_ntp_sync(const char *server, k_u64_t *unix_seconds)
{
    return net_ntp_sync_port(server, NTP_PORT, unix_seconds);
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
/* --- the servers of HTTP --- */

/* The start of the server of files: `serve` is the door of the volume and
 * `tls` the same door with the certificate of /etc/ssl. The two share the
 * arguments: an optional port and an optional root. */
static int httpd_start_files(int argc, char **argv, bool tls)
{
    struct httpd_config config;
    k_u16_t port = tls ? 443u : HTTPD_PORT;
    const char *root = argc > 2 ? argv[2] : HTTPD_ROOT_DEFAULT;
    int rc;

    if (argc > 3) {
        kshell_printf("usage: httpd %s [port] [root]\n", tls ? "tls" : "serve");
        return KSHELL_USAGE;
    }

    if (argc > 1 && !parse_port(argv[1], &port)) {
        kshell_printf("usage: httpd %s [port] [root]\n", tls ? "tls" : "serve");
        return KSHELL_USAGE;
    }

    if (httpd_running()) {
        kshell_printf("httpd: already on port %u\n", (unsigned)httpd_port());
        return KSHELL_OK;
    }

    memset(&config, 0, sizeof(config));
    config.root = root;
    config.log = HTTPD_LOG_PATH;
    config.port = port;
    config.connections = HTTPD_CONNECTIONS;
    config.idle_timeout_ms = HTTPD_IDLE_TIMEOUT_MS;
    config.tls = tls;

    rc = httpd_start(&config);

    if (rc == HTTPD_EROOT) {
        kshell_printf("httpd: %s is not a directory\n", root);
        return KSHELL_ERROR;
    }

    if (rc != HTTPD_OK) {
        kshell_printf("httpd: the port %u is not ours\n", (unsigned)port);
        return KSHELL_ERROR;
    }

    kshell_printf("httpd: the files of %s are on port %u%s\n", httpd_root(), (unsigned)httpd_port(),
                  tls && net_tls_server_supported() ? " (TLS)" : "");

    if (tls && !net_tls_server_supported()) {
        kshell_printf("httpd: this image has no serving side of TLS\n");
    }

    return KSHELL_OK;
}

/* `httpd off` closes both doors and says what it found: a script of init may
 * stop a server that the boot did not start. */
static int httpd_stop_all(void)
{
    bool was_on = net_http_server_running() || httpd_running();

    if (net_http_server_running()) {
        net_http_server_stop();
    }

    if (httpd_running()) {
        httpd_stop();
    }

    kshell_printf("httpd: %s\n", was_on ? "stopped" : "it is not on");

    return KSHELL_OK;
}

/*
 * The command does not start a server by itself at the boot: a machine of
 * this size opens a port when its owner says so, from a script of init or
 * from the shell. The policy is the platform's; the doors are here.
 *
 * `httpd [port]` is the page of the state (net/net_http.h), `httpd serve
 * [port] [root]` is the server of the files of the volume (net/httpd.h) and
 * `httpd tls [port] [root]` is the same with the certificate of the machine.
 */
static int cmd_httpd(int argc, char **argv)
{
    k_u16_t port = NET_HTTP_PORT;

    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        return httpd_stop_all();
    }

    if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
        return httpd_start_files(argc - 1, argv + 1, false);
    }

    if (argc >= 2 && strcmp(argv[1], "tls") == 0) {
        return httpd_start_files(argc - 1, argv + 1, true);
    }

    if (argc > 2) {
        kshell_printf("usage: httpd [port|off] | httpd serve [port] [root]\n");
        return KSHELL_USAGE;
    }

    /*
     * The argument is read before the state of the server: a mistyped port is
     * a usage whatever the door is doing.
     */
    if (argc == 2 && !parse_port(argv[1], &port)) {
        kshell_printf("usage: httpd [port|off] | httpd serve [port] [root]\n");
        return KSHELL_USAGE;
    }

    if (net_http_server_running()) {
        /* One server is one port: a second `httpd` says where the page is and
         * leaves the server alone. Moving it is `httpd off` and then
         * `httpd`. */
        kshell_printf("httpd: already on port %u\n", (unsigned)net_http_server_port());
        return KSHELL_OK;
    }

    if (argc == 1 && httpd_running()) {
        kshell_printf("httpd: %u request(s), %u refused, %u session(s) on port %u\n",
                      (unsigned)httpd_requests(), (unsigned)httpd_refused(),
                      (unsigned)httpd_active(), (unsigned)httpd_port());
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
    static char body[4097];
    struct http_response response;
    int rc;

    if (argc != 3 || strcmp(argv[1], "get") != 0) {
        kshell_printf("usage: http get <url>\n");
        return KSHELL_USAGE;
    }

    rc = net_http_get(argv[2], body, sizeof(body), &response);
    if (rc != HTTP_OK) {
        if (rc == HTTP_TLS) {
            kshell_printf("http: %s: %s\n", argv[2], net_tls_last_error());
        } else if (rc == HTTP_ERROR) {
            kshell_printf("http: %s: the connection failed\n", argv[2]);
        } else if (rc == HTTP_TOO_LONG) {
            kshell_printf("http: %s: the head of the answer is longer than %u bytes\n", argv[2],
                          (unsigned)HTTP_HEAD_MAX);
        } else {
            kshell_printf("http: %s: the URL is not one we speak\n", argv[2]);
        }
        return KSHELL_ERROR;
    }

    if (response.truncated) {
        if (response.total > 0) {
            kshell_printf("HTTP %d, truncated at %u of %u byte(s)\n", response.status,
                          (unsigned)response.length, (unsigned)response.total);
        } else {
            kshell_printf("HTTP %d, truncated at %u byte(s)\n", response.status,
                          (unsigned)response.length);
        }
    } else {
        kshell_printf("HTTP %d, %u byte(s)\n", response.status, (unsigned)response.length);
    }

    if (response.length > 0) {
        kshell_printf("%s\n", body);
    }

    return KSHELL_OK;
}

/*
 * The console face of the stream (ADR 0024 §2): a URL and a file, and a
 * second run of the same command continues a file that was cut. The bytes
 * never pass through the memory of the shell: they go from the socket to the
 * filesystem a piece at a time.
 */
static int cmd_wget(int argc, char **argv)
{
    struct http_stream stream;
    struct vfs_stat info;
    k_u64_t have = 0;
    bool resume = false;
    bool hashed = false;
    char digest[HTTP_SHA256_HEX];
    int rc;

    if (argc != 3 && argc != 4) {
        kshell_printf("usage: wget [--sha256] <url> <file>\n");
        return KSHELL_USAGE;
    }

    if (argc == 4) {
        if (strcmp(argv[1], "--sha256") != 0) {
            kshell_printf("usage: wget [--sha256] <url> <file>\n");
            return KSHELL_USAGE;
        }

        argv++;
        hashed = true;
    }

    if (vfs_stat(argv[2], &info) == 0) {
        /* A device - /dev/null for a stream of a hundred megabytes that is
         * not kept - is a sink as good as a file. A directory is not. */
        if (info.type == 2) {
            kshell_printf("wget: %s is not a file\n", argv[2]);
            return KSHELL_ERROR;
        }

        have = info.size;
        resume = have > 0;
    }

    if (hashed) {
        rc = net_http_download_hashed(argv[1], argv[2], resume, &stream, digest);
    } else {
        rc = net_http_download(argv[1], argv[2], resume, &stream);
    }

    if (rc != HTTP_OK) {
        /*
         * A file this command made and could not fill is not a file - unless
         * the body was cut, which is exactly the file a second run of the
         * command continues (ADR 0024 §2).
         */
        if (!resume && rc != HTTP_SHORT) {
            (void)vfs_unlink(argv[2]);
        }

        switch (rc) {
        case HTTP_TLS:
            kshell_printf("wget: %s: %s\n", argv[1], net_tls_last_error());
            break;
        case HTTP_TIMEOUT:
            kshell_printf("wget: %s: the transfer took too long\n", argv[1]);
            break;
        case HTTP_REDIRECT:
            kshell_printf("wget: %s: too many redirects\n", argv[1]);
            break;
        case HTTP_SHORT:
            kshell_printf("wget: %s: the answer ended before the body did\n", argv[1]);
            break;
        case HTTP_SINK:
            kshell_printf("wget: %s: the file could not keep the bytes\n", argv[2]);
            break;
        case HTTP_PROTOCOL:
            kshell_printf("wget: %s: a body this client cannot read\n", argv[1]);
            break;
        case HTTP_TOO_LONG:
            kshell_printf("wget: %s: the head of the answer is longer than %u bytes\n", argv[1],
                          (unsigned)HTTP_HEAD_MAX);
            break;
        default:
            kshell_printf("wget: %s: the server answered %d\n", argv[1], stream.status);
            break;
        }

        return KSHELL_ERROR;
    }

    kshell_printf("wget: HTTP %d, %llu byte(s) to %s", stream.status,
                  (unsigned long long)stream.length, argv[2]);

    if (resume && stream.resumed) {
        kshell_printf(" (resumed at %llu)", (unsigned long long)have);
    } else if (stream.redirects > 0) {
        kshell_printf(" (%u redirect(s))", (unsigned)stream.redirects);
    }

    kshell_printf("\n");

    if (hashed) {
        kshell_printf("sha256: %s\n", digest);
    }

    return KSHELL_OK;
}

/* --- the update over the network --- */

/* The key of an image on a machine that carries no key of its own: a file of
 * /etc/ota, thirty-two bytes in hexadecimal, written by a person or by the
 * bundle of a release. The board has the key of its boot compiled in. */
#define OTA_KEY_PATH "/etc/ota/image.pub"

static void ota_print_state(const struct ota_state *state)
{
    kshell_printf("ota: slot %d %s", state->active, ota_state_name(state->slot_state));

    if (state->sequence > 0) {
        kshell_printf(", %u install(s)", (unsigned)state->sequence);
    }

    if (state->rolled_back) {
        kshell_printf(", rolled back");
    }

    kshell_printf("\n");
}

static int cmd_ota(int argc, char **argv)
{
    struct ota_state state;
    struct ota_config config;
    int rc;

    if (argc < 2) {
        kshell_printf("usage: ota [status|boot|confirm|fetch <url>|key <file>]\n");
        return KSHELL_USAGE;
    }

    if (strcmp(argv[1], "status") == 0) {
        rc = ota_status(&state);

        if (rc != OTA_OK) {
            kshell_printf("ota: %s\n", ota_strerror(rc));
            return KSHELL_ERROR;
        }

        ota_print_state(&state);
        return KSHELL_OK;
    }

    if (strcmp(argv[1], "boot") == 0) {
        rc = ota_boot(&state);

        if (rc != OTA_OK) {
            kshell_printf("ota: %s\n", ota_strerror(rc));
            return KSHELL_ERROR;
        }

        ota_print_state(&state);
        return KSHELL_OK;
    }

    if (strcmp(argv[1], "confirm") == 0) {
        rc = ota_confirm();

        if (rc != OTA_OK) {
            kshell_printf("ota: %s\n", ota_strerror(rc));
            return KSHELL_ERROR;
        }

        if (ota_status(&state) == OTA_OK) {
            kshell_printf("ota: slot %d is valid\n", state.active);
        } else {
            kshell_printf("ota: confirmed\n");
        }

        return KSHELL_OK;
    }

    if (strcmp(argv[1], "fetch") == 0) {
        if (argc != 3) {
            kshell_printf("usage: ota fetch <url>\n");
            return KSHELL_USAGE;
        }

        memset(&config, 0, sizeof(config));
        config.url = argv[2];
        /* The board trusts the key of its boot; a machine whose key is a
         * development one reads /etc/ota. */
#if !MICRO_OS_OTA_BUILTIN_KEY
        config.key_path = OTA_KEY_PATH;
#endif
        config.state = OTA_STATE_DEFAULT;

        rc = ota_install(&config, &state);

        if (rc != OTA_INSTALLED) {
            kshell_printf("ota: %s\n", ota_strerror(rc));
            return KSHELL_ERROR;
        }

        kshell_printf("ota: the image of %s is in slot %d (%s); a reboot is needed\n", argv[2],
                      state.active, ota_state_name(state.slot_state));

        return KSHELL_OK;
    }

    if (strcmp(argv[1], "key") == 0) {
        char text[OTA_KEY_HEX + 4];
        k_u8_t priv[VFS_FILE_DATA_SIZE];
        struct vfs_file file;
        int n;

        if (argc != 3) {
            kshell_printf("usage: ota key <file>\n");
            return KSHELL_USAGE;
        }

        if (vfs_open(argv[2], VFS_O_RDONLY, &file, priv) != 0) {
            kshell_printf("ota: %s is not a file\n", argv[2]);
            return KSHELL_ERROR;
        }

        n = vfs_read(&file, text, sizeof(text) - 1);
        (void)vfs_close(&file);

        if (n < (int)OTA_KEY_HEX - 1) {
            kshell_printf("ota: %s is not a key of thirty-two bytes in hexadecimal\n", argv[2]);
            return KSHELL_ERROR;
        }

        text[n] = '\0';
        kshell_printf("ota: the key of %s is %c%c...%c%c\n", argv[2], text[0], text[1],
                      text[OTA_KEY_HEX - 4], text[OTA_KEY_HEX - 3]);

        return KSHELL_OK;
    }

    kshell_printf("usage: ota [status|boot|confirm|fetch <url>|key <file>]\n");

    return KSHELL_USAGE;
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

/*
 * `ntp [server[:port]]`. The port is the protocol's unless it is written:
 * 123 is the port of every server of the world, and another one is a server
 * of a person or the clock of a test (the host of QEMU user-net cannot take
 * 123 without a privilege).
 */
static int cmd_ntp(int argc, char **argv)
{
    const char *text = argc > 1 ? argv[1] : "pool.ntp.org";
    char server[HTTP_HOST_MAX];
    k_u16_t port = NTP_PORT;
    const char *colon;
    k_size_t length;
    k_u64_t seconds;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    if (argc > 2) {
        kshell_printf("usage: ntp [server[:port]]\n");
        return KSHELL_USAGE;
    }

    colon = strrchr(text, ':');
    if (colon != NULL) {
        const char *digits = colon + 1;
        k_u32_t value = 0;

        if (digits[0] == '\0') {
            kshell_printf("usage: ntp [server[:port]]\n");
            return KSHELL_USAGE;
        }

        for (; *digits != '\0'; digits++) {
            if (*digits < '0' || *digits > '9') {
                kshell_printf("usage: ntp [server[:port]]\n");
                return KSHELL_USAGE;
            }

            value = value * 10u + (k_u32_t)(*digits - '0');
            if (value > 65535u) {
                kshell_printf("usage: ntp [server[:port]]\n");
                return KSHELL_USAGE;
            }
        }

        length = (k_size_t)(colon - text);
        if (length == 0 || length >= sizeof(server)) {
            kshell_printf("usage: ntp [server[:port]]\n");
            return KSHELL_USAGE;
        }

        memcpy(server, text, length);
        server[length] = '\0';
        port = (k_u16_t)value;
    } else {
        if (strlen(text) >= sizeof(server)) {
            kshell_printf("usage: ntp [server[:port]]\n");
            return KSHELL_USAGE;
        }
        memcpy(server, text, strlen(text) + 1);
    }

    if (port == 0) {
        kshell_printf("usage: ntp [server[:port]]\n");
        return KSHELL_USAGE;
    }

    if (net_ntp_sync_port(server, port, &seconds) != 0) {
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
    rc |= kshell_register("wget", "download a file: wget [--sha256] <url> <file>", cmd_wget);
    rc |= kshell_register("ota", "an update: ota [status|boot|confirm|fetch <url>]", cmd_ota);
#endif
#if MICRO_OS_NET_NTP
    rc |= kshell_register("ntp", "set the clock from NTP: ntp [server]", cmd_ntp);
#endif
#if MICRO_OS_NET_HTTP_SERVER
    rc |= kshell_register("httpd", "the servers: httpd [port|off], httpd serve [port] [root]",
                          cmd_httpd);
#endif
#if MICRO_OS_NET_MQTT
    rc |= kshell_register("mqtt", "publish over MQTT: mqtt <host> <port> <topic> <message>",
                          cmd_mqtt);
#endif

    return rc;
}
