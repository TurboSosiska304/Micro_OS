/*
 * Micro OS — the HTTP client over a transport.
 *
 * The request is one line and a few headers. http_get() reads the answer into
 * a small buffer, because the console of the board has no room for more and a
 * home page of a few kilobytes is the whole point of the command.
 * http_stream() is the same protocol with a sink instead of a buffer: the
 * body is handed over as it arrives, so a program of a repository arrives as
 * a file and not as a hole in the memory of the shell (ADR 0024 §2).
 */
#include "net/http.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/sysinfo.h"
#include "net/tls.h"

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int ci_prefix(const char *line, const char *prefix)
{
    k_size_t i;

    for (i = 0; prefix[i] != '\0'; i++) {
        if (ascii_lower(line[i]) != ascii_lower(prefix[i])) {
            return 0;
        }
    }

    return 1;
}

int http_parse_url(const char *url, struct http_url *out)
{
    const char *p;
    const char *host;
    k_size_t len;
    int port;
    bool tls;

    if (url == NULL || out == NULL) {
        return HTTP_INVALID;
    }

    /*
     * The two schemes of the web. The port of each is the one of the
     * protocol: a URL without a colon is 80 for http and 443 for https,
     * and the TLS layer is chosen by the scheme and nothing else.
     */
    if (strncmp(url, "http://", 7) == 0) {
        tls = false;
        port = 80;
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        tls = true;
        port = 443;
        host = url + 8;
    } else {
        return HTTP_INVALID;
    }

    p = host;
    while (*p != '\0' && *p != '/' && *p != ':') {
        p++;
    }

    len = (k_size_t)(p - host);
    if (len == 0 || len >= HTTP_HOST_MAX) {
        return HTTP_INVALID;
    }

    memcpy(out->host, host, len);
    out->host[len] = '\0';

    if (*p == ':') {
        int value = 0;

        p++;
        if (*p < '0' || *p > '9') {
            return HTTP_INVALID;
        }
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (value > 65535) {
                return HTTP_INVALID;
            }
            p++;
        }
        if (value == 0) {
            return HTTP_INVALID;
        }
        port = value;
    }

    if (*p != '\0' && *p != '/') {
        return HTTP_INVALID;
    }

    len = strlen(*p != '\0' ? p : "/");
    if (len >= HTTP_PATH_MAX) {
        return HTTP_INVALID;
    }

    memcpy(out->path, *p != '\0' ? p : "/", len);
    out->path[len] = '\0';
    out->port = port;
    out->tls = tls;

    return HTTP_OK;
}

/*
 * The transport of an exchange: the one the caller named, or the TLS layer
 * over it when the URL is https. NULL with the reason in net_tls_last_error()
 * when the image has no TLS at all.
 *
 * `secure` belongs to the caller and lives as long as the exchange: it is
 * the template of the TLS layer with `user` pointing at the plain transport.
 */
static const struct http_transport *transport_for(const struct http_transport *plain,
                                                  const struct http_url *parts,
                                                  struct http_transport *secure)
{
    const struct http_transport *tls;

    if (!parts->tls) {
        return plain;
    }

    tls = net_tls_transport();
    if (tls == NULL) {
        return NULL;
    }

    *secure = *tls;
    secure->user = plain;

    return secure;
}

/* The status of the first line: "HTTP/1.0 200 OK". */
static int parse_status(const char *header)
{
    const char *p;

    if (strncmp(header, "HTTP/1.", 7) != 0) {
        return -1;
    }

    p = strchr(header, ' ');
    if (p == NULL) {
        return -1;
    }
    p++;

    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') {
        return -1;
    }

    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

/* The value of Content-Length, or -1 when the answer has none. */
static int parse_content_length(const char *header)
{
    const char *line = strchr(header, '\n');

    while (line != NULL) {
        k_size_t i = 1;

        /* The end of the line, without its carriage return. */
        while (line[i] != '\0' && line[i] != '\n') {
            i++;
        }

        if (ci_prefix(line + 1, "content-length:")) {
            const char *p = line + 16;
            int value = 0;
            int digits = 0;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
            while (p < line + i && *p >= '0' && *p <= '9') {
                value = value * 10 + (*p - '0');
                if (value > 1024 * 1024) {
                    return -1;
                }
                digits++;
                p++;
            }

            return digits > 0 ? value : -1;
        }

        line = strchr(line + 1, '\n');
    }

    return -1;
}

