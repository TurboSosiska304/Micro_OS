/*
 * Micro OS — the server of files over HTTP (net/httpd.h).
 *
 * One task takes the door, and one task per session answers the request of
 * its client: the kernel lends a TCP session and nothing of it polls, so a
 * session that waits is a task that sleeps. The ceiling of the sessions is
 * the table of the tasks (net/httpd.h), and a client over it is answered
 * with 503 and closed instead of being left in a queue - an honest refusal
 * is a thing a client can read.
 *
 * The files are the VFS. A request answers a file of the volume, a listing
 * of a directory (chunked, built as it is sent) or one of the refusals; a
 * path is decoded and walked segment by segment under the root, and a `..`
 * that would climb out of it is 403. Nothing is written: GET and HEAD only.
 *
 * The headers are the ones a person's browser expects and a monitor can
 * use: Content-Length, Content-Type from the extension, ETag with the size
 * and the moment of the change, 304 for the client that already has it,
 * Accept-Ranges and a single byte range (206, 416). A connection is kept
 * alive between requests until a client says no, the idle timeout of the
 * port says it is gone, or the count of the requests of one session is
 * spent.
 *
 * Every request is one line of a log: the console, or the file the
 * configuration names (HTTPD_LOG_PATH). The line has the method, the target,
 * the status and the bytes of the body.
 */
#include "net/httpd.h"

#include <string.h>

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "net/tls_server.h"

/* The byte that means "to the end" in a Range and in a length. */
#define HTTPD_SIZE_UNKNOWN ((k_u64_t) - 1)

/* The longest head of an answer and the longest line of a listing. */
#define HTTPD_HEAD_MAX 640
#define HTTPD_LIST_MAX 640

/* The wait of the listener, short enough to notice httpd_stop(). */
#define HTTPD_ACCEPT_TIMEOUT_MS 300

/* The step of the waits that read the clock, so a stop is noticed. */
#define HTTPD_POLL_MS 10

/* The wait of httpd_stop() for the door and then for the sessions. */
#define HTTPD_STOP_WAIT_MS 6000u
#define HTTPD_STOP_STEP_MS 50

/* The state of the server, one of it: the server is one port of one machine. */
static bool s_enabled;
static bool s_serving;
static bool s_tls;
static k_u16_t s_port;
static k_server_t *s_server;
static char s_root[HTTPD_PATH_MAX];
static char s_log[VFS_PATH_MAX];
static k_u8_t s_max_connections;
static k_u32_t s_idle_timeout_ms;

/* The counters, read by the console while the sessions change them. */
static k_u32_t s_requests;
static k_u32_t s_refused;
static k_u32_t s_active;

/*
 * The pids of the sessions of this server. A task that takes a slot of the
 * table has to give it back, and only the parent may: `task_wait_any()` of
 * the kernel reaps any zombie of the table, so a server that called it would
 * take the exit of a program the shell is waiting for. The list is the
 * children of the door and nothing else, and the accept task reaps them by
 * pid in the seconds of its own wait.
 */
static k_u32_t s_session_pids[HTTPD_CONNECTIONS_MAX];
static k_size_t s_session_count;

static void sessions_reap(void)
{
    k_size_t i = 0;

    while (i < s_session_count) {
        int rc = task_wait(s_session_pids[i], NULL, 0);

        if (rc == 0 || rc == -1) {
            k_size_t j;

            for (j = i + 1; j < s_session_count; j++) {
                s_session_pids[j - 1] = s_session_pids[j];
            }

            s_session_count--;
            continue;
        }

        i++;
    }
}

static bool server_enabled(void)
{
    return __atomic_load_n(&s_enabled, __ATOMIC_SEQ_CST);
}

static bool server_serving(void)
{
    return __atomic_load_n(&s_serving, __ATOMIC_SEQ_CST);
}

static void server_set_enabled(bool value)
{
    __atomic_store_n(&s_enabled, value, __ATOMIC_SEQ_CST);
}

static void server_set_serving(bool value)
{
    __atomic_store_n(&s_serving, value, __ATOMIC_SEQ_CST);
}

static k_u32_t counter_active(void)
{
    return __atomic_load_n(&s_active, __ATOMIC_SEQ_CST);
}

static void counter_add(k_u32_t *counter)
{
    __atomic_add_fetch(counter, 1u, __ATOMIC_SEQ_CST);
}

static void counter_sub(k_u32_t *counter)
{
    __atomic_sub_fetch(counter, 1u, __ATOMIC_SEQ_CST);
}

/* --- the text of an answer --- */

static void text_append(char *buf, k_size_t size, k_size_t *len, const char *text)
{
    k_size_t n = strlen(text);
    k_size_t space;

    if (size == 0 || *len >= size - 1) {
        return;
    }

    space = size - 1 - *len;
    if (n > space) {
        n = space;
    }

    memcpy(buf + *len, text, n);
    *len += n;
    buf[*len] = '\0';
}

static void text_append_u64(char *buf, k_size_t size, k_size_t *len, k_u64_t value)
{
    char digits[21];
    k_size_t i = sizeof(digits) - 1;

    digits[i] = '\0';

    do {
        digits[--i] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);

    text_append(buf, size, len, digits + i);
}

/* A length in hexadecimal: the characters of an ETag and of a Content-Range
 * have to be the ones a client compares byte for byte. */
static void text_append_hex(char *buf, k_size_t size, k_size_t *len, k_u64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[17];
    k_size_t i = sizeof(text) - 1;

    text[i] = '\0';

    do {
        text[--i] = digits[value & 0x0Fu];
        value >>= 4;
    } while (value != 0);

    text_append(buf, size, len, text + i);
}

/* Whether `text` is `word` in any case, and ends where `word` ends. */
static bool text_equal_nocase(const char *text, const char *word, k_size_t word_len)
{
    k_size_t i;

    for (i = 0; i < word_len; i++) {
        char left = text[i];
        char right = word[i];

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }

        if (left != right) {
            return false;
        }
    }

    return text[word_len] == '\0';
}

