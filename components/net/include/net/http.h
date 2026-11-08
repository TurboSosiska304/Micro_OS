/*
 * Micro OS — a small HTTP client.
 *
 * The protocol has no sockets in it: the request is written and the answer
 * is read through a transport, so the host tests replay a canned answer of a
 * server without a network. The ESP32 backend fills the transport with the
 * sockets of lwIP.
 *
 * The client speaks HTTP/1.0 and closes the connection after the answer: the
 * board has no room for a pool of keep-alive connections, and a server that
 * speaks 1.1 still answers a 1.0 request with `Connection: close`.
 */
#ifndef MICRO_OS_NET_HTTP_H
#define MICRO_OS_NET_HTTP_H

#include "kernel/types.h"

#define HTTP_HOST_MAX 64
#define HTTP_PATH_MAX 128

#define HTTP_OK 0
#define HTTP_ERROR (-1)     /* the transport failed */
#define HTTP_INVALID (-2)   /* the URL is not one we speak */
#define HTTP_TOO_LONG (-3)  /* the answer does not fit the buffers */
#define HTTP_NO_ANSWER (-4) /* the connection closed before the answer */

/* The parts of "http://host:port/path". */
struct http_url {
    char host[HTTP_HOST_MAX];
    char path[HTTP_PATH_MAX];
    int port;
};

/*
 * A stream of bytes to one peer. `open` returns 0 and sets the context of
 * the connection; `recv` answers the number of the bytes read, 0 when the
 * peer closed the connection, and a negative code on a real error.
 */
struct http_transport {
    int (*open)(const char *host, int port, void **ctx);
    int (*send)(void *ctx, const void *data, k_size_t size);
    int (*recv)(void *ctx, void *data, k_size_t size);
    void (*close)(void *ctx);
};

struct http_response {
    int status;      /* 200, 404, ... */
    k_size_t length; /* the bytes of the body that are in the buffer */
};

/* The URL as its parts; HTTP_INVALID for anything but a plain http URL. */
int http_parse_url(const char *url, struct http_url *out);

/*
 * GET the URL and copy the body of the answer into `body` (at most `size`
 * bytes, the last one is left for the terminating zero). The body may be
 * cut when it is longer than the buffer: this is a client for a line of a
 * console, not for a file of many megabytes.
 */
int http_get(const struct http_transport *transport, const char *url, char *body, k_size_t size,
             struct http_response *out);

#endif /* MICRO_OS_NET_HTTP_H */
