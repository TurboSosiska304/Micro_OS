/*
 * Micro OS — the server of files over HTTP.
 *
 * A program of the system and not a part of the kernel: it lives in the
 * portable half of components/net, its sockets come from kernel/net.h, its
 * files from the VFS, and a port that links neither of them does not build
 * it. The page of the state (net/net_http.h) is its smaller neighbour: two
 * paths of plain text for a monitor. This server is the other shape - the
 * files of the volume, a listing of a directory, the headers a browser and a
 * person's `curl` expect, and many sessions at once.
 *
 * The server keeps the rules of the machine it is on: a ceiling of the
 * sessions that is the task table and not a number of its own, an idle
 * timeout so that a client that went away does not keep a task for ever, and
 * one line of a log per request so that the console of the board or
 * /var/log/httpd.log says what happened.
 *
 * A request changes nothing: GET and HEAD only. A path is resolved under the
 * root of the server and a `..` that would leave it is a refusal, not a
 * climb; a directory is a listing and never the bytes of a file.
 */
#ifndef MICRO_OS_NET_HTTPD_H
#define MICRO_OS_NET_HTTPD_H

#include "kernel/types.h"

/* The port of a server a person starts by hand, and the root of a tree of
 * pages when the caller names none. */
#define HTTPD_PORT 8080
#define HTTPD_ROOT_DEFAULT "/srv"
#define HTTPD_LOG_PATH "/var/log/httpd.log"

/*
 * The ceiling of the sessions that are served at once. The task table of the
 * kernel is 32 slots, the shell and the services of the boot keep theirs, and
 * a session that is waiting is one task: 16 leaves the rest of the machine
 * its room on both the host and the board. A build with more memory changes
 * the number in one place.
 */
#ifndef MICRO_OS_HTTPD_CONNECTIONS
#define MICRO_OS_HTTPD_CONNECTIONS 16
#endif
#define HTTPD_CONNECTIONS MICRO_OS_HTTPD_CONNECTIONS

/* The ceiling of the sessions a caller may ask for: past the table a task
 * cannot be made at all, and an honest refusal is better than a promise. */
#define HTTPD_CONNECTIONS_MAX 24

/* The time a client has to say its request, and the time a session that
 * keeps its connection open may be quiet between two requests. */
#define HTTPD_REQUEST_TIMEOUT_MS 3000u
#define HTTPD_IDLE_TIMEOUT_MS 5000u

/* The requests one connection may carry before the server closes it: a
 * client that keeps a session open for ever must not keep a task for ever. */
#define HTTPD_KEEPALIVE_REQUESTS 100

/* The longest request line and head of headers, the longest path of the tree
 * and the longest line of the log. All three are refusals past the end, never
 * a truncation of a request. */
#define HTTPD_REQUEST_MAX 1024
#define HTTPD_PATH_MAX 192
#define HTTPD_LOG_MAX 192

/* A block of a file and of a listing: one transfer of the socket. */
#define HTTPD_BUFFER 2048

/* The answers of the calls below. */
#define HTTPD_OK 0
#define HTTPD_EBUSY (-1) /* the server is already on, or being stopped */
#define HTTPD_EPORT (-2) /* the port is not ours */
#define HTTPD_EROOT (-3) /* the root is not a directory */
#define HTTPD_ETASK (-4) /* no task for the server */
#define HTTPD_EARGS (-5) /* the configuration is not one we serve */

struct httpd_config {
    const char *root;        /* an absolute path of the VFS; NULL is the default */
    const char *log;         /* a file of the VFS; NULL or "" is the console */
    k_u16_t port;            /* 0 is refused */
    k_u8_t connections;      /* 0 is HTTPD_CONNECTIONS, more than MAX is refused */
    k_u32_t idle_timeout_ms; /* 0 is HTTPD_IDLE_TIMEOUT_MS */
    /* Serve TLS on this port as well: the pair of net/tls_server.h is the
     * certificate of the machine, read from /etc/ssl. An image without the
     * serving side says so and starts plain HTTP instead. */
    bool tls;
};

/*
 * Start the server. The listener is taken here, before the task runs, so a
 * caller that gets 0 knows the port is ours. A server that is already on
 * answers 0 and keeps its port: a script of init may name it twice.
 */
int httpd_start(const struct httpd_config *config);

/* Stop the server: no new session, and the ones that are open are given the
 * time of their own timeouts to end. Bounded, because init must not hang. */
void httpd_stop(void);

bool httpd_running(void);

/* The port of the running server, 0 when it is not running. */
k_u16_t httpd_port(void);

/* The root a running server serves, or NULL. */
const char *httpd_root(void);

/* What the server has done since it started: the requests it answered, the
 * sessions it refused because the ceiling was reached, and the sessions it
 * serves right now. The console and the tests read them. */
k_u32_t httpd_requests(void);
k_u32_t httpd_refused(void);
k_u32_t httpd_active(void);

#endif /* MICRO_OS_NET_HTTPD_H */