/* Whether a value of a header is one word, exactly and in any case. */
static bool value_is(const char *value, k_size_t len, const char *word)
{
    k_size_t word_len = strlen(word);
    k_size_t i;

    if (len != word_len) {
        return false;
    }

    for (i = 0; i < len; i++) {
        char left = value[i];
        char right = word[i];

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }

        if (left != right) {
            return false;
        }
    }

    return true;
}

/* --- the stream of a session --- */

/*
 * A session is the socket of the kernel, or that socket behind TLS when the
 * server was started with a certificate. The two shapes are one `struct
 * httpd_stream` so that the request and the answer below never ask which one
 * they have.
 */
struct httpd_stream {
    k_socket_t *socket;
    void *tls; /* net_tls_server_* context, NULL for plain HTTP */
};

static int stream_send(struct httpd_stream *stream, const void *data, k_size_t size)
{
    if (size == 0) {
        return 0;
    }

    if (stream->tls != NULL) {
        return net_tls_server_send(stream->tls, data, size);
    }

    return k_socket_send(stream->socket, data, size, true);
}

static int stream_recv(struct httpd_stream *stream, void *data, k_size_t size)
{
    if (stream->tls != NULL) {
        return net_tls_server_recv(stream->tls, data, size);
    }

    return k_socket_recv(stream->socket, data, size);
}

/* The whole block, or a failure: a half page is worse than a slow one. */
static bool stream_send_all(struct httpd_stream *stream, const void *data, k_size_t size)
{
    const char *bytes = data;
    k_size_t sent = 0;

    while (sent < size) {
        int n = stream_send(stream, bytes + sent, size - sent);

        if (n <= 0) {
            return false;
        }

        sent += (k_size_t)n;
    }

    return true;
}

/* --- the request --- */

struct httpd_request {
    const char *method;
    k_size_t method_len;
    const char *target; /* the raw target, for the log */
    k_size_t target_len;
    /* The path, decoded and normalised: an absolute path of the VFS below
     * the root, without a trailing slash except for "/" itself. */
    char path[HTTPD_PATH_MAX];
    bool dir_slash; /* the target asked for a directory: it ended in "/" */
    bool keep_alive;
    bool has_body; /* the client promised bytes after the head */
    bool has_range;
    bool range_suffix;
    k_u64_t range_first;
    k_u64_t range_last; /* HTTPD_SIZE_UNKNOWN for "to the end" */
    bool has_etag;
    char etag[48];
    k_size_t head_len; /* the bytes of the buffer the request took */
};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

/* The answers of the walk of a target. */
#define TARGET_OK 0
#define TARGET_ESCAPE (-1) /* a `..` that would leave the root */
#define TARGET_BAD (-2)    /* an escape, a zero byte or a name too long */

/*
 * The percent-decoding of a target, then the walk of its segments. `out` is
 * the path under the root: a `..` that would leave it fails, and so does a
 * byte that cannot be in the name of a file of the VFS. A query or a
 * fragment is not a part of the path: the server has no parameters.
 */
static int target_normalize(const char *target, k_size_t len, char out[HTTPD_PATH_MAX],
                            bool *dir_slash)
{
    char decoded[HTTPD_PATH_MAX];
    k_size_t used = 0;
    k_size_t i;

    *dir_slash = false;

    if (len == 0 || target[0] != '/') {
        return TARGET_BAD;
    }

    for (i = 0; i < len && target[i] != '?' && target[i] != '#'; i++) {
        char c = target[i];

        if (c == '%') {
            int high;
            int low;

            if (i + 2 >= len) {
                return TARGET_BAD;
            }

            high = hex_digit(target[i + 1]);
            low = hex_digit(target[i + 2]);

            if (high < 0 || low < 0) {
                return TARGET_BAD;
            }

            c = (char)((high << 4) | low);
            i += 2;
        }

        /* A zero ends a name before its length does, and a backslash is not
         * a separator here: both are refusals of the request, not of the
         * file system. */
        if (c == '\0' || c == '\\') {
            return TARGET_BAD;
        }

        if (used >= sizeof(decoded) - 1) {
            return TARGET_BAD;
        }

        decoded[used++] = c;
    }

    decoded[used] = '\0';

    if (used > 0 && decoded[used - 1] == '/') {
        *dir_slash = true;
    }

    /* The walk of the segments: "." and an empty segment are the current
     * directory, ".." goes up, and up from the root is the refusal. */
    used = 0;
    i = 0;

    while (decoded[i] != '\0') {
        k_size_t start;
        k_size_t length;

        while (decoded[i] == '/') {
            i++;
        }

        start = i;
        while (decoded[i] != '\0' && decoded[i] != '/') {
            i++;
        }

        length = i - start;

        if (length == 0 || (length == 1 && decoded[start] == '.')) {
            continue;
        }

        if (length == 2 && decoded[start] == '.' && decoded[start + 1] == '.') {
            if (used == 0) {
                return TARGET_ESCAPE;
            }

            while (used > 0 && out[used - 1] != '/') {
                used--;
            }
            if (used > 0) {
                used--;
            }
            continue;
        }

        if (used + 1 + length >= HTTPD_PATH_MAX) {
            return TARGET_BAD;
        }

        out[used++] = '/';
        memcpy(out + used, decoded + start, length);
        used += length;
    }

    out[used] = '\0';

    return TARGET_OK;
}

/* The name of a header line is `name:` at the start of the line. */
static bool header_named(const char *line, k_size_t line_len, const char *name, const char **value,
                         k_size_t *value_len)
{
    k_size_t name_len = strlen(name);
    k_size_t i;

    if (line_len <= name_len || line[name_len] != ':') {
        return false;
    }

    /* The name of a header is compared without the terminator: the line
     * continues with the colon and the value. */
    if (!value_is(line, name_len, name)) {
        return false;
    }

    i = name_len + 1;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }

    *value = line + i;
    *value_len = line_len - i;

    while (*value_len > 0 &&
           ((*value)[*value_len - 1] == '\r' || (*value)[*value_len - 1] == ' ')) {
        (*value_len)--;
    }

    return true;
}

/* The offset just past the empty line that ends the head, 0 when it is not
 * there yet. Both endings are taken: a client of HTTP/1.1 sends CRLF, and a
 * program of a test may send the line feed alone. */
