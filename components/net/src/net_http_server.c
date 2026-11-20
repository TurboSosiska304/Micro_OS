/*
 * Micro OS — the page of the state over HTTP.
 *
 * One task, one port, one request at a time: the listener accepts a session,
 * the request is read into a buffer of a fixed size, one answer goes out and
 * the session is closed. A board with a few kilobytes of free RAM has no room
 * for keep-alive and no reason for it: a page of the state is asked for once
 * in a while, by a person or by a monitor.
 *
 * Every wait is short. The accept waits half a second so that the task
 * notices `net_http_server_stop()`; the request has two seconds to arrive in
 * full; a write waits for the port's own timeout. Nothing here allocates: the
 * buffers are static, because the task is one and the server is one.
 *
 * The sockets come from the kernel (kernel/net.h), not from the port: the
 * same server runs over lwIP on the board and over the sockets of the host
 * under the tests.
 */
#include "net/net_http.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/task.h"
#include "kernel/version.h"

/* The wait for the next client, short enough to notice the stop. */
#define HTTP_SERVER_ACCEPT_TIMEOUT_MS 500

/*
 * The time a request has to arrive in full. A client of a home network is
 * either there at once or gone; the caller of the command has to wait at
 * most this long for an answer that will never come.
 */
#define HTTP_SERVER_READ_TIMEOUT_MS 2000

/* The longest request line and headers we read: the rest is a refusal. */
#define HTTP_SERVER_REQUEST_MAX 512

/* The page of the state fits many times into this, and one buffer does. */
#define HTTP_SERVER_BODY_MAX 512

/* A line of the answer: the status, a few headers, and the empty line. */
#define HTTP_SERVER_HEAD_MAX 224

/* A read that failed answers at once; the sleep keeps it from spinning. */
#define HTTP_SERVER_POLL_MS 20

#define HTTP_SERVER_STOP_WAIT_MS 4000
#define HTTP_SERVER_STOP_STEP_MS 50

/* What the reader of the request says. */
#define HTTP_REQUEST_OK 0
#define HTTP_REQUEST_LONG (-1)
#define HTTP_REQUEST_TIMEOUT (-2)
#define HTTP_REQUEST_CLOSED (-3)

/*
 * The two flags of the server are the only state the task of the server and
 * the task that starts and stops it share. They are read and written with the
 * atomics of the kernel (see the note at the helpers below), not as plain
 * bools: a flag of one task read by another is a race the detector of the
 * host sees, and the board would be looking at a value the compiler cached.
 */
static bool s_enabled;
static bool s_serving;

/* The port the running server took. It is written before the two flags above
 * and read by the caller of `net_http_server_port`; an atomic read and write
 * keep a second task from seeing half of it. */
static k_u16_t s_port;

/* --- the text of the answer --- */

/*
 * Append a line to a buffer that is bounded by its capacity. A truncated
 * answer is still a well-formed one: the length of the body is counted after
 * the copy, so the client reads what is there and not what was promised.
 */
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

/* A number in decimal: the answer is written without printf, on purpose. */
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

/*
 * The page of the state: who we are, how long the board has been up, what is
 * left of the memory and how many tasks use it. The numbers come from the
 * kernel (kernel/sysinfo.h, kernel/task.h), never from the chip.
 */
static k_size_t page_of_state(char *buf, k_size_t size)
{
    k_size_t len = 0;

    buf[0] = '\0';

    text_append(buf, size, &len, kernel_name());
    text_append(buf, size, &len, " ");
    text_append(buf, size, &len, kernel_version());
    text_append(buf, size, &len, " (");
    text_append(buf, size, &len, k_sysinfo_arch());
    text_append(buf, size, &len, ")\n");

    text_append(buf, size, &len, "uptime: ");
    text_append_u64(buf, size, &len, k_sysinfo_uptime_ms() / 1000u);
    text_append(buf, size, &len, " s\n");

    text_append(buf, size, &len, "memory: ");
    text_append_u64(buf, size, &len, k_sysinfo_heap_free() / 1024u);
    text_append(buf, size, &len, " KB free of ");
    text_append_u64(buf, size, &len, k_sysinfo_heap_total() / 1024u);
    text_append(buf, size, &len, " KB\n");

    text_append(buf, size, &len, "psram: ");
    text_append_u64(buf, size, &len, k_sysinfo_psram_free() / 1024u);
    text_append(buf, size, &len, " KB free\n");

    text_append(buf, size, &len, "tasks: ");
    text_append_u64(buf, size, &len, task_count());
    text_append(buf, size, &len, "\n");

    return len;
}

/* --- the answer --- */

