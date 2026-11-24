/*
 * Micro OS — the server of files over HTTP, on the loopback of the host.
 *
 * The volume is a RAM volume of MicroFS mounted at the root of the test
 * machine, and the server serves /www of it. The client is the test itself,
 * through the sockets of the port: the request and the answer cross the
 * stack of the host and the code of the server, and what this case proves is
 * the shape of the answer - the length, the tag, the range, the listing, the
 * refusal of a path that climbs, and the ceiling of the sessions.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "net/httpd.h"

#define TEST_BLOCKS 2048
#define TEST_ROOT "/www"
#define TEST_DATA_SIZE 65536

/*
 * The sessions this case asks for. The table of the tasks of the kernel has
 * 32 slots and the suite before this case leaves its own tasks in it, so six
 * sessions are what the test surely holds; the ceiling is a number of the
 * configuration and the case proves the rule with it.
 */
#define TEST_CONNECTIONS 6

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static const char g_page[] = "<html><body>hello from the volume</body></html>\n";
static k_u8_t g_data[TEST_DATA_SIZE];
static char g_big[TEST_DATA_SIZE + 4096];

/*
 * A port of the loopback for this process, taken once. Two worktrees of a
 * host run this suite at the same time, and a number written in the tree
 * belongs to whichever of them bound it first - the other one then fails a
 * check that has nothing to do with the code under test. The port is asked
 * of the kernel with a listener of its own: a probe that binds is a port
 * that is free at this moment, and the caller starts the server with the
 * number that follows it when the start itself loses the race.
 */
static k_u16_t test_free_port(void)
{
    static k_u16_t s_candidate;

    if (s_candidate == 0) {
        /* The moment of this process is the seed: two copies of the suite
         * start at different times, and a collision that the seed cannot
         * avoid is caught by the probe below and by the start of the server. */
        s_candidate = (k_u16_t)(18100u + (k_u32_t)(k_sysinfo_uptime_ms() % 5000u));
    }

    {
        unsigned attempt;

        for (attempt = 0; attempt < 500; attempt++) {
            k_server_t *probe;

            if (s_candidate < 18100u || s_candidate >= 24000u) {
                s_candidate = 18100u;
            }

            probe = k_server_listen(s_candidate);

            if (probe != NULL) {
                k_server_close(probe);
                return s_candidate++;
            }

            s_candidate++;
        }
    }

    /* A machine with every number of the range taken is a machine this test
     * cannot use; the caller reads the zero and fails with its own words. */
    return 0;
}

/* --- the tree of the test --- */

static bool make_tree(void)
{
    static k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_size_t i;

    if (mfs_ram_init(&g_volume, TEST_BLOCKS) != 0) {
        return false;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);

    if (mfs_vfs_register("/") != 0) {
        return false;
    }

    if (vfs_mkdir_p(TEST_ROOT "/sub") != 0) {
        return false;
    }

    if (vfs_open(TEST_ROOT "/index.html", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) !=
        0) {
        return false;
    }
    (void)vfs_write(&file, g_page, sizeof(g_page) - 1);
    (void)vfs_close(&file);

    if (vfs_open(TEST_ROOT "/sub/a.txt", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) !=
        0) {
        return false;
    }
    (void)vfs_write(&file, "alpha\n", 6);
    (void)vfs_close(&file);

    for (i = 0; i < sizeof(g_data); i++) {
        g_data[i] = (k_u8_t)((i * 31u + 7u) & 0xFFu);
    }

    if (vfs_open(TEST_ROOT "/data.bin", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) != 0) {
        return false;
    }
    (void)vfs_write(&file, g_data, sizeof(g_data));
    (void)vfs_close(&file);

    return true;
}

