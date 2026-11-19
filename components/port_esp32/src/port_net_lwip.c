/*
 * Micro OS — the network of the ESP32: the sockets of lwIP.
 *
 * This file is the port side of kernel/port/net.h. The portable code names a
 * host and a port and gets an opaque session; the resolution of the name,
 * the timeouts and the sockets of lwIP stay here, because they are the stack
 * of the chip and of nobody else.
 *
 * Every wait has a limit. A home network comes and goes, and a command of
 * the shell must answer whether or not anybody is on the other side.
 */
#include "kernel/port.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/netdb.h>
#include <lwip/sockets.h>

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
};

struct k_server {
    int fd;
};

static struct k_socket *session_new(int fd)
{
    struct k_socket *session = malloc(sizeof(*session));

    if (session == NULL) {
        /* Without the little struct there is no session to hand out. */
        close(fd);
        return NULL;
    }

    session->fd = fd;

    return session;
}

static int set_timeout(int fd, k_u32_t timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }

    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

    /*
     * lwIP gives every address the room of a `sockaddr_storage` even when
     * the family is IPv4: the copy is the size of the address we asked for.
     */
    memset(out, 0, sizeof(*out));
    memcpy(out, result->ai_addr, sizeof(*out));
    freeaddrinfo(result);

    return 0;
}

k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    struct sockaddr_in address;
    int fd;

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

    if (set_timeout(fd, timeout_ms) != 0 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        kwarn("net: cannot connect to %s:%u (rc=%d)", host, (unsigned)port, (int)errno);
        close(fd);
        return NULL;
    }

    return session_new(fd);
}

int k_port_net_send(k_port_socket_t *socket, const void *data, k_size_t size, bool wait)
{
    const char *p = data;
    k_size_t sent = 0;

    if (socket == NULL || data == NULL) {
        return -1;
    }

    if (!wait) {
        int n = send(socket->fd, p, size, MSG_DONTWAIT);

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
        int n = send(socket->fd, p + sent, size - sent, 0);

        if (n <= 0) {
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

    return recv(socket->fd, data, size, 0);
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

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        kwarn("net: cannot make the listener of port %u (rc=%d)", (unsigned)port, (int)errno);
        return NULL;
    }

    /* A board that comes back after a reset finds its own old socket in
     * the pile of the stack for a while: the port is ours to reuse. */
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
    struct timeval timeout;
    fd_set read_fds;
    int fd;
    int ready;

    if (server == NULL) {
        return NULL;
    }

    FD_ZERO(&read_fds);
    FD_SET(server->fd, &read_fds);

    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

    ready = select(server->fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ready <= 0 || !FD_ISSET(server->fd, &read_fds)) {
        return NULL;
    }

    fd = accept(server->fd, NULL, NULL);
    if (fd < 0) {
        return NULL;
    }

    if (set_timeout(fd, PORT_NET_SESSION_TIMEOUT_MS) != 0) {
        close(fd);
        return NULL;
    }

    return session_new(fd);
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

    if (set_timeout(fd, timeout_ms) != 0) {
        close(fd);
        return -1;
    }

    n = sendto(fd, request, request_size, 0, (struct sockaddr *)&address, sizeof(address));
    if (n != (int)request_size) {
        kwarn("net: cannot send to %s:%u (rc=%d)", host, (unsigned)port, (int)errno);
        close(fd);
        return -1;
    }

    n = recv(fd, answer, answer_size, 0);
    close(fd);

    if (n <= 0) {
        kwarn("net: no answer from %s:%u (rc=%d)", host, (unsigned)port, (int)errno);
        return -1;
    }

    return n;
}