/* Send one answer: the status, the headers and the body, then the socket is
 * closed by the caller. `wait` is true here: the answer is the whole of the
 * session, and a half of a page is worse than a slow one. */
static void reply_send(k_socket_t *client, int status, const char *reason, const char *body,
                       k_size_t body_len, bool allow_get)
{
    char head[HTTP_SERVER_HEAD_MAX];
    k_size_t len = 0;

    head[0] = '\0';

    text_append(head, sizeof(head), &len, "HTTP/1.0 ");
    text_append_u64(head, sizeof(head), &len, (k_u64_t)status);
    text_append(head, sizeof(head), &len, " ");
    text_append(head, sizeof(head), &len, reason);
    text_append(head, sizeof(head), &len, "\r\nContent-Type: text/plain; charset=utf-8\r\n");
    text_append(head, sizeof(head), &len, "Content-Length: ");
    text_append_u64(head, sizeof(head), &len, (k_u64_t)body_len);
    text_append(head, sizeof(head), &len, "\r\nConnection: close\r\n");

    if (allow_get) {
        text_append(head, sizeof(head), &len, "Allow: GET\r\n");
    }

    text_append(head, sizeof(head), &len, "\r\n");

    if (k_socket_send(client, head, len, true) < 0) {
        return;
    }

    if (body_len > 0) {
        (void)k_socket_send(client, body, body_len, true);
    }
}

static void reply_400(k_socket_t *client)
{
    static const char body[] = "bad request\n";

    reply_send(client, 400, "Bad Request", body, sizeof(body) - 1, false);
}

/* --- the request --- */

/* The empty line that ends the headers. */
static bool headers_end(const char *buf, k_size_t len)
{
    k_size_t i;

    for (i = 3; i < len; i++) {
        if (buf[i] == '\n' && buf[i - 1] == '\r' && buf[i - 2] == '\n' && buf[i - 3] == '\r') {
            return true;
        }
    }

    return false;
}

/*
 * Read the request into `buf` (at most `size` bytes, the buffer keeps a zero
 * after them). A request that does not end its headers within the buffer or
 * within the time is not answered as one: the caller sends 400 and closes.
 */
static int request_read(k_socket_t *client, char *buf, k_size_t size)
{
    k_u64_t deadline = k_sysinfo_uptime_ms() + HTTP_SERVER_READ_TIMEOUT_MS;
    k_size_t used = 0;
    int n;

    buf[0] = '\0';

    while (used < size) {
        n = k_socket_recv(client, buf + used, size - used);

        if (n > 0) {
            used += (k_size_t)n;
            buf[used] = '\0';

            if (headers_end(buf, used)) {
                return HTTP_REQUEST_OK;
            }

            continue;
        }

        if (n == 0) {
            return HTTP_REQUEST_CLOSED;
        }

        /*
         * The port answers -1 both for a session that was quiet for its poll
         * and for a real error, and the contract does not tell them apart: the
         * clock does, and the deadline of the request is the only thing that
         * turns a silence into a refusal.
         */
        if (k_sysinfo_uptime_ms() >= deadline) {
            return HTTP_REQUEST_TIMEOUT;
        }

        k_sched_sleep_ms(HTTP_SERVER_POLL_MS);
    }

    return HTTP_REQUEST_LONG;
}

/*
 * The first line of the request: `method SP target SP version`. The method and
 * the target become slices of the buffer; the version is not read, because a
 * server of two pages answers the same to 1.0 and 1.1.
 */
static bool request_parts(const char *request, const char **method, k_size_t *method_len,
                          const char **target, k_size_t *target_len)
{
    const char *p = request;
    const char *start;

    while (*p != '\0' && *p != ' ' && *p != '\r' && *p != '\n') {
        p++;
    }

    if (*p != ' ') {
        return false;
    }

    *method = request;
    *method_len = (k_size_t)(p - request);

    start = p + 1;
    p = start;

    while (*p != '\0' && *p != ' ' && *p != '\r' && *p != '\n') {
        p++;
    }

    *target = start;
    *target_len = (k_size_t)(p - start);

    return *method_len > 0 && *target_len > 0;
}

/*
 * Whether the target is `path`. A query is not a part of the path here: the
 * page has no parameters, and a monitor is free to add its own.
 */
static bool target_is(const char *target, k_size_t len, const char *path)
{
    k_size_t path_len = strlen(path);

    if (len < path_len || memcmp(target, path, path_len) != 0) {
        return false;
    }

    return len == path_len || target[path_len] == '?';
}

static bool method_is_get(const char *method, k_size_t len)
{
    return len == 3 && memcmp(method, "GET", 3) == 0;
}

/* --- the session --- */

/*
 * The buffers are static: the server is one task and it answers one request at
 * a time, and eight kilobytes of the stack of the board are not the place for
 * a kilobyte of text that outlives no request anyway.
 */
