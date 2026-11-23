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

#define TEST_PORT 18096
#define TEST_BLOCKS 2048
#define TEST_ROOT "/www"
#define TEST_DATA_SIZE 65536

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static const char g_page[] = "<html><body>hello from the volume</body></html>\n";
static k_u8_t g_data[TEST_DATA_SIZE];
static char g_big[TEST_DATA_SIZE + 4096];

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

static k_socket_t *client_open(void)
{
    int attempt;

    for (attempt = 0; attempt < 20; attempt++) {
        k_socket_t *client = k_socket_connect("127.0.0.1", TEST_PORT, 1000);

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
    k_u64_t deadline = k_sysinfo_uptime_ms() + 4000u;
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

static void test_ceiling(void)
{
    static k_socket_t *idle[HTTPD_CONNECTIONS];
    char answer[1024];
    k_u32_t i;
    k_u32_t opened = 0;

    ktest_begin("httpd: the ceiling of the sessions refuses and says so");

    for (i = 0; i < HTTPD_CONNECTIONS; i++) {
        idle[i] = client_open();

        if (idle[i] == NULL) {
            break;
        }

        opened++;

        /* The session has to be taken by the server before the next one
         * arrives, so the counter of the accepted sessions is real: the
         * accept loop of a busy machine is not the speed of this test. */
        for (k_u32_t wait = 0; wait < 100 && httpd_active() < opened; wait++) {
            k_sched_sleep_ms(20);
        }
    }

    KT_CHECK_INT_EQ((int)opened, (int)HTTPD_CONNECTIONS);
    KT_CHECK_INT_EQ((int)httpd_active(), (int)HTTPD_CONNECTIONS);

    {
        k_socket_t *extra = client_open();

        KT_CHECK(extra != NULL);
        if (extra != NULL) {
            KT_CHECK_INT_EQ(
                k_socket_send(extra, "GET /index.html HTTP/1.1\r\nHost: t\r\n\r\n", 36, true), 36);

            {
                k_size_t used = client_read(extra, answer, sizeof(answer), false);

                KT_CHECK(used > 0);
                KT_CHECK(strstr(answer, "503 Service Unavailable") != NULL);
            }

            k_socket_close(extra);
        }
    }

    KT_CHECK(httpd_refused() >= 1u);
    KT_CHECK(httpd_requests() > 0u);

    for (i = 0; i < opened; i++) {
        k_socket_close(idle[i]);
    }

    /* The sessions end at the close of the client; the stop below waits for
     * the last of them as well. */
    /* Ten seconds and not four: a session is a task of the machine, and a
     * host that builds three worktrees at once schedules it late. The check is
     * that the ceiling lets go - the moment it does is not the subject. */
    for (i = 0; i < 500 && httpd_active() > 0; i++) {
        k_sched_sleep_ms(20);
    }

    KT_CHECK_INT_EQ((int)httpd_active(), 0);
}

void test_httpd(void)
{
    struct httpd_config config;

    if (!make_tree()) {
        KT_CHECK(false);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.root = TEST_ROOT;
    config.log = "";
    config.port = TEST_PORT;
    config.connections = HTTPD_CONNECTIONS;
    config.idle_timeout_ms = 2000;

    ktest_begin("httpd: the server starts on a port of the test");

    KT_CHECK_INT_EQ(httpd_start(&config), HTTPD_OK);
    KT_CHECK(httpd_running());
    KT_CHECK_INT_EQ((int)httpd_port(), TEST_PORT);
    KT_CHECK(httpd_root() != NULL && strcmp(httpd_root(), TEST_ROOT) == 0);

    test_static();
    test_refusals();
    test_keepalive();
    test_idle_timeout();
    test_ceiling();

    ktest_begin("httpd: the door closes");
    httpd_stop();
    KT_CHECK(!httpd_running());
    KT_CHECK_INT_EQ((int)httpd_port(), 0);

    /* A root that is not a directory of the volume is a refusal with a
     * sentence, not a server that serves nothing. */
    ktest_begin("httpd: a root that is not there is refused");
    config.root = TEST_ROOT "/missing";
    KT_CHECK_INT_EQ(httpd_start(&config), HTTPD_EROOT);
    KT_CHECK(!httpd_running());

    drop_tree();
}