int http_get(const struct http_transport *transport, const char *url, char *body, k_size_t size,
             struct http_response *out)
{
    struct http_url parts;
    struct http_transport secure;
    const struct http_transport *plain = transport;
    char request[HTTP_HOST_MAX + HTTP_PATH_MAX + 64];
    char header[512];
    char *end;
    k_size_t header_used = 0;
    k_size_t body_used = 0;
    k_size_t start;
    k_size_t available;
    int content_length = -1;
    int status;
    int n;
    void *ctx = NULL;

    if (transport == NULL || transport->open == NULL || transport->send == NULL ||
        transport->recv == NULL || transport->close == NULL || body == NULL || size == 0 ||
        out == NULL) {
        return HTTP_INVALID;
    }

    memset(out, 0, sizeof(*out));
    body[0] = '\0';

    if (http_parse_url(url, &parts) != HTTP_OK) {
        return HTTP_INVALID;
    }

    transport = transport_for(plain, &parts, &secure);
    if (transport == NULL) {
        return HTTP_TLS;
    }

    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MicroOS\r\nConnection: close\r\n\r\n",
                 parts.path, parts.host);
    if (n < 0 || (k_size_t)n >= sizeof(request)) {
        return HTTP_TOO_LONG;
    }

    if (transport->open(transport, parts.host, parts.port, &ctx) != 0) {
        return parts.tls ? HTTP_TLS : HTTP_ERROR;
    }

    if (transport->send(ctx, request, (k_size_t)n) != n) {
        transport->close(ctx);
        return HTTP_ERROR;
    }

    /*
     * The answer may come in pieces, and the end of the headers may sit
     * across two of them: the buffer keeps what has arrived and the search
     * looks at the whole of it every time.
     */
    while (header_used + 1 < sizeof(header)) {
        n = transport->recv(ctx, header + header_used, sizeof(header) - 1 - header_used);
        if (n < 0) {
            transport->close(ctx);
            return HTTP_ERROR;
        }
        if (n == 0) {
            break;
        }

        header_used += (k_size_t)n;
        header[header_used] = '\0';

        end = strstr(header, "\r\n\r\n");
        if (end != NULL) {
            break;
        }
    }

    header[header_used] = '\0';
    end = strstr(header, "\r\n\r\n");
    if (end == NULL) {
        transport->close(ctx);
        return header_used + 1 >= sizeof(header) ? HTTP_TOO_LONG : HTTP_NO_ANSWER;
    }

    status = parse_status(header);
    if (status < 0) {
        transport->close(ctx);
        return HTTP_NO_ANSWER;
    }

    content_length = parse_content_length(header);

    start = (k_size_t)(end - header) + 4;
    available = header_used - start;
    if (available > size - 1) {
        available = size - 1;
    }
    memcpy(body, header + start, available);
    body_used = available;

    while (body_used < size - 1) {
        if (content_length >= 0 && body_used >= (k_size_t)content_length) {
            break;
        }

        n = transport->recv(ctx, body + body_used, size - 1 - body_used);
        if (n < 0) {
            transport->close(ctx);
            return HTTP_ERROR;
        }
        if (n == 0) {
            break;
        }

        body_used += (k_size_t)n;
    }

    transport->close(ctx);

    body[body_used] = '\0';
    out->status = status;
    out->length = body_used;

    return HTTP_OK;
}

/* --- the stream: a body that goes where it is told --- */

/* The head of an answer is a few hundred bytes; a longer one is a server
 * that is not answering a request of this client. */
#define HTTP_HEAD_MAX 640

/* One read of the socket. The body is handed over in pieces of this size, so
 * the buffer is the price of the stream and a program costs no more. */
#define HTTP_READ_MAX 256

/* The bytes of a body that is not a success (a 404 of a mirror) that are read
 * and thrown away: enough for the sentence of the server, and not a reason to
 * hold the console for a page nobody asked for. */
