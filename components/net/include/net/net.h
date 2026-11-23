/*
 * Micro OS — the network services of the board: HTTP, NTP and MQTT.
 *
 * The protocol parts (the URL, the request, the packet of NTP, the page of
 * the state, the packet of MQTT) live in http.c, ntp.c, net_http_server.c
 * and mqtt.c and are tested on the host; this file is the four commands of
 * the shell, and the sockets under them belong to the kernel
 * (kernel/net.h), which lends them from the port. Nothing here knows which
 * stack resolved the name.
 */
#ifndef MICRO_OS_NET_NET_H
#define MICRO_OS_NET_NET_H

#include "kernel/types.h"
#include "net/http.h"

/*
 * The milliseconds one exchange spends before it gives up. The board sits
 * on a home network: a command of the shell has to answer even when that
 * network, or the server of the moment, has gone away.
 */
#define NET_TIMEOUT_MS 5000

/*
 * The budget of a file: a program of a repository is a hundred kilobytes on
 * a link that may be slow, and the timeout of one read is not the timeout of
 * the whole download.
 */
#define NET_DOWNLOAD_TIMEOUT_MS 120000u

/* Register the commands `http`, `ntp`, `httpd`, `mqtt` and `wget`. */
int net_register(void);

/* GET a URL through the sockets of the port. */
int net_http_get(const char *url, char *body, k_size_t size, struct http_response *out);

/* The transport of the port, for a caller that streams on its own. */
const struct http_transport *net_http_transport(void);

/*
 * GET a URL into a file of the VFS, through the transport of the caller (a
 * fake one in the tests, NULL for the sockets of the port). With `resume`
 * and an existing file the request asks for the rest of it (a Range), and a
 * server that answers with the whole object starts the file again. The
 * answer of the exchange - the status above all - is left in `out`, which
 * may be NULL when the caller wants only the result of the call.
 *
 * Returns HTTP_OK when the file holds the object the server promised, and
 * the code of http_stream() otherwise. A status that is not 200 or 206 is
 * HTTP_ERROR with the status in `out`: the body of a refusal is not a file.
 */
int net_http_download_to(const struct http_transport *transport, const char *url, const char *path,
                         bool resume, struct http_stream *out);

/* The same over the sockets of the port. */
int net_http_download(const char *url, const char *path, bool resume, struct http_stream *out);

/* The time of an NTP server, in the seconds of Unix (UTC). */
int net_ntp_sync(const char *server, k_u64_t *unix_seconds);

/*
 * The same with the port named: 123 is the port of the protocol, and a
 * clock on another one is the case of a test (the host of QEMU user-net
 * cannot bind 123 without a privilege) and of a home server. `server` is a
 * name or an address, as it is for net_ntp_sync().
 */
int net_ntp_sync_port(const char *server, k_u16_t port, k_u64_t *unix_seconds);

#endif /* MICRO_OS_NET_NET_H */