static void drop_tree(void)
{
    (void)vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* --- the client of the test --- */

/* The port the server took for this run: the case finds it once and every
 * client of the test asks it. */
static k_u16_t s_server_port;

static k_socket_t *client_open(void)
{
    int attempt;

    for (attempt = 0; attempt < 20; attempt++) {
        k_socket_t *client = k_socket_connect("127.0.0.1", s_server_port, 1000);

        if (client != NULL) {
            return client;
        }

        k_sched_sleep_ms(20);
    }

    return NULL;
}

/* Whether the answer is complete: the head, and then the body its length
 * promises (or the chunk that ends a chunked one). */
static bool answer_complete(const char *buffer, k_size_t used, k_size_t *head_len)
{
    const char *end = NULL;
    const char *length;
    k_size_t i;
    k_size_t promised = 0;

    for (i = 3; i < used; i++) {
        if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' && buffer[i - 1] == '\r' &&
            buffer[i] == '\n') {
            end = buffer + i + 1;
            break;
        }
    }

    if (end == NULL) {
        return false;
    }

    *head_len = (k_size_t)(end - buffer);

    if (strstr(buffer, "Transfer-Encoding: chunked") != NULL) {
        return used >= 5 && memcmp(buffer + used - 5, "0\r\n\r\n", 5) == 0;
    }

    length = strstr(buffer, "Content-Length: ");
    if (length == NULL) {
        /* A 304 has no body and no length. */
        return true;
    }

    length += strlen("Content-Length: ");
    while (*length >= '0' && *length <= '9') {
        promised = promised * 10u + (k_size_t)(*length - '0');
        length++;
    }

    return used >= *head_len + promised;
}

/*
 * The whole answer of one connection: the head of the first one and the body
 * that its length promises, then the same for the next one while the
 * connection stays open. `until_close` reads to the end of the session, which
 * is what a request with `Connection: close` promises.
 */
static k_size_t client_read(k_socket_t *client, char *buffer, k_size_t size, bool until_close)
{
    /* Ten seconds and not four: the answer is the server's task, and a host
     * that builds three worktrees at once schedules it late. The bound is
     * still a bound - a door that never answers fails this case. */
    k_u64_t deadline = k_sysinfo_uptime_ms() + 10000u;
    k_size_t used = 0;
    k_size_t head_len = 0;

    buffer[0] = '\0';

    while (used < size - 1) {
        int n = k_socket_recv(client, buffer + used, size - 1 - used);

        if (n > 0) {
            used += (k_size_t)n;
            buffer[used] = '\0';

            if (!until_close && answer_complete(buffer, used, &head_len)) {
                return used;
            }

            continue;
        }

        if (n == 0) {
            break;
        }

        if (k_sysinfo_uptime_ms() >= deadline) {
            break;
        }

        k_sched_sleep_ms(10);
    }

    return used;
}

/* One request, one answer, and the connection follows it. */
static k_size_t request(const char *text, char *answer, k_size_t size)
{
    k_socket_t *client = client_open();
    k_size_t used;

    if (client == NULL) {
        answer[0] = '\0';
        return 0;
    }

    (void)k_socket_send(client, text, strlen(text), true);
    used = client_read(client, answer, size, false);
    k_socket_close(client);

    return used;
}

/* The value of a header of an answer, copied; false when it is not there. */
static bool header_value(const char *answer, const char *name, char *out, k_size_t size)
{
    const char *at = strstr(answer, name);
    k_size_t len = 0;

    if (at == NULL) {
        return false;
    }

    at += strlen(name);

    while (*at == ' ') {
        at++;
    }

    while (at[len] != '\0' && at[len] != '\r' && at[len] != '\n' && len + 1 < size) {
        out[len] = at[len];
        len++;
    }

    out[len] = '\0';

    return true;
}

/* The body of an answer: after the empty line of the head. */
static const char *answer_body(const char *answer)
{
    const char *at = strstr(answer, "\r\n\r\n");

    return at != NULL ? at + 4 : NULL;
}

/* --- the cases --- */