#define HTTP_DISCARD_MAX 2048

/* The reader: the socket and the bytes of it that the parser has not used. */
struct http_reader {
    const struct http_transport *transport;
    void *ctx;
    k_u64_t deadline;
    bool timed_out;
    k_size_t used;
    k_size_t pos;
    char buf[HTTP_READ_MAX];
};

static bool reader_expired(const struct http_reader *reader)
{
    return reader->deadline != 0u && k_sysinfo_uptime_ms() >= reader->deadline;
}

/*
 * More bytes in the buffer: the count still available, 0 at the end of the
 * answer and -1 on an error or an expired budget (reader->timed_out says
 * which of the two).
 */
static int reader_fill(struct http_reader *reader)
{
    int n;

    if (reader->pos < reader->used) {
        return (int)(reader->used - reader->pos);
    }

    if (reader_expired(reader)) {
        reader->timed_out = true;
        return -1;
    }

    n = reader->transport->recv(reader->ctx, reader->buf, sizeof(reader->buf));
    if (n <= 0) {
        return n;
    }

    reader->used = (k_size_t)n;
    reader->pos = 0;

    return n;
}

static int reader_byte(struct http_reader *reader, char *out)
{
    if (reader_fill(reader) <= 0) {
        return -1;
    }

    *out = reader->buf[reader->pos++];

    return 0;
}

static int reader_read(struct http_reader *reader, void *out, k_size_t size)
{
    k_u8_t *dst = (k_u8_t *)out;
    k_size_t done = 0;

    while (done < size) {
        k_size_t take;

        if (reader_fill(reader) <= 0) {
            return -1;
        }

        take = reader->used - reader->pos;
        if (take > size - done) {
            take = size - done;
        }

        memcpy(dst + done, reader->buf + reader->pos, take);
        reader->pos += take;
        done += take;
    }

    return 0;
}

/* The head of an answer, the empty line included, as a string. */
static int stream_head(struct http_reader *reader, char *head, k_size_t size)
{
    k_size_t used = 0;

    while (used + 1 < size) {
        if (reader_byte(reader, &head[used]) != 0) {
            return reader->timed_out ? HTTP_TIMEOUT : HTTP_NO_ANSWER;
        }

        used++;

        if (used >= 4 && memcmp(head + used - 4, "\r\n\r\n", 4) == 0) {
            head[used] = '\0';
            return HTTP_OK;
        }
    }

    return HTTP_TOO_LONG;
}

/* The value of a header field, without its trailing carriage return. */
static bool header_field(const char *head, const char *name, char *out, k_size_t size)
{
    const char *line = strstr(head, "\r\n");
    k_size_t name_len = strlen(name);

    if (out != NULL && size > 0) {
        out[0] = '\0';
    }

    while (line != NULL) {
        const char *value;
        const char *end;

        line += 2;

        if (line[0] == '\r' && line[1] == '\n') {
            break; /* the end of the head */
        }

        if (!ci_prefix(line, name)) {
            line = strstr(line, "\r\n");
            continue;
        }

        value = line + name_len;
        while (*value == ' ' || *value == '\t') {
            value++;
        }

        end = strstr(value, "\r\n");
        if (end == NULL) {
            end = value + strlen(value);
        }

        if (out != NULL && size > 0) {
            k_size_t len = (k_size_t)(end - value);

            if (len >= size) {
                len = size - 1;
            }
            memcpy(out, value, len);
            out[len] = '\0';
        }

        return true;
    }

    return false;
}

/* Content-Length as a length of a file and not of a page: no cap here. */
static bool header_content_length(const char *head, k_u64_t *out)
{
    char text[24];
    k_u64_t value = 0;
    k_size_t i;

    if (!header_field(head, "Content-Length:", text, sizeof(text)) || text[0] == '\0') {
        return false;
    }

    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        if (value > (0xFFFFFFFFFFFFFFFFull - (k_u64_t)(text[i] - '0')) / 10u) {
            return false;
        }

        value = value * 10u + (k_u64_t)(text[i] - '0');
    }

    *out = value;

    return true;
}

