/*
 * Micro OS — a small HTTP client.
 *
 * The protocol has no sockets in it: the request is written and the answer
 * is read through a transport, so the host tests replay a canned answer of a
 * server without a network. net.c fills the transport with the sockets of
 * the port.
 *
 * The client speaks HTTP/1.0 and closes the connection after the answer: the
 * board has no room for a pool of keep-alive connections, and a server that
 * speaks 1.1 still answers a 1.0 request with `Connection: close`.
 *
 * Two shapes of the same client live here. http_get() is the client of a
 * line of the console: the body lands in a buffer and a page longer than it
 * is cut. http_stream() is the client of a file: the body goes to a sink as
 * it arrives, so a program of many kilobytes costs no buffer of that size,
 * and it speaks the two bodies of HTTP/1.1 (Content-Length and chunked), a
 * bounded chain of redirects, a Range for a download that was interrupted
 * (ADR 0024 §2).
 */
#ifndef MICRO_OS_NET_HTTP_H
#define MICRO_OS_NET_HTTP_H

#include "kernel/types.h"

#define HTTP_HOST_MAX 64
#define HTTP_PATH_MAX 128

/*
 * The bytes of the head of an answer this client reads before the body. A
 * head of a real server is a few hundred bytes, and the one of GitHub Pages -
 * the repository of `pkg` - is 702: the two hundred of the first release did
 * not hold it, and a download of an index failed with a sentence about the
 * length of the address. The buffer is the ceiling of a head this client
 * reads, and the answer names the number when a server writes more.
 */
#define HTTP_HEAD_MAX 1024u

#define HTTP_OK 0
#define HTTP_ERROR (-1)     /* the transport failed */
#define HTTP_INVALID (-2)   /* the URL is not one we speak */
#define HTTP_TOO_LONG (-3)  /* the answer does not fit the buffers */
#define HTTP_NO_ANSWER (-4) /* the connection closed before the answer */
#define HTTP_TIMEOUT (-5)   /* the exchange outlived its budget */
#define HTTP_REDIRECT (-6)  /* the chain of redirects did not end */
#define HTTP_SINK (-7)      /* the sink refused a byte of the body */
#define HTTP_PROTOCOL (-8)  /* a body the client cannot read */
#define HTTP_SHORT (-9)     /* the body ended before its Content-Length */
#define HTTP_TLS (-10)      /* the HTTPS connection was refused by TLS */

/*
 * The redirects a chain may have. GitHub Pages, a mirror and a release page
 * are three hops at most; a loop is a broken server and not a long way.
 */
#define HTTP_REDIRECT_MAX 5

/* The parts of "http://host:port/path" or "https://host:port/path". */
struct http_url {
    char host[HTTP_HOST_MAX];
    char path[HTTP_PATH_MAX];
    int port;
    bool tls; /* the scheme is https: the transport is wrapped (net/tls.h) */
};

/*
 * A stream of bytes to one peer. `open` returns 0 and sets the context of
 * the connection; `recv` answers the number of the bytes read, 0 when the
 * peer closed the connection, and a negative code on a real error.
 *
 * `open` receives the transport itself, because a transport may wrap the
 * one below it: the TLS layer of net/tls.h takes the plain transport from
 * `user` and secures the same `send`/`recv` pair. A plain transport has
 * `user` empty and ignores the argument.
 */
struct http_transport {
    int (*open)(const struct http_transport *self, const char *host, int port, void **ctx);
    int (*send)(void *ctx, const void *data, k_size_t size);
    int (*recv)(void *ctx, void *data, k_size_t size);
    void (*close)(void *ctx);
    const void *user; /* the transport below, for a layer that wraps one */
};

struct http_response {
    int status;      /* 200, 404, ... */
    k_size_t length; /* the bytes of the body that are in the buffer */
    k_size_t total;  /* Content-Length when the server sent it, else 0 */
    bool truncated;  /* the body was longer than the buffer */
};

/*
 * Where the body of a stream goes. `write` answers the number of the bytes
 * it took, and a short answer is a failure of the sink, not of the
 * protocol: a disk that is full must stop the download instead of filling
 * the memory with what it could not keep.
 *
 * `reset` is called when the answer is the whole object although the request
 * asked for a part of it (a 200 after a Range): the sink has to forget what
 * it had and start at zero. A sink that cannot do that leaves the field
 * empty, and the client then answers the caller with HTTP_ERROR: half of an
 * old file followed by a whole new one is a broken file, and only the sink
 * knows how to open the right one.
 */
struct http_sink {
    void *ctx;
    int (*write)(void *ctx, const void *data, k_size_t size);
    int (*reset)(void *ctx);
};

/*
 * One download. The fields before `status` are the request and the ones from
 * `status` on are the answer, which is what a caller with a small stack can
 * afford: one structure in, one out.
 *
 *   transport     the sockets (of the port, or a fake one in the tests)
 *   url           "http://host[:port]/path" or "https://host[:port]/path";
 *                 the https URL is carried by the TLS layer over this same
 *                 transport (ADR 0024 §3), or is HTTP_TLS in an image built
 *                 without it
 *   sink          the body; a NULL write counts the bytes and keeps none
 *   resume_from   a byte count: the request asks for the rest of the object
 *   max_redirects a bound on the chain; <0 is HTTP_REDIRECT_MAX
 *   timeout_ms    the budget of the whole exchange; 0 is no budget at all
 */
struct http_stream {
    const struct http_transport *transport;
    const char *url;
    struct http_sink sink;
    k_u64_t resume_from;
    int max_redirects;
    k_u32_t timeout_ms;

    int status;     /* the status of the last answer */
    k_u64_t length; /* the bytes of the body that the sink took */
    k_u64_t total;  /* Content-Length of the answer, 0 when it had none */
    k_size_t redirects;
    bool chunked; /* the body came in chunks */
    bool resumed; /* the answer continued the object at resume_from */
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

/*
 * GET the URL and hand the body to the sink as it arrives. The status, the
 * number of the bytes and the shape of the body are left in `stream->status`
 * and the fields after it. A redirect is followed while the chain is shorter
 * than max_redirects: an absolute Location or a path on the same host.
 *
 * The budget of `timeout_ms` is checked between the reads of the transport:
 * a recv that waits longer than the budget is the port's business, and the
 * next read after it answers HTTP_TIMEOUT.
 *
 * Returns HTTP_OK when the body the server promised was handed to the sink,
 * HTTP_SHORT when the connection ended in the middle of it, HTTP_TIMEOUT when
 * the budget ran out, HTTP_SINK when the sink refused the bytes, HTTP_REDIRECT
 * when the chain did not end, HTTP_PROTOCOL when the body is not one of the
 * two shapes of HTTP/1.1, and HTTP_ERROR when the transport failed before an
 * answer.
 */
int http_stream(struct http_stream *stream);

#endif /* MICRO_OS_NET_HTTP_H */