static void test_static(void)
{
    char answer[2048];
    char all[8192];
    char value[128];
    const char *body;

    ktest_begin("httpd: a file, its type, its length and its tag");

    KT_CHECK(request("GET /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(strstr(answer, "Content-Type: text/html; charset=utf-8") != NULL);
    KT_CHECK(header_value(answer, "Content-Length:", value, sizeof(value)));
    KT_CHECK(strcmp(value, "48") == 0);

    body = answer_body(answer);
    KT_CHECK(body != NULL && memcmp(body, g_page, sizeof(g_page) - 1) == 0);
    KT_CHECK(header_value(answer, "ETag:", value, sizeof(value)));
    KT_CHECK(value[0] == '"' && value[strlen(value) - 1] == '"');

    ktest_begin("httpd: a binary file and a HEAD without a body");

    KT_CHECK(request("GET /data.bin HTTP/1.0\r\n\r\n", g_big, sizeof(g_big)) > 0);
    KT_CHECK(strstr(g_big, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(strstr(g_big, "Content-Type: application/octet-stream") != NULL);
    KT_CHECK(strstr(g_big, "Accept-Ranges: bytes") != NULL);

    body = answer_body(g_big);
    KT_CHECK(body != NULL);
    if (body != NULL) {
        KT_CHECK(memcmp(body, g_data, sizeof(g_data)) == 0);
    }

    KT_CHECK(request("HEAD /data.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(header_value(answer, "Content-Length:", value, sizeof(value)));
    KT_CHECK_INT_EQ((int)strlen(value), 5);
    KT_CHECK(strcmp(value, "65536") == 0);

    /* A HEAD has no body: the answer ends with the empty line of the head. */
    ktest_begin("httpd: the answer of a HEAD is the head alone");

    KT_CHECK(request("HEAD /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    {
        const char *end = strstr(answer, "\r\n\r\n");
        k_size_t head = end != NULL ? (k_size_t)(end + 4 - answer) : 0;

        KT_CHECK(end != NULL && strlen(answer) == head);
    }

    ktest_begin("httpd: bytes, a suffix and an unsatisfiable range");

    KT_CHECK(request("GET /data.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=10-19\r\n"
                     "Connection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 206 Partial Content") != NULL);
    KT_CHECK(strstr(answer, "Content-Range: bytes 10-19/65536") != NULL);
    KT_CHECK(header_value(answer, "Content-Length:", value, sizeof(value)));
    KT_CHECK(strcmp(value, "10") == 0);
    body = answer_body(answer);
    KT_CHECK(body != NULL && memcmp(body, g_data + 10, 10) == 0);

    KT_CHECK(request("GET /data.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=-5\r\n"
                     "Connection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 206 Partial Content") != NULL);
    KT_CHECK(strstr(answer, "Content-Range: bytes 65531-65535/65536") != NULL);
    body = answer_body(answer);
    KT_CHECK(body != NULL && memcmp(body, g_data + TEST_DATA_SIZE - 5, 5) == 0);

    KT_CHECK(request("GET /data.bin HTTP/1.1\r\nHost: t\r\nRange: bytes=999999-\r\n"
                     "Connection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 416 Range Not Satisfiable") != NULL);
    KT_CHECK(strstr(answer, "Content-Range: bytes */65536") != NULL);

    ktest_begin("httpd: 304 when the client has the tag");

    KT_CHECK(request("GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n", answer, sizeof(answer)) > 0);
    KT_CHECK(header_value(answer, "ETag:", value, sizeof(value)));

    {
        char request_text[256];
        char tag[128];

        memcpy(tag, value, sizeof(tag));
        tag[sizeof(tag) - 1] = '\0';

        snprintf(request_text, sizeof(request_text),
                 "GET /index.html HTTP/1.1\r\nHost: t\r\nIf-None-Match: %s\r\n"
                 "Connection: close\r\n\r\n",
                 tag);

        KT_CHECK(request(request_text, answer, sizeof(answer)) > 0);
    }

    KT_CHECK(strstr(answer, "HTTP/1.1 304 Not Modified") != NULL);
    KT_CHECK(strstr(answer, "\r\n\r\n") != NULL);
    KT_CHECK(answer_body(answer) != NULL && strlen(answer_body(answer)) == 0);

    ktest_begin("httpd: a directory is a listing and not the bytes of a file");

    KT_CHECK(request("GET /sub HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 301 Moved Permanently") != NULL);
    KT_CHECK(strstr(answer, "Location: /sub/") != NULL);

    KT_CHECK(request("GET /sub/ HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", all,
                     sizeof(all)) > 0);
    KT_CHECK(strstr(all, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(strstr(all, "Transfer-Encoding: chunked") != NULL);
    KT_CHECK(strstr(all, "text/html") != NULL);
    KT_CHECK(strstr(all, "a.txt") != NULL);
    KT_CHECK(strstr(all, "0\r\n\r\n") != NULL);

    KT_CHECK(request("GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", all, sizeof(all)) >
             0);
    KT_CHECK(strstr(all, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(strstr(all, "index.html") != NULL);
    KT_CHECK(strstr(all, "sub/") != NULL);
}

static void test_refusals(void)
{
    char answer[2048];

    ktest_begin("httpd: a path that climbs out of the root is refused");

    KT_CHECK(request("GET /../etc/passwd HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 403 Forbidden") != NULL);

    KT_CHECK(request("GET /%2e%2e/etc/passwd HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 403 Forbidden") != NULL);

    KT_CHECK(request("GET /sub/../../etc/passwd HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 403 Forbidden") != NULL);

    /* A `..` that stays inside the root is a path like any other. */
    KT_CHECK(request("GET /sub/../index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 200 OK") != NULL);

    ktest_begin("httpd: a missing file, a method it does not serve");

    KT_CHECK(request("GET /nope.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", answer,
                     sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 404 Not Found") != NULL);

    KT_CHECK(request("POST /index.html HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 405 Method Not Allowed") != NULL);
    KT_CHECK(strstr(answer, "Allow: GET, HEAD") != NULL);

    ktest_begin("httpd: a malformed request is 400 and the session ends");

    KT_CHECK(request("nonsense\r\n\r\n", answer, sizeof(answer)) > 0);
    KT_CHECK(strstr(answer, "HTTP/1.1 400 Bad Request") != NULL);
}

static void test_keepalive(void)
{
    static char all[8192];
    k_socket_t *client;
    static const char two[] = "GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n"
                              "GET /sub/a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    k_size_t used;

    ktest_begin("httpd: two requests on one connection, a 200 and a 200");

    client = client_open();
    KT_CHECK(client != NULL);
    if (client == NULL) {
        return;
    }

    KT_CHECK_INT_EQ(k_socket_send(client, two, sizeof(two) - 1, true), (int)(sizeof(two) - 1));
    used = client_read(client, all, sizeof(all), true);
    k_socket_close(client);

    KT_CHECK(used > 0);
    KT_CHECK(strstr(all, "HTTP/1.1 200 OK") != NULL);
    KT_CHECK(strstr(all, "hello from the volume") != NULL);
    KT_CHECK(strstr(all, "alpha\n") != NULL);
    /* Two answers: the first one says the connection is kept. */
    KT_CHECK(strstr(all, "Connection: keep-alive") != NULL);
    KT_CHECK(strstr(all, "Connection: close") != NULL);
}

/*
 * A session that says nothing is not kept for ever: the server closes it at
 * the idle timeout of its configuration, and the client reads the end of the
 * session. The timeout of this test is two seconds.
 */
static void test_idle_timeout(void)
{
    static const char quiet[] = "GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n";
    static char answer[4096];
    k_socket_t *client;
    k_u64_t deadline;
    int rc = -1;

    ktest_begin("httpd: a session that is quiet ends at the idle timeout");

    client = client_open();
    KT_CHECK(client != NULL);
    if (client == NULL) {
        return;
    }

    KT_CHECK_INT_EQ(k_socket_send(client, quiet, sizeof(quiet) - 1, true),
                    (int)(sizeof(quiet) - 1));

    KT_CHECK(client_read(client, answer, sizeof(answer), false) > 0);

    /* Nothing more is sent: the keep-alive session has to end by itself. */
    deadline = k_sysinfo_uptime_ms() + 6000u;

    while (k_sysinfo_uptime_ms() < deadline) {
        rc = k_socket_recv(client, answer, sizeof(answer));

        if (rc == 0) {
            break;
        }

        k_sched_sleep_ms(50);
    }

    KT_CHECK_INT_EQ(rc, 0);
    k_socket_close(client);
}

/*
 * The number of sessions of the door, as the test keeps it: the count is the
 * server's and the wait is the test's, so a door that took the client but has
 * not counted it yet is a wait and not an answer to the next check.
 */
static bool wait_for_sessions(k_u32_t want, k_u32_t stable_ms, k_u32_t timeout_ms, const char *what)
{
    k_u64_t deadline = k_sysinfo_uptime_ms() + (k_u64_t)timeout_ms;
    k_u64_t stable_since = 0;
    bool steady = false;

    for (;;) {
        if (httpd_active() == want) {
            if (!steady) {
                steady = true;
                stable_since = k_sysinfo_uptime_ms();
            } else if (k_sysinfo_uptime_ms() - stable_since >= (k_u64_t)stable_ms) {
                return true;
            }
        } else {
            steady = false;
        }

        if (k_sysinfo_uptime_ms() >= deadline) {
            ktest_fail(__FILE__, __LINE__, "%s: %u session(s) after %u ms, wanted %u", what,
                       (unsigned)httpd_active(), (unsigned)timeout_ms, (unsigned)want);
            return false;
        }

        k_sched_sleep_ms(10);
    }
}

/*
 * A client that closes is a session that ends. The door of this phase keeps
 * an idle session for thirty seconds, so a session that waited for its
 * timeout instead of the close would still be counted here: the wait of the
 * check is five seconds and the difference is the point of the case.
 */
static void test_close_is_noticed(void)
{
    static const char ask[] = "GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n";
    char answer[1024];
    k_socket_t *client;

    ktest_begin("httpd: a session ends at the close of the client");

    client = client_open();
    KT_CHECK(client != NULL);

    if (client == NULL) {
        return;
    }

    KT_CHECK_INT_EQ(k_socket_send(client, ask, sizeof(ask) - 1, true), (int)(sizeof(ask) - 1));
    KT_CHECK(client_read(client, answer, sizeof(answer), false) > 0);
    KT_CHECK(wait_for_sessions(1, 0, 5000u, "the session of the client"));

    k_socket_close(client);

    KT_CHECK(wait_for_sessions(0, 100u, 5000u, "the session after the close"));
}

/*
 * A client that closes in the instant between its connect and the session of
 * the door is the case where a count added after the task exists is lost:
 * the task may run and give its count back before the accept loop counts it.
 * The door counts first and serves after, and twenty of these leave nothing
 * behind.
 */
static void test_ghost_clients(void)
{
    unsigned i;

    ktest_begin("httpd: clients that close at once leave no session behind");

    for (i = 0; i < 20; i++) {
        k_socket_t *ghost = client_open();

        if (ghost != NULL) {
            k_socket_close(ghost);
        }
    }

    KT_CHECK(wait_for_sessions(0, 300u, 10000u, "the ghosts of the door"));
}

/*
 * The ceiling of the sessions. Every session of the door is one client that
 * has asked something and stays for the next request, and the door of this
 * phase keeps it for thirty seconds - so the count is the count of the
 * clients, not of the seconds of a loaded host. A client over it is refused
 * with 503, and the count comes back to zero after the clients close: a
 * session that is still there ten seconds later is a leak and the check says
 * so with the numbers of the door.
 */
static void test_ceiling(void)
{
    static k_socket_t *idle[TEST_CONNECTIONS];
    static const char ask[] = "GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n";
    char answer[1024];
    k_u32_t opened = 0;
    k_u32_t i;
    bool full = true;

    ktest_begin("httpd: the ceiling of the sessions refuses and says so");

    for (i = 0; i < TEST_CONNECTIONS; i++) {
        k_size_t used;

        idle[i] = client_open();

        if (idle[i] == NULL) {
            full = false;
            break;
        }

        opened++;

        /* A request, so the session is one of the door and not a client the
         * accept has not taken yet: the answer is read and the connection
         * stays for the next request, which is what keep-alive is. */
        KT_CHECK_INT_EQ(k_socket_send(idle[i], ask, sizeof(ask) - 1, true), (int)(sizeof(ask) - 1));

        used = client_read(idle[i], answer, sizeof(answer), false);
        KT_CHECK(used > 0);

        if (used == 0 || !wait_for_sessions(opened, 0, 5000u, "a session of the door")) {
            full = false;
            break;
        }
    }

    KT_CHECK(full);

    if (full) {
        k_socket_t *extra = client_open();

        KT_CHECK(extra != NULL);

        if (extra != NULL) {
            k_size_t used;

            KT_CHECK_INT_EQ(k_socket_send(extra, ask, sizeof(ask) - 1, true),
                            (int)(sizeof(ask) - 1));

            used = client_read(extra, answer, sizeof(answer), false);

            /* The session over the ceiling is refused: the answer is read or
             * the case says what the door did instead, with its numbers,
             * rather than failing on a race of a moment. */
            if (used == 0 || strstr(answer, "503 Service Unavailable") == NULL) {
                ktest_fail(__FILE__, __LINE__,
                           "the session over the ceiling was not refused: %u byte(s), "
                           "%u active, %u request(s), %u refused",
                           (unsigned)used, (unsigned)httpd_active(), (unsigned)httpd_requests(),
                           (unsigned)httpd_refused());
            }

            KT_CHECK(strstr(answer, "503 Service Unavailable") != NULL);
            k_socket_close(extra);
        }

        KT_CHECK(httpd_refused() >= 1u);
    }

    KT_CHECK(httpd_requests() > 0u);

    for (i = 0; i < opened; i++) {
        k_socket_close(idle[i]);
    }

    /* The sessions end at the close of the client; the count has to come
     * back to zero and stay there, which is what the steady window says. */
    KT_CHECK(wait_for_sessions(0, 500u, 10000u, "the sessions after the close"));
}

/* The server of a phase of the case: a port of the machine and the timeout
 * the phase needs. A port that loses the race to another copy of the suite
 * costs the next number and not the case. */
static bool start_server(k_u32_t idle_ms)
{
    unsigned attempt;

    for (attempt = 0; attempt < 8; attempt++) {
        struct httpd_config config;

        memset(&config, 0, sizeof(config));
        config.root = TEST_ROOT;
        config.log = "";
        config.connections = TEST_CONNECTIONS;
        config.idle_timeout_ms = idle_ms;
        config.port = test_free_port();

        if (httpd_start(&config) == HTTPD_OK) {
            s_server_port = (k_u16_t)httpd_port();
            return true;
        }
    }

    return false;
}

void test_httpd(void)
{
    if (!make_tree()) {
        KT_CHECK(false);
        return;
    }

    ktest_begin("httpd: the server starts on a port of the test");

    KT_CHECK(start_server(2000));
    KT_CHECK(httpd_running());
    KT_CHECK(httpd_port() != 0);
    KT_CHECK(httpd_root() != NULL && strcmp(httpd_root(), TEST_ROOT) == 0);

    test_static();
    test_refusals();
    test_keepalive();
    test_idle_timeout();

    ktest_begin("httpd: the door closes");
    httpd_stop();
    KT_CHECK(!httpd_running());
    KT_CHECK_INT_EQ((int)httpd_port(), 0);

    /* The ceiling wants sessions that are still there when the last client
     * arrives: the door of this phase keeps an idle session for thirty
     * seconds, and the sessions of the idle test above are the ones of its
     * two seconds. The two phases do not share a timeout because they do not
     * ask the same question. */
    ktest_begin("httpd: the door serves the ceiling with a long idle timeout");
    KT_CHECK(start_server(30000));
    KT_CHECK(httpd_running());

    test_close_is_noticed();
    test_ghost_clients();
    test_ceiling();

    ktest_begin("httpd: the door closes after the ceiling");
    httpd_stop();
    KT_CHECK(!httpd_running());

    /* A root that is not a directory of the volume is a refusal with a
     * sentence, not a server that serves nothing. */
    ktest_begin("httpd: a root that is not there is refused");
    {
        struct httpd_config config;

        memset(&config, 0, sizeof(config));
        config.root = TEST_ROOT "/missing";
        config.log = "";
        config.port = test_free_port();

        KT_CHECK_INT_EQ(httpd_start(&config), HTTPD_EROOT);
    }
    KT_CHECK(!httpd_running());

    drop_tree();
}