/* The size of one chunk: hexadecimal, with the extensions after ';' ignored. */
static bool chunk_size(const char *line, k_u64_t *out)
{
    k_u64_t value = 0;
    k_size_t i = 0;
    int digits = 0;

    while (line[i] == ' ' || line[i] == '\t') {
        i++;
    }

    for (; line[i] != '\0' && line[i] != ';'; i++) {
        char c = line[i];
        k_u32_t digit;

        if (c >= '0' && c <= '9') {
            digit = (k_u32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (k_u32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (k_u32_t)(c - 'A' + 10);
        } else {
            return false;
        }

        if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 16u) {
            return false;
        }

        value = value * 16u + digit;
        digits++;
    }

    if (digits == 0) {
        return false;
    }

    *out = value;

    return true;
}

static int sink_take(struct http_stream *stream, const char *data, k_size_t size)
{
    if (stream->sink.write == NULL) {
        return (int)size;
    }

    return stream->sink.write(stream->sink.ctx, data, size);
}

/* Hand `count` bytes of the body to the sink. */
static int stream_body_count(struct http_reader *reader, struct http_stream *stream, k_u64_t count)
{
    while (count > 0) {
        k_size_t take;

        if (reader_fill(reader) <= 0) {
            return reader->timed_out ? HTTP_TIMEOUT : HTTP_SHORT;
        }

        take = reader->used - reader->pos;
        if ((k_u64_t)take > count) {
            take = (k_size_t)count;
        }

        if (sink_take(stream, reader->buf + reader->pos, take) != (int)take) {
            return HTTP_SINK;
        }

        reader->pos += take;
        count -= (k_u64_t)take;
        stream->length += (k_u64_t)take;
    }

    return HTTP_OK;
}

/* The body that ends when the connection does. */
static int stream_body_until_close(struct http_reader *reader, struct http_stream *stream)
{
    for (;;) {
        k_size_t take;
        int n = reader_fill(reader);

        if (n == 0) {
            return HTTP_OK;
        }

        if (n < 0) {
            return reader->timed_out ? HTTP_TIMEOUT : HTTP_ERROR;
        }

        take = reader->used - reader->pos;

        if (sink_take(stream, reader->buf + reader->pos, take) != (int)take) {
            return HTTP_SINK;
        }

        reader->pos = reader->used;
        stream->length += (k_u64_t)take;
    }
}

/* One line of the chunked framing, without its CRLF. */
static int chunk_line(struct http_reader *reader, char *line, k_size_t size)
{
    k_size_t used = 0;

    for (;;) {
        char c;

        if (reader_byte(reader, &c) != 0) {
            return reader->timed_out ? HTTP_TIMEOUT : HTTP_SHORT;
        }

        if (c == '\n') {
            if (used > 0 && line[used - 1] == '\r') {
                used--;
            }
            line[used] = '\0';
            return HTTP_OK;
        }

        if (used + 1 >= size) {
            return HTTP_PROTOCOL;
        }

        line[used++] = c;
    }
}

/* The body of HTTP/1.1: a size in hexadecimal, the bytes, and a zero ends it. */
static int stream_body_chunked(struct http_reader *reader, struct http_stream *stream)
{
    char line[32];

    for (;;) {
        k_u64_t size;
        char end[2];
        int rc;

        rc = chunk_line(reader, line, sizeof(line));
        if (rc != HTTP_OK) {
            return rc;
        }

        if (!chunk_size(line, &size)) {
            return HTTP_PROTOCOL;
        }

        if (size == 0) {
            /*
             * The end of the body is the empty line after the trailers. A
             * server that sends none still ends the body with one CRLF, and
             * a connection that closes here is the end of it too.
             */
            for (;;) {
                rc = chunk_line(reader, line, sizeof(line));
                if (rc != HTTP_OK) {
                    return rc == HTTP_SHORT ? HTTP_OK : rc;
                }
                if (line[0] == '\0') {
                    return HTTP_OK;
                }
            }
        }

        rc = stream_body_count(reader, stream, size);
        if (rc != HTTP_OK) {
            return rc;
        }

        if (reader_read(reader, end, 2) != 0) {
            return reader->timed_out ? HTTP_TIMEOUT : HTTP_SHORT;
        }

        if (end[0] != '\r' || end[1] != '\n') {
            return HTTP_PROTOCOL;
        }
    }
}

/* The body of the answer, in the shape the head named. */
static int stream_body(struct http_reader *reader, struct http_stream *stream, const char *head,
                       bool deliver)
{
    char encoding[32];
    k_u64_t length = 0;
    bool has_length = header_content_length(head, &length);
    int rc;

    stream->chunked = false;
    stream->total = 0;

    if (header_field(head, "Transfer-Encoding:", encoding, sizeof(encoding)) &&
        strstr(encoding, "chunked") != NULL) {
        stream->chunked = true;
    }

    if (!deliver) {
        /*
         * A refusal is read and thrown away: the caller asks the status and
         * not the page of the mirror. Nothing goes to the sink, and the
         * length of the answer is zero, because a body the caller did not
         * get is not a body.
         */
        struct http_stream discard = *stream;

        discard.sink.write = NULL;
        discard.length = 0;

        if (stream->chunked) {
            rc = stream_body_chunked(reader, &discard);
        } else if (has_length) {
            rc = stream_body_count(reader, &discard,
                                   length > HTTP_DISCARD_MAX ? HTTP_DISCARD_MAX : length);
            rc = (rc == HTTP_SHORT) ? HTTP_OK : rc;
        } else {
            rc = stream_body_until_close(reader, &discard);
        }

        stream->total = has_length ? length : discard.length;

        return rc == HTTP_TIMEOUT ? HTTP_TIMEOUT : HTTP_OK;
    }

    if (has_length) {
        stream->total = length;
    }

    if (stream->chunked) {
        return stream_body_chunked(reader, stream);
    }

    if (has_length) {
        return stream_body_count(reader, stream, length);
    }

    return stream_body_until_close(reader, stream);
}

/* The URL a redirect points at: an absolute one, or a path of the same host. */
static int redirect_url(const struct http_url *parts, const char *location, char *out,
                        k_size_t size)
{
    const char *scheme = parts->tls ? "https" : "http";
    const char *slash;
    int n;

    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        if (strlen(location) >= size) {
            return HTTP_TOO_LONG;
        }

        memcpy(out, location, strlen(location) + 1);

        return HTTP_OK;
    }

    if (location[0] == '/') {
        n = snprintf(out, size, "%s://%s:%d%s", scheme, parts->host, parts->port, location);
    } else {
        /* A path without a leading slash replaces the last part of the
         * path: "/a/b" with "c" is "/a/c". The base of the tree is the
         * server's business and not a URL this client invents. */
        slash = strrchr(parts->path, '/');

        if (slash == NULL) {
            return HTTP_INVALID;
        }

        n = snprintf(out, size, "%s://%s:%d%.*s%s", scheme, parts->host, parts->port,
                     (int)(slash - parts->path) + 1, parts->path, location);
    }

    if (n < 0 || (k_size_t)n >= size) {
        return HTTP_TOO_LONG;
    }

    return HTTP_OK;
}