static k_size_t head_end(const char *buf, k_size_t len)
{
    k_size_t i;

    for (i = 1; i < len; i++) {
        if (buf[i] == '\n' && buf[i - 1] == '\n') {
            return i + 1;
        }

        if (i >= 3 && buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' &&
            buf[i - 3] == '\r') {
            return i + 1;
        }
    }

    return 0;
}

static bool method_is(const char *method, k_size_t len, const char *word)
{
    k_size_t word_len = strlen(word);

    return len == word_len && memcmp(method, word, len) == 0;
}

/* `bytes=first-last`, `bytes=first-` or `bytes=-suffix`. One range only: a
 * multi-range answer is a body of several parts and nothing here needs it.
 * A range this parser does not speak is dropped: the whole object is a
 * correct answer to a request for parts of it. */
static void range_parse(const char *value, k_size_t len, struct httpd_request *request)
{
    k_size_t i = 0;
    k_u64_t first = 0;
    k_u64_t last = 0;
    bool has_first = false;

    while (i < len && (value[i] == ' ' || value[i] == '\t')) {
        i++;
    }

    if (len - i < 6 || memcmp(value + i, "bytes=", 6) != 0) {
        return;
    }

    i += 6;

    while (i < len && value[i] >= '0' && value[i] <= '9') {
        if (first > (HTTPD_SIZE_UNKNOWN - 9u) / 10u) {
            return;
        }
        first = first * 10u + (k_u64_t)(value[i] - '0');
        has_first = true;
        i++;
    }

    if (i >= len || value[i] != '-') {
        return;
    }

    i++;

    if (i < len && value[i] == ',') {
        return;
    }

    if (i == len) {
        if (!has_first) {
            return;
        }

        request->has_range = true;
        request->range_suffix = false;
        request->range_first = first;
        request->range_last = HTTPD_SIZE_UNKNOWN;
        return;
    }

    while (i < len && value[i] >= '0' && value[i] <= '9') {
        if (last > (HTTPD_SIZE_UNKNOWN - 9u) / 10u) {
            return;
        }
        last = last * 10u + (k_u64_t)(value[i] - '0');
        i++;
    }

    while (i < len && (value[i] == ' ' || value[i] == '\t')) {
        i++;
    }

    if (i != len) {
        return;
    }

    request->has_range = true;

    if (!has_first) {
        request->range_suffix = true;
        request->range_first = last;
        request->range_last = HTTPD_SIZE_UNKNOWN;
        return;
    }

    if (last < first) {
        request->has_range = false;
        return;
    }

    request->range_suffix = false;
    request->range_first = first;
    request->range_last = last;
}

/* One value of an If-None-Match, or a list of them: the answer is 304 when
 * the tag of the file is one of them. The weak mark of a tag is ignored,
 * which is what a server may do with a comparison of a cache. */
static bool etag_matches(const char *value, k_size_t len, const char *etag)
{
    k_size_t etag_len = strlen(etag);
    k_size_t i = 0;

    while (i < len) {
        k_size_t start;
        k_size_t stop;

        while (i < len && (value[i] == ' ' || value[i] == ',')) {
            i++;
        }

        start = i;
        while (i < len && value[i] != ',') {
            i++;
        }
        stop = i;

        while (stop > start && value[stop - 1] == ' ') {
            stop--;
        }

        if (stop - start >= 2 && value[start] == 'W' && value[start + 1] == '/') {
            start += 2;
        }

        if (stop - start == etag_len && memcmp(value + start, etag, etag_len) == 0) {
            return true;
        }
    }

    return false;
}

/* The head of the request: `method SP target SP version`, then the headers
 * we use. The headers we do not know are read and dropped. */
static int request_parse(char *request, k_size_t used, struct httpd_request *out)
{
    k_size_t end = head_end(request, used);
    k_size_t pos;
    const char *p;
    int target_rc;

    memset(out, 0, sizeof(*out));

    if (end == 0) {
        return 400;
    }

    out->head_len = end;
    out->keep_alive = true;

    /* The method. */
    p = request;
    while (*p != '\0' && *p != ' ' && *p != '\r' && *p != '\n') {
        p++;
    }

    if (*p != ' ') {
        return 400;
    }

    out->method = request;
    out->method_len = (k_size_t)(p - request);

    /* The target. */
    p++;
    out->target = p;
    while (*p != '\0' && *p != ' ' && *p != '\r' && *p != '\n') {
        p++;
    }

    out->target_len = (k_size_t)(p - out->target);

    if (out->target_len > HTTPD_PATH_MAX) {
        return 414;
    }

    target_rc = target_normalize(out->target, out->target_len, out->path, &out->dir_slash);
    if (target_rc == TARGET_ESCAPE) {
        return 403;
    }
    if (target_rc != TARGET_OK) {
        return 400;
    }

    /* The version: HTTP/1.0 closes unless the client asks to keep, and 1.1
     * keeps unless the client asks to close. Anything else is read as 1.0. */
    while (*p == ' ') {
        p++;
    }

    if (used - (k_size_t)(p - request) >= 8 && memcmp(p, "HTTP/1.1", 8) == 0) {
        out->keep_alive = true;
    } else {
        out->keep_alive = false;
    }

    /* The headers, one line at a time, until the empty line. */
    pos = 0;
    while (pos < end && request[pos] != '\n') {
        pos++;
    }

    while (pos < end) {
        k_size_t start;
        k_size_t stop;
        const char *value;
        k_size_t value_len;

        pos++;
        start = pos;
        while (pos < end && request[pos] != '\n') {
            pos++;
        }
        stop = pos;

        while (stop > start && (request[stop - 1] == '\r' || request[stop - 1] == ' ')) {
            stop--;
        }

        if (stop == start) {
            break;
        }

        if (header_named(request + start, stop - start, "Connection", &value, &value_len)) {
            if (value_is(value, value_len, "close")) {
                out->keep_alive = false;
            } else if (value_is(value, value_len, "keep-alive")) {
                out->keep_alive = true;
            }
        } else if (header_named(request + start, stop - start, "Range", &value, &value_len)) {
            range_parse(value, value_len, out);
        } else if (header_named(request + start, stop - start, "If-None-Match", &value,
                                &value_len)) {
            if (value_len < sizeof(out->etag)) {
                memcpy(out->etag, value, value_len);
                out->etag[value_len] = '\0';
                out->has_etag = true;
            }
        } else if (header_named(request + start, stop - start, "Content-Length", &value,
                                &value_len)) {
            /* This server reads no body: a request that promises one ends the
             * connection after its answer. */
            if (!(value_len == 1 && value[0] == '0')) {
                out->has_body = true;
                out->keep_alive = false;
            }
        }
    }

    return 0;
}

