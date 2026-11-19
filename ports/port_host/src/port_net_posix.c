/*
 * Micro OS — the network of the host: the sockets of the process.
 *
 * This file is the port side of kernel/port/net.h. It exists so that the
 * HTTP client, the clock of NTP and the telnet server are exercised on a
 * laptop, over the loopback, without a board.
 *
 * The timeouts go through select and not through SO_RCVTIMEO/SO_SNDTIMEO,
 * although the host would take either: the board's stack only has the
 * socket options, so the waits are written the way both ports can keep the
 * same promise — a call answers within its timeout and says -1 when it ran
 * out of time.
 */
#include "kernel/port.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kernel/log.h"

/*
 * How long an accepted session waits for its client before it answers -1.
 * The contract has no separate "nothing yet" answer, so a session polls: the
 * console over the air uses that second of silence to look at its own state
 * and to notice a client that has gone away without a word.
 */
#define PORT_NET_SESSION_TIMEOUT_MS 1000

struct k_socket {
    int fd;
    k_u32_t timeout_ms;
};

struct k_server {
    int fd;
};

static struct k_socket *session_new(int fd, k_u32_t timeout_ms)
{
    struct k_socket *session = malloc(sizeof(*session));

    if (session == NULL) {
        /* Without the little struct there is no session to hand out. */
        close(fd);
        return NULL;
    }

    session->fd = fd;
    session->timeout_ms = timeout_ms;

    return session;
}

/* Wait until the socket is ready. 1 ready, 0 the time is up, -1 an error. */
static int wait_fd(int fd, bool write, k_u32_t timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = (time_t)(timeout_ms / 1000u);
    timeout.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

    for (;;) {
        fd_set set;
        int rc;

        FD_ZERO(&set);
        FD_SET(fd, &set);

        rc = select(fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &timeout);
        if (rc >= 0) {
            return rc > 0 ? 1 : 0;
        }
        if (errno != EINTR) {
            return -1;
        }
        /* A signal cut the wait short: the time that is left is in timeout,
         * so the loop waits it out instead of starting over. */
    }
}

static int resolve(const char *host, k_u16_t port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char service[8];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;

    snprintf(service, sizeof(service), "%u", (unsigned)port);

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0 || result == NULL || result->ai_addr == NULL) {
        if (result != NULL) {
            freeaddrinfo(result);
        }
        kwarn("net: cannot resolve %s:%u (rc=%d)", host, (unsigned)port, rc);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out, result->ai_addr, sizeof(*out));
    freeaddrinfo(result);

    return 0;
}

k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    struct sockaddr_in address;
    int fd;
    int flags;

    if (host == NULL) {
        return NULL;
    }

    if (resolve(host, port, SOCK_STREAM, &address) != 0) {
        return NULL;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        kwarn("net: cannot make a socket (rc=%d)", (int)errno);
        return NULL;
    }

    /*
     * A connect to a host that is not there waits for the default of the
     * system, which is a minute and more: non-blocking plus select gives it
     * the timeout of the caller, the way SO_SNDTIMEO does on the board.
     */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        kwarn("net: cannot make the socket non-blocking (rc=%d)", (int)errno);
        close(fd);
        return NULL;
    }

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int error = 0;
        int failure = errno;
        socklen_t error_size = sizeof(error);
        bool connected;

        if (failure != EINPROGRESS) {
            connected = false;
        } else if (wait_fd(fd, true, timeout_ms) != 1) {
            connected = false;
        } else {
            connected =
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0;
        }

        if (!connected) {
            kwarn("net: cannot connect to %s:%u (rc=%d)", host, (unsigned)port,
                  error != 0 ? error : failure);
            close(fd);
            return NULL;
        }
    }

    if (fcntl(fd, F_SETFL, flags) != 0) {
        kwarn("net: cannot make the socket blocking again (rc=%d)", (int)errno);
        close(fd);
        return NULL;
    }

    return session_new(fd, timeout_ms);
}