static bool redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

int http_stream(struct http_stream *stream)
{
    char head[HTTP_HEAD_MAX];
    char current[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
    char next[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
    char request[HTTP_HOST_MAX + HTTP_PATH_MAX + 96];
    struct http_transport secure;
    const struct http_transport *plain;
    int max_redirects;
    int rc;

    if (stream == NULL || stream->url == NULL) {
        return HTTP_INVALID;
    }

    if (stream->transport == NULL || stream->transport->open == NULL ||
        stream->transport->send == NULL || stream->transport->recv == NULL ||
        stream->transport->close == NULL) {
        return HTTP_INVALID;
    }

    if (strlen(stream->url) >= sizeof(current)) {
        return HTTP_TOO_LONG;
    }

    memcpy(current, stream->url, strlen(stream->url) + 1);

    max_redirects = stream->max_redirects;
    if (max_redirects < 0) {
        max_redirects = HTTP_REDIRECT_MAX;
    }

    plain = stream->transport;

    /* The answer always starts empty, whatever the caller left there. */
    stream->status = 0;
    stream->length = 0;
    stream->total = 0;
    stream->redirects = 0;
    stream->chunked = false;
    stream->resumed = false;

    for (;;) {
        struct http_url parts;
        struct http_reader reader;
        const struct http_transport *transport;
        void *ctx = NULL;
        int n;

        if (http_parse_url(current, &parts) != HTTP_OK) {
            return HTTP_INVALID;
        }

        /*
         * The scheme of the hop decides the layer: a redirect from http to
         * https is a second exchange through TLS, and one back is plain.
         * The template is copied per hop, so its `user` names the transport
         * of this stream and no other.
         */
        transport = transport_for(plain, &parts, &secure);
        if (transport == NULL) {
            return HTTP_TLS;
        }

        n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MicroOS\r\n"
                     "Accept: */*\r\nConnection: close\r\n",
                     parts.path, parts.host);

        if (n < 0 || (k_size_t)n >= sizeof(request)) {
            return HTTP_TOO_LONG;
        }

        if (stream->resume_from > 0u) {
            int more = snprintf(request + n, sizeof(request) - (k_size_t)n,
                                "Range: bytes=%llu-\r\n", (unsigned long long)stream->resume_from);

            if (more < 0 || (k_size_t)more >= sizeof(request) - (k_size_t)n) {
                return HTTP_TOO_LONG;
            }

            n += more;
        }

        if ((k_size_t)n + 2 >= sizeof(request)) {
            return HTTP_TOO_LONG;
        }

        memcpy(request + n, "\r\n", 3);
        n += 2;

        if (transport->open(transport, parts.host, parts.port, &ctx) != 0) {
            return parts.tls ? HTTP_TLS : HTTP_ERROR;
        }

        if (transport->send(ctx, request, (k_size_t)n) != n) {
            transport->close(ctx);
            return HTTP_ERROR;
        }

        reader.transport = transport;
        reader.ctx = ctx;
        reader.timed_out = false;
        reader.used = 0;
        reader.pos = 0;
        reader.deadline = 0;
        if (stream->timeout_ms > 0u) {
            reader.deadline = k_sysinfo_uptime_ms() + (k_u64_t)stream->timeout_ms;
        }

        rc = stream_head(&reader, head, sizeof(head));
        if (rc != HTTP_OK) {
            transport->close(ctx);
            return rc;
        }

        stream->status = parse_status(head);
        if (stream->status < 0) {
            transport->close(ctx);
            return HTTP_NO_ANSWER;
        }

        if (redirect_status(stream->status)) {
            char location[HTTP_PATH_MAX];

            if (!header_field(head, "Location:", location, sizeof(location)) ||
                location[0] == '\0') {
                transport->close(ctx);
                return HTTP_REDIRECT;
            }

            if (stream->redirects >= (k_size_t)max_redirects) {
                transport->close(ctx);
                return HTTP_REDIRECT;
            }

            rc = redirect_url(&parts, location, next, sizeof(next));
            if (rc != HTTP_OK) {
                transport->close(ctx);
                return rc;
            }

            transport->close(ctx);
            memcpy(current, next, strlen(next) + 1);
            stream->redirects++;

            continue;
        }

        /*
         * An answer that is not a part of the object asked for is not the
         * file: the sink starts at zero, and a sink that cannot forget what
         * it had is a caller that must open its own file again and ask
         * again. This is the 200 of a server that ignores Range.
         */
        if (stream->resume_from > 0u) {
            if (stream->status == 206) {
                stream->resumed = true;
            } else if (stream->status == 200) {
                if (stream->sink.reset == NULL) {
                    transport->close(ctx);
                    return HTTP_ERROR;
                }

                if (stream->sink.reset(stream->sink.ctx) != 0) {
                    transport->close(ctx);
                    return HTTP_SINK;
                }

                stream->resumed = false;
            }
        }

        rc = stream_body(&reader, stream, head, stream->status >= 200 && stream->status < 300);

        transport->close(ctx);

        return rc;
    }
}