/* --- the log --- */

static void log_line(const struct httpd_request *request, int status, k_u64_t bytes)
{
    char line[HTTPD_LOG_MAX];
    k_size_t len = 0;
    k_size_t method_len = request->method_len > 8 ? 8 : request->method_len;
    k_size_t target_len = request->target_len;

    line[0] = '\0';

    if (target_len > 64) {
        target_len = 64;
    }

    if (method_len > 0) {
        memcpy(line, request->method, method_len);
        len = method_len;
        line[len] = '\0';
    }

    text_append(line, sizeof(line), &len, " ");
    if (request->target != NULL) {
        char target[65];

        memcpy(target, request->target, target_len);
        target[target_len] = '\0';
        text_append(line, sizeof(line), &len, target);
    }
    text_append(line, sizeof(line), &len, " ");
    text_append_u64(line, sizeof(line), &len, (k_u64_t)status);
    text_append(line, sizeof(line), &len, " ");
    text_append_u64(line, sizeof(line), &len, bytes);
    text_append(line, sizeof(line), &len, "\n");

    if (s_log[0] == '\0') {
        kinfo("httpd: %s", line);
        return;
    }

    {
        k_u8_t priv[VFS_FILE_DATA_SIZE];
        struct vfs_file file;

        if (vfs_open(s_log, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &file, priv) != 0) {
            /* The volume refused the line: the console is the log of the
             * last resort, and a person still sees what happened. */
            kinfo("httpd: %s", line);
            return;
        }

        (void)vfs_write(&file, line, len);
        (void)vfs_close(&file);
    }
}

/* --- the answer --- */

struct httpd_answer {
    int status;
    const char *content_type;
    const char *text;      /* the body of a short answer */
    struct vfs_file *file; /* the body of a file, when it is not NULL */
    k_u64_t file_offset;
    k_u64_t file_length;
    k_u64_t total_length; /* the size of the whole object, for a Range */
    bool has_etag;
    char etag[48];
    const char *location; /* the 301 of a directory without its slash */
    bool has_range;       /* the 206 and the 416 carry a Content-Range */
    bool chunked;         /* the body has no length yet */
};