int k_port_net_send(k_port_socket_t *socket, const void *data, k_size_t size, bool wait)
{
    const char *p = data;
    k_size_t sent = 0;

    if (socket == NULL || data == NULL) {
        return -1;
    }

    if (!wait) {
        /* MSG_NOSIGNAL: a client that is gone turns the write into a
         * negative answer, not into a signal that kills the board. */
        int n = (int)send(socket->fd, p, size, MSG_DONTWAIT | MSG_NOSIGNAL);

        if (n >= 0) {
            return n;
        }

        /*
         * A window that is full is not a client that is gone: nothing was
         * taken, and the next line may fit. Only a real error is reported
         * as one, because the session reads it as the end of the client.
         */
        return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
    }

    while (sent < size) {
        ssize_t n;

        /* select first: a blocking send of a host that stopped reading
         * waits for the default of the system, which is not a timeout. */
        if (wait_fd(socket->fd, true, socket->timeout_ms) != 1) {
            return -1;
        }

        n = send(socket->fd, p + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (k_size_t)n;
    }

    return (int)size;
}

int k_port_net_recv(k_port_socket_t *socket, void *data, k_size_t size)
{
    if (socket == NULL || data == NULL) {
        return -1;
    }

    for (;;) {
        ssize_t n;

        if (wait_fd(socket->fd, false, socket->timeout_ms) != 1) {
            return -1;
        }

        n = recv(socket->fd, data, size, 0);
        if (n >= 0) {
            return (int)n;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

void k_port_net_close(k_port_socket_t *socket)
{
    if (socket == NULL) {
        return;
    }

    close(socket->fd);
    free(socket);
}

k_port_server_t *k_port_net_listen(k_u16_t port)
{
    struct sockaddr_in address;
    struct k_server *server;
    int one = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        kwarn("net: cannot make the listener of port %u (rc=%d)", (unsigned)port, (int)errno);
        return NULL;
    }

    /* A test that runs twice in a row, or a server that was restarted,
     * finds its own old socket in the pile for a while: the port is ours. */
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 1) != 0) {
        kwarn("net: cannot listen on port %u (rc=%d)", (unsigned)port, (int)errno);
        close(fd);
        return NULL;
    }

    server = malloc(sizeof(*server));
    if (server == NULL) {
        close(fd);
        return NULL;
    }

    server->fd = fd;

    return server;
}

k_port_socket_t *k_port_net_accept(k_port_server_t *server, k_u32_t timeout_ms)
{
    int fd;

    if (server == NULL) {
        return NULL;
    }

    if (wait_fd(server->fd, false, timeout_ms) != 1) {
        return NULL;
    }

    fd = accept(server->fd, NULL, NULL);
    if (fd < 0) {
        return NULL;
    }

    return session_new(fd, PORT_NET_SESSION_TIMEOUT_MS);
}

void k_port_net_server_close(k_port_server_t *server)
{
    if (server == NULL) {
        return;
    }

    close(server->fd);
    free(server);
}

int k_port_net_udp_exchange(const char *host, k_u16_t port, const void *request,
                            k_size_t request_size, void *answer, k_size_t answer_size,
                            k_u32_t timeout_ms)
{
    struct sockaddr_in address;
    int fd;
    int n;

    if (host == NULL || request == NULL || answer == NULL) {
        return -1;
    }

    if (resolve(host, port, SOCK_DGRAM, &address) != 0) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        kwarn("net: cannot make a UDP socket (rc=%d)", (int)errno);
        return -1;
    }

    n = (int)sendto(fd, request, request_size, 0, (struct sockaddr *)&address, sizeof(address));
    if (n != (int)request_size) {
        kwarn("net: cannot send to %s:%u (rc=%d)", host, (unsigned)port, (int)errno);
        close(fd);
        return -1;
    }

    if (wait_fd(fd, false, timeout_ms) != 1) {
        kwarn("net: no answer from %s:%u", host, (unsigned)port);
        close(fd);
        return -1;
    }

    n = (int)recv(fd, answer, answer_size, 0);
    close(fd);

    if (n <= 0) {
        kwarn("net: no answer from %s:%u (rc=%d)", host, (unsigned)port, (int)errno);
        return -1;
    }

    return n;
}