static void session_serve(k_socket_t *client)
{
    static char request[HTTP_SERVER_REQUEST_MAX + 1];
    static char body[HTTP_SERVER_BODY_MAX];
    const char *method;
    const char *target;
    k_size_t method_len = 0;
    k_size_t target_len = 0;
    k_size_t body_len;
    int rc = request_read(client, request, HTTP_SERVER_REQUEST_MAX);

    if (rc == HTTP_REQUEST_CLOSED) {
        /* The client left before it said anything: there is nobody to answer. */
        return;
    }

    if (rc != HTTP_REQUEST_OK ||
        !request_parts(request, &method, &method_len, &target, &target_len)) {
        reply_400(client);
        return;
    }

    if (!method_is_get(method, method_len)) {
        static const char text[] = "method not allowed\n";

        reply_send(client, 405, "Method Not Allowed", text, sizeof(text) - 1, true);
        return;
    }

    if (target_is(target, target_len, "/health")) {
        static const char text[] = "ok\n";

        reply_send(client, 200, "OK", text, sizeof(text) - 1, false);
        return;
    }

    if (target_is(target, target_len, "/")) {
        body_len = page_of_state(body, sizeof(body));
        reply_send(client, 200, "OK", body, body_len, false);
        return;
    }

    {
        static const char text[] = "not found\n";

        reply_send(client, 404, "Not Found", text, sizeof(text) - 1, false);
    }
}

/* --- the server --- */

/*
 * The two flags are read by the task of the server and written by the task
 * that starts and stops it: they are the only state the two share, and an
 * ordinary read and write of a bool is a race the detector of the host sees.
 * The builtins are the ones the kernel itself uses for the same reason.
 */
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

/* The listener arrives as the argument: the task owns it and closes it, so
 * that no socket is closed under the wait of another task. */
static void http_server_task(void *arg)
{
    k_server_t *server = arg;

    while (server_enabled()) {
        k_socket_t *client = k_server_accept(server, HTTP_SERVER_ACCEPT_TIMEOUT_MS);

        if (client == NULL) {
            continue;
        }

        /* One request, one answer, and the session is closed: see the note at
         * the top of the file. */
        session_serve(client);
        k_socket_close(client);
    }

    k_server_close(server);
    server_set_serving(false);
}

int net_http_server_start(k_u16_t port)
{
    k_server_t *server;

    if (server_serving()) {
        /* Already on: a script of init may name the server twice. A server
         * that is being stopped is not: its port may come back to us. */
        return server_enabled() ? 0 : -1;
    }

    if (port == 0) {
        return -1;
    }

    /*
     * The listener is taken here and not in the task: a caller that gets 0
     * knows the port is ours, and a connect that comes at once finds the door
     * instead of racing the scheduler.
     */
    server = k_server_listen(port);
    if (server == NULL) {
        kwarn("http: the port %u is not ours", (unsigned)port);
        return -1;
    }

    /* The port first: a caller that sees the server running then reads the
     * port it runs on, never the one of a server before it. */
    __atomic_store_n(&s_port, port, __ATOMIC_SEQ_CST);
    server_set_enabled(true);
    server_set_serving(true);

    if (task_create("httpd", http_server_task, server, TASK_PRIO_DEFAULT) == NULL) {
        server_set_enabled(false);
        server_set_serving(false);
        k_server_close(server);
        kwarn("http: no task for the page of the state");
        return -1;
    }

    klog(K_LOG_LEVEL_INFO, "http: the page of the state on port %u", (unsigned)port);

    return 0;
}

void net_http_server_stop(void)
{
    k_u32_t waited = 0;

    if (!server_serving()) {
        return;
    }

    server_set_enabled(false);

    /*
     * The task holds the listener, so it is the task that closes it: closing
     * the socket under a wait of accept is not a thing the port promises. The
     * wait is bounded — a request that is being answered ends by its own
     * timeouts, and init must not hang on a client that went away.
     */
    while (server_serving() && waited < HTTP_SERVER_STOP_WAIT_MS) {
        k_sched_sleep_ms(HTTP_SERVER_STOP_STEP_MS);
        waited += HTTP_SERVER_STOP_STEP_MS;
    }

    if (server_serving()) {
        kwarn("http: the task of the server did not stop in %u ms",
              (unsigned)HTTP_SERVER_STOP_WAIT_MS);
    }
}

bool net_http_server_running(void)
{
    return server_serving() && server_enabled();
}

k_u16_t net_http_server_port(void)
{
    if (!net_http_server_running()) {
        return 0;
    }

    return __atomic_load_n(&s_port, __ATOMIC_SEQ_CST);
}