static const char *status_reason(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 414:
        return "URI Too Long";
    case 416:
        return "Range Not Satisfiable";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

/*
 * The head of one answer. The length is written for everything but a 304
 * (which has no body) and a chunked answer (which announces chunks instead).
 */
static bool answer_head(struct httpd_stream *stream, const struct httpd_answer *answer,
                        bool keep_alive)
{
    char head[HTTPD_HEAD_MAX];
    k_size_t len = 0;
    k_u64_t body = 0;

    head[0] = '\0';

    text_append(head, sizeof(head), &len, "HTTP/1.1 ");
    text_append_u64(head, sizeof(head), &len, (k_u64_t)answer->status);
    text_append(head, sizeof(head), &len, " ");
    text_append(head, sizeof(head), &len, status_reason(answer->status));
    text_append(head, sizeof(head), &len, "\r\n");
    text_append(head, sizeof(head), &len, "Server: microos-httpd\r\n");

    if (answer->status != 304) {
        text_append(head, sizeof(head), &len, "Content-Type: ");
        text_append(head, sizeof(head), &len,
                    answer->content_type != NULL ? answer->content_type
                                                 : "text/plain; charset=utf-8");
        text_append(head, sizeof(head), &len, "\r\n");
    }

    if (answer->file != NULL) {
        body = answer->file_length;
    } else if (answer->text != NULL) {
        body = (k_u64_t)strlen(answer->text);
    }

    if (answer->has_range) {
        text_append(head, sizeof(head), &len, "Content-Range: bytes ");
        if (answer->status == 416) {
            text_append(head, sizeof(head), &len, "*/");
            text_append_u64(head, sizeof(head), &len, answer->total_length);
        } else {
            text_append_u64(head, sizeof(head), &len, answer->file_offset);
            text_append(head, sizeof(head), &len, "-");
            text_append_u64(head, sizeof(head), &len,
                            answer->file_offset + answer->file_length - 1u);
            text_append(head, sizeof(head), &len, "/");
            text_append_u64(head, sizeof(head), &len, answer->total_length);
        }
        text_append(head, sizeof(head), &len, "\r\n");
    }

    if (answer->has_etag) {
        text_append(head, sizeof(head), &len, "ETag: ");
        text_append(head, sizeof(head), &len, answer->etag);
        text_append(head, sizeof(head), &len, "\r\n");
    }

    if (answer->location != NULL) {
        text_append(head, sizeof(head), &len, "Location: ");
        text_append(head, sizeof(head), &len, answer->location);
        text_append(head, sizeof(head), &len, "\r\n");
    }

    if (answer->status == 405) {
        text_append(head, sizeof(head), &len, "Allow: GET, HEAD\r\n");
    }

    if (answer->chunked) {
        text_append(head, sizeof(head), &len, "Transfer-Encoding: chunked\r\n");
    } else if (answer->status != 304) {
        text_append(head, sizeof(head), &len, "Content-Length: ");
        text_append_u64(head, sizeof(head), &len, body);
        text_append(head, sizeof(head), &len, "\r\n");
    }

    if (answer->status != 304 && !answer->chunked) {
        text_append(head, sizeof(head), &len, "Accept-Ranges: bytes\r\n");
    }

    text_append(head, sizeof(head), &len,
                keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    text_append(head, sizeof(head), &len, "\r\n");

    return stream_send_all(stream, head, len);
}

/* The bytes of a file from its offset: one block of the stack at a time. */
static bool answer_file_body(struct httpd_stream *stream, struct vfs_file *file, k_u64_t offset,
                             k_u64_t length)
{
    char block[HTTPD_BUFFER];
    k_u64_t left = length;

    file->offset = offset;

    while (left > 0) {
        k_size_t want = left > sizeof(block) ? sizeof(block) : (k_size_t)left;
        int n = vfs_read(file, block, want);

        if (n <= 0) {
            /* A file that ended before its length: the answer is short and
             * the client sees a broken one, which is the truth of the file. */
            return false;
        }

        if (!stream_send_all(stream, block, (k_size_t)n)) {
            return false;
        }

        left -= (k_u64_t)n;
    }

    return true;
}

static bool send_chunk(struct httpd_stream *stream, const char *data, k_size_t size)
{
    char head[24];
    k_size_t len = 0;

    if (size == 0) {
        return true;
    }

    head[0] = '\0';
    text_append_hex(head, sizeof(head), &len, (k_u64_t)size);
    text_append(head, sizeof(head), &len, "\r\n");

    return stream_send_all(stream, head, len) && stream_send_all(stream, data, size) &&
           stream_send_all(stream, "\r\n", 2);
}

static bool send_chunk_end(struct httpd_stream *stream)
{
    return stream_send_all(stream, "0\r\n\r\n", 5);
}

/* One answer, the head and the body, for the short ones. */
static int answer_send(struct httpd_stream *stream, struct httpd_answer *answer, bool keep_alive,
                       bool body_wanted)
{
    if (!answer_head(stream, answer, keep_alive)) {
        return -1;
    }

    if (body_wanted && answer->status != 304 && answer->text != NULL) {
        if (!stream_send_all(stream, answer->text, strlen(answer->text))) {
            return -1;
        }
    }

    return 0;
}

static int answer_text(struct httpd_stream *stream, int status, const char *text, bool keep_alive,
                       bool body_wanted)
{
    struct httpd_answer answer;

    memset(&answer, 0, sizeof(answer));
    answer.status = status;
    answer.text = text;

    return answer_send(stream, &answer, keep_alive, body_wanted);
}

/* --- the content types --- */

struct httpd_type {
    const char *extension;
    const char *type;
};

static const struct httpd_type s_types[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"txt", "text/plain; charset=utf-8"},
    {"log", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"xml", "application/xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"pdf", "application/pdf"},
    {"gz", "application/gzip"},
    {"zst", "application/zstd"},
    {"zip", "application/zip"},
    {"mox", "application/octet-stream"},
    {"bin", "application/octet-stream"},
};

static const char *content_type_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    k_size_t i;

    if (dot == NULL || (slash != NULL && dot < slash)) {
        return "application/octet-stream";
    }

    dot++;

    for (i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++) {
        if (text_equal_nocase(dot, s_types[i].extension, strlen(s_types[i].extension))) {
            return s_types[i].type;
        }
    }

    return "application/octet-stream";
}

/* --- the listing --- */

static void html_escape(char *out, k_size_t size, const char *text)
{
    k_size_t len = 0;

    out[0] = '\0';

    while (*text != '\0' && len + 8 < size) {
        char one[2] = {*text, '\0'};

        switch (*text) {
        case '&':
            text_append(out, size, &len, "&amp;");
            break;
        case '<':
            text_append(out, size, &len, "&lt;");
            break;
        case '>':
            text_append(out, size, &len, "&gt;");
            break;
        case '"':
            text_append(out, size, &len, "&quot;");
            break;
        default:
            text_append(out, size, &len, one);
            break;
        }

        text++;
    }
}

/* The name of an entry as the target of a link: a space and the characters
 * that mean something in a URL become their %XX form. */
static void url_escape(char *out, k_size_t size, const char *text)
{
    static const char digits[] = "0123456789ABCDEF";
    k_size_t len = 0;

    out[0] = '\0';

    while (*text != '\0' && len + 4 < size) {
        unsigned char byte = (unsigned char)*text;

        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~' || byte == '/') {
            char one[2] = {(char)byte, '\0'};

            text_append(out, size, &len, one);
        } else {
            char code[4];

            code[0] = '%';
            code[1] = digits[(byte >> 4) & 0x0Fu];
            code[2] = digits[byte & 0x0Fu];
            code[3] = '\0';
            text_append(out, size, &len, code);
        }

        text++;
    }
}

/*
 * A directory is a page of links and not the bytes of a file. It is built as
 * it is sent: the number of the entries of a volume is not known before the
 * last one, and a server that held the whole listing in memory would need a
 * buffer of the size of the directory.
 */
