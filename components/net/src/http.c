/*
 * Micro OS — the HTTP client over a transport.
 *
 * The request is one line and a few headers, the answer is read into a small
 * buffer: the console of the board has no room for more, and a home page of
 * a few kilobytes is the whole point of the command.
 */
#include "net/http.h"

#include <stdio.h>
#include <string.h>

static int ci_prefix(const char *line, const char *prefix)
{
    k_size_t i;

    for (i = 0; prefix[i] != '\0'; i++) {
        char c = line[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }

        if (c != prefix[i]) {
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
    int port = 80;

    if (url == NULL || out == NULL) {
        return HTTP_INVALID;
    }

    if (strncmp(url, "http://", 7) != 0) {
        return HTTP_INVALID;
    }

    host = url + 7;

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

    return HTTP_OK;
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

    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MicroOS\r\nConnection: close\r\n\r\n",
                 parts.path, parts.host);
    if (n < 0 || (k_size_t)n >= sizeof(request)) {
        return HTTP_TOO_LONG;
    }

    if (transport->open(parts.host, parts.port, &ctx) != 0) {
        return HTTP_ERROR;
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