static int serve_listing(struct httpd_stream *stream, const struct httpd_request *request,
                         const char *path, void *priv, bool keep_alive, bool body_wanted)
{
    struct httpd_answer answer;
    struct vfs_file directory;
    struct vfs_dirent entry;
    char line[HTTPD_LIST_MAX];
    char href[VFS_NAME_MAX * 3 + 8];
    char name[VFS_NAME_MAX * 6 + 8];
    k_u64_t entries = 0;
    k_size_t count = 0;
    k_size_t len;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, &directory, priv);
    if (rc != 0) {
        return answer_text(stream, 404, "not found\n", keep_alive, body_wanted);
    }

    memset(&answer, 0, sizeof(answer));
    answer.status = 200;
    answer.content_type = "text/html; charset=utf-8";
    answer.chunked = true;

    if (!answer_head(stream, &answer, keep_alive)) {
        (void)vfs_close(&directory);
        return -1;
    }

    if (body_wanted) {
        len = 0;
        line[0] = '\0';
        text_append(line, sizeof(line), &len,
                    "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>");
        html_escape(name, sizeof(name), request->path);
        text_append(line, sizeof(line), &len, name);
        text_append(line, sizeof(line), &len, "</title></head>\n<body><h1>");
        text_append(line, sizeof(line), &len, name);
        text_append(line, sizeof(line), &len, "</h1>\n<p>the directory of ");
        text_append(line, sizeof(line), &len, s_root);
        text_append(line, sizeof(line), &len, "</p>\n<ul>\n");

        if (!send_chunk(stream, line, len)) {
            (void)vfs_close(&directory);
            return -1;
        }

        while (vfs_readdir(&directory, &entry, 1, &count) == 0 && count > 0) {
            if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
                continue;
            }

            len = 0;
            line[0] = '\0';

            url_escape(href, sizeof(href), request->path);
            text_append(line, sizeof(line), &len, "<li><a href=\"");
            text_append(line, sizeof(line), &len, href);
            if (href[0] != '\0' && href[strlen(href) - 1] != '/') {
                text_append(line, sizeof(line), &len, "/");
            }
            url_escape(name, sizeof(name), entry.name);
            text_append(line, sizeof(line), &len, name);
            text_append(line, sizeof(line), &len, "\">");

            html_escape(name, sizeof(name), entry.name);
            text_append(line, sizeof(line), &len, name);

            if (entry.type == 2) {
                text_append(line, sizeof(line), &len, "/");
            }

            text_append(line, sizeof(line), &len, "</a>");

            if (entry.type != 2) {
                text_append(line, sizeof(line), &len, " (");
                text_append_u64(line, sizeof(line), &len, entry.size);
                text_append(line, sizeof(line), &len, " byte(s))");
            }

            text_append(line, sizeof(line), &len, "</li>\n");

            if (!send_chunk(stream, line, len)) {
                (void)vfs_close(&directory);
                return -1;
            }

            entries++;
        }

        len = 0;
        line[0] = '\0';
        text_append(line, sizeof(line), &len, "</ul>\n<p>");
        text_append_u64(line, sizeof(line), &len, entries);
        text_append(line, sizeof(line), &len, entries == 1 ? " entry.</p>\n" : " entries.</p>\n");
        text_append(line, sizeof(line), &len, "</body></html>\n");

        if (!send_chunk(stream, line, len) || !send_chunk_end(stream)) {
            (void)vfs_close(&directory);
            return -1;
        }
    }

    (void)vfs_close(&directory);

    log_line(request, 200, entries);

    return 0;
}

/* --- the file --- */

static int serve_file(struct httpd_stream *stream, const struct httpd_request *request,
                      const char *path, const struct vfs_statx *info, void *priv, bool keep_alive,
                      bool body_wanted)
{
    struct httpd_answer answer;
    struct vfs_file file;
    char etag[48];
    k_size_t len = 0;
    k_u64_t first = 0;
    k_u64_t last = info->size == 0 ? 0 : info->size - 1u;
    int rc;

    memset(&answer, 0, sizeof(answer));
    answer.status = 200;
    answer.content_type = content_type_of(path);
    answer.file = &file;
    answer.total_length = info->size;

    /* The tag of the object: the size and the moment of its last change,
     * which is what a client needs to know that it has the same file. */
    etag[0] = '\0';
    text_append(etag, sizeof(etag), &len, "\"");
    text_append_hex(etag, sizeof(etag), &len, info->size);
    text_append(etag, sizeof(etag), &len, "-");
    text_append_hex(etag, sizeof(etag), &len, info->mtime);
    text_append(etag, sizeof(etag), &len, "\"");
    answer.has_etag = true;
    memcpy(answer.etag, etag, sizeof(etag));

    if (request->has_etag && etag_matches(request->etag, strlen(request->etag), etag)) {
        answer.status = 304;
        answer.content_type = NULL;
        answer.file = NULL;
        rc = answer_send(stream, &answer, keep_alive, false);
        log_line(request, 304, 0);
        return rc;
    }

    if (request->has_range) {
        if (request->range_suffix) {
            k_u64_t suffix = request->range_first;

            if (suffix == 0 || info->size == 0) {
                memset(&answer, 0, sizeof(answer));
                answer.status = 416;
                answer.has_range = true;
                answer.total_length = info->size;
                rc = answer_send(stream, &answer, keep_alive, body_wanted);
                log_line(request, 416, 0);
                return rc;
            }

            first = suffix >= info->size ? 0 : info->size - suffix;
            last = info->size - 1u;
        } else {
            first = request->range_first;
            if (first >= info->size) {
                /* The answer keeps the tag of the object: the client that
                 * asked for a byte past the end learns how long it is. */
                struct httpd_answer refused;

                memset(&refused, 0, sizeof(refused));
                refused.status = 416;
                refused.has_range = true;
                refused.total_length = info->size;
                refused.has_etag = true;
                memcpy(refused.etag, etag, sizeof(etag));

                rc = answer_send(stream, &refused, keep_alive, body_wanted);
                log_line(request, 416, 0);
                return rc;
            }

            last = request->range_last;
            if (last == HTTPD_SIZE_UNKNOWN || last >= info->size) {
                last = info->size - 1u;
            }
        }

        answer.status = 206;
        answer.has_range = true;
        answer.file_offset = first;
        answer.file_length = last - first + 1u;
    } else {
        answer.file_offset = 0;
        answer.file_length = info->size;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return answer_text(stream, 404, "not found\n", keep_alive, body_wanted);
    }

    if (!answer_head(stream, &answer, keep_alive)) {
        (void)vfs_close(&file);
        return -1;
    }

    if (body_wanted) {
        if (!answer_file_body(stream, &file, answer.file_offset, answer.file_length)) {
            (void)vfs_close(&file);
            return -1;
        }
    }

    (void)vfs_close(&file);

    log_line(request, answer.status, body_wanted ? answer.file_length : 0);

    return 0;
}

/* --- one request --- */

/*
 * Answer one request. The target is the path under the root; a file, a
 * listing of a directory and a 301 of a directory without its last slash are
 * the three shapes a found object has.
 */
static int serve_request(struct httpd_stream *stream, struct httpd_request *request, void *priv)
{
    char path[HTTPD_PATH_MAX];
    struct vfs_statx info;
    bool keep_alive = request->keep_alive;
    bool body_wanted = method_is(request->method, request->method_len, "GET");
    k_size_t path_len = strlen(request->path);
    k_size_t root_len = strlen(s_root);
    int rc;

    counter_add(&s_requests);

    if (!body_wanted && !method_is(request->method, request->method_len, "HEAD")) {
        rc = answer_text(stream, 405, "method not allowed\n", keep_alive, true);
        log_line(request, 405, 0);
        return rc;
    }

    /* The root followed by the path: "/" is the root itself, and any other
     * path is a name below it. */
    if (root_len + path_len >= sizeof(path)) {
        rc = answer_text(stream, 414, "uri too long\n", keep_alive, true);
        log_line(request, 414, 0);
        return rc;
    }

    memcpy(path, s_root, root_len);
    path[root_len] = '\0';

    if (path_len > 1) {
        if (root_len > 0 && s_root[root_len - 1] == '/') {
            memcpy(path + root_len - 1, request->path, path_len + 1);
        } else {
            memcpy(path + root_len, request->path, path_len + 1);
        }
    }

    if (vfs_statx(path, &info) != 0) {
        rc = answer_text(stream, 404, "not found\n", keep_alive, true);
        log_line(request, 404, 0);
        return rc;
    }

    if (info.type == 2) {
        /* A directory is a listing, and it is reached through the slash that
         * says it is one: without it the client is sent to it. */
        if (!request->dir_slash) {
            struct httpd_answer answer;
            char location[HTTPD_PATH_MAX + 8];
            k_size_t len = 0;

            location[0] = '\0';
            text_append(location, sizeof(location), &len, request->path);
            text_append(location, sizeof(location), &len, "/");

            memset(&answer, 0, sizeof(answer));
            answer.status = 301;
            answer.location = location;
            answer.text = "moved permanently\n";

            rc = answer_send(stream, &answer, keep_alive, true);
            log_line(request, 301, 0);
            return rc;
        }

        return serve_listing(stream, request, path, priv, keep_alive, body_wanted);
    }

    if (info.type != 1) {
        rc = answer_text(stream, 403, "not a file\n", keep_alive, true);
        log_line(request, 403, 0);
        return rc;
    }

    return serve_file(stream, request, path, &info, priv, keep_alive, body_wanted);
}

/* --- the session --- */

struct httpd_session {
    struct httpd_stream stream;
    k_size_t used; /* the bytes of the buffer that are a request not yet read */
    /* The private data of the descriptor the session has open: the VFS hands
     * this buffer to the filesystem, and MicroFS keeps the cursor of its file
     * in it (components/mfs/src/mfs_vfs.c). */
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char request[HTTPD_REQUEST_MAX + 1];
};

/*
 * The request in the buffer: read until the empty line that ends the head,
 * with the clock of the machine as the only bound of a client that says
 * nothing. A session that closes before its first byte ends quietly.
 */
static int session_fill(struct httpd_session *session, k_u32_t timeout_ms)
{
    k_u64_t deadline = k_sysinfo_uptime_ms() + (k_u64_t)timeout_ms;

    while (session->used < HTTPD_REQUEST_MAX) {
        if (head_end(session->request, session->used) != 0) {
            return 1;
        }

        {
            int n = stream_recv(&session->stream, session->request + session->used,
                                HTTPD_REQUEST_MAX - session->used);

            if (n > 0) {
                session->used += (k_size_t)n;
                session->request[session->used] = '\0';
                continue;
            }

            if (n == 0) {
                return session->used == 0 ? 0 : -1;
            }
        }

        if (k_sysinfo_uptime_ms() >= deadline) {
            return session->used == 0 ? 0 : -1;
        }

        k_sched_sleep_ms(HTTPD_POLL_MS);
    }

    return -1;
}

/*
 * One session: the requests of one connection, until it ends. The bytes of
 * the next request that arrived with the answer of the last one are kept and
 * not read again - a client may pipeline, and the buffer is the queue.
 */
static void session_serve(struct httpd_session *session)
{
    k_u32_t served = 0;

    while (server_enabled() && served < HTTPD_KEEPALIVE_REQUESTS) {
        struct httpd_request request;
        k_size_t leftover;
        int rc;

        session->request[session->used] = '\0';

        rc = session_fill(session, served == 0 ? HTTPD_REQUEST_TIMEOUT_MS : s_idle_timeout_ms);
        if (rc <= 0) {
            /* A client that said nothing at all and one that stopped in the
             * middle of a head are both the end of the session, and there is
             * nobody to answer a half request. */
            return;
        }

        rc = request_parse(session->request, session->used, &request);
        if (rc != 0) {
            struct httpd_request refused;

            memset(&refused, 0, sizeof(refused));
            refused.method = "?";
            refused.method_len = 1;
            refused.target = "(refused)";
            refused.target_len = 10;

            if (rc == 414) {
                (void)answer_text(&session->stream, 414, "uri too long\n", false, true);
                log_line(&refused, 414, 0);
            } else if (rc == 403) {
                (void)answer_text(&session->stream, 403, "forbidden\n", false, true);
                log_line(&refused, 403, 0);
            } else {
                (void)answer_text(&session->stream, 400, "bad request\n", false, true);
                log_line(&refused, 400, 0);
            }

            return;
        }

        if (serve_request(&session->stream, &request, session->priv) != 0) {
            return;
        }

        served++;

        /* The requests that came in the same block as this one stay in the
         * buffer; the rest of the block is the next read. */
        leftover = session->used > request.head_len ? session->used - request.head_len : 0;

        if (leftover > 0) {
            memmove(session->request, session->request + request.head_len, leftover);
        }

        session->used = leftover;
        session->request[session->used] = '\0';

        if (!request.keep_alive || request.has_body) {
            return;
        }
    }
}

static void httpd_session_task(void *arg)
{
    struct httpd_session *session = arg;
    bool handshake = true;

    if (s_tls) {
        handshake = net_tls_server_open(session->stream.socket, &session->stream.tls) == 0;
    }

    if (handshake) {
        session_serve(session);
    }

    if (session->stream.tls != NULL) {
        net_tls_server_close(session->stream.tls);
    }

    k_socket_close(session->stream.socket);

    counter_sub(&s_active);

    kmm_free(session);
}

/* The refusal of a session past the ceiling: it is read by a client and it
 * costs no task of the table. */
static void refuse_session(k_socket_t *client)
{
    static const char answer[] = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Server: microos-httpd\r\n"
                                 "Content-Type: text/plain; charset=utf-8\r\n"
                                 "Content-Length: 12\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "busy, later\n";

    /* The counter first: a client that reads the refusal and asks the server
     * at once must see the refusal counted. */
    counter_add(&s_refused);
    (void)k_socket_send(client, answer, sizeof(answer) - 1, false);
    k_socket_close(client);
}

/* --- the server --- */

static void httpd_accept_task(void *arg)
{
    k_server_t *server = arg;

    while (server_enabled()) {
        k_socket_t *client = k_server_accept(server, HTTPD_ACCEPT_TIMEOUT_MS);
        struct httpd_session *session;

        /* The wait of the listener is the moment the sessions that ended
         * are reaped: their slots are the table of the tasks, and a server
         * of many sessions would fill it. */
        sessions_reap();

        if (client == NULL) {
            continue;
        }

        if (counter_active() >= s_max_connections) {
            refuse_session(client);
            continue;
        }

        /*
         * The list of the sessions is also the ceiling of the door: a session
         * whose pid is not kept is a slot of the task table that nobody gives
         * back, so a client past the list is refused here.
         */
        if (s_session_count >= K_ARRAY_SIZE(s_session_pids)) {
            refuse_session(client);
            continue;
        }

        session = kmm_alloc(sizeof(*session), KMM_PRIORITY_USER);
        if (session == NULL) {
            refuse_session(client);
            continue;
        }

        memset(session, 0, sizeof(*session));
        session->stream.socket = client;

        /*
         * The session is counted before the task that serves it exists. A
         * client may close in the instant between the two, and the task then
         * runs and gives the count back; an add that came after the create
         * could be lost behind a task that had already finished (the count
         * would wrap below zero and the door would read a number of sessions
         * that never existed), or a session that is already serving would be
         * counted late (and one more let over the ceiling). The count is the
         * reservation, and the task is the work.
         */
        counter_add(&s_active);

        {
            struct task *task =
                task_create("httpd", httpd_session_task, session, TASK_PRIO_DEFAULT);

            if (task == NULL) {
                counter_sub(&s_active);
                kmm_free(session);
                refuse_session(client);
                continue;
            }

            s_session_pids[s_session_count++] = task->pid;
        }
    }

    k_server_close(server);
    server_set_serving(false);
}

int httpd_start(const struct httpd_config *config)
{
    struct vfs_stat root_info;
    struct task *task;
    const char *root;

    if (server_serving()) {
        return server_enabled() ? HTTPD_OK : HTTPD_EBUSY;
    }

    if (config == NULL || config->port == 0) {
        return HTTPD_EARGS;
    }

    root = config->root != NULL ? config->root : HTTPD_ROOT_DEFAULT;

    if (strlen(root) >= sizeof(s_root)) {
        return HTTPD_EARGS;
    }

    if (vfs_stat(root, &root_info) != 0 || root_info.type != 2) {
        kwarn("httpd: the root %s is not a directory of the volume", root);
        return HTTPD_EROOT;
    }

    s_max_connections = config->connections;
    if (s_max_connections == 0) {
        s_max_connections = HTTPD_CONNECTIONS;
    }
    if (s_max_connections > HTTPD_CONNECTIONS_MAX) {
        s_max_connections = HTTPD_CONNECTIONS_MAX;
    }

    s_idle_timeout_ms =
        config->idle_timeout_ms != 0 ? config->idle_timeout_ms : HTTPD_IDLE_TIMEOUT_MS;

    memcpy(s_root, root, strlen(root) + 1);
    s_log[0] = '\0';

    if (config->log != NULL && config->log[0] != '\0') {
        if (strlen(config->log) >= sizeof(s_log)) {
            return HTTPD_EARGS;
        }

        memcpy(s_log, config->log, strlen(config->log) + 1);
    }

    s_tls = config->tls && net_tls_server_supported();

    if (config->tls && !s_tls) {
        kwarn("httpd: this image has no serving side of TLS, the door is plain HTTP");
    }

    s_requests = 0;
    s_refused = 0;
    s_active = 0;
    s_session_count = 0;

    s_server = k_server_listen(config->port);
    if (s_server == NULL) {
        kwarn("httpd: the port %u is not ours", (unsigned)config->port);
        return HTTPD_EPORT;
    }

    s_port = config->port;
    server_set_enabled(true);
    server_set_serving(true);

    task = task_create("httpd", httpd_accept_task, s_server, TASK_PRIO_DEFAULT);
    if (task == NULL) {
        server_set_enabled(false);
        server_set_serving(false);
        k_server_close(s_server);
        s_server = NULL;
        return HTTPD_ETASK;
    }

    kinfo("httpd: %s on port %u, at most %u session(s)%s", s_root, (unsigned)s_port,
          (unsigned)s_max_connections, s_tls ? ", TLS" : "");

    return HTTPD_OK;
}

void httpd_stop(void)
{
    k_u32_t waited = 0;

    if (!server_serving()) {
        return;
    }

    server_set_enabled(false);

    while (server_serving() && waited < HTTPD_STOP_WAIT_MS) {
        k_sched_sleep_ms(HTTPD_STOP_STEP_MS);
        waited += HTTPD_STOP_STEP_MS;
    }

    /* The sessions that are open end by their own timeouts; the ones that are
     * still there after the wait are left to finish and reported. */
    while (counter_active() > 0 && waited < 2u * HTTPD_STOP_WAIT_MS) {
        k_sched_sleep_ms(HTTPD_STOP_STEP_MS);
        waited += HTTPD_STOP_STEP_MS;
    }

    if (server_serving()) {
        kwarn("httpd: the door did not close in %u ms", (unsigned)HTTPD_STOP_WAIT_MS);
    }
}

bool httpd_running(void)
{
    return server_serving() && server_enabled();
}

k_u16_t httpd_port(void)
{
    if (!httpd_running()) {
        return 0;
    }

    return __atomic_load_n(&s_port, __ATOMIC_SEQ_CST);
}

const char *httpd_root(void)
{
    return httpd_running() ? s_root : NULL;
}

k_u32_t httpd_requests(void)
{
    return __atomic_load_n(&s_requests, __ATOMIC_SEQ_CST);
}

k_u32_t httpd_refused(void)
{
    return __atomic_load_n(&s_refused, __ATOMIC_SEQ_CST);
}

k_u32_t httpd_active(void)
{
    return counter_active();
}
