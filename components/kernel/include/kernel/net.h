/*
 * Micro OS — the network, as the kernel offers it.
 *
 * An upper layer opens a session, sends, receives and closes it; asks for
 * a server and waits for a client. It never sees a descriptor, a socket or
 * a timeout of the platform: the shape of a session belongs to the port,
 * and the type below is opaque on purpose.
 *
 * There is no TLS, no IPv6 and no raw socket here, and that is a decision,
 * not a gap: the board is on the network of its owner, and the flash has
 * no room for a certificate store (ADR 0003 and the roadmap).
 */
#ifndef MICRO_OS_KERNEL_NET_H
#define MICRO_OS_KERNEL_NET_H

#include "kernel/types.h"

/*
 * A session and a server. The structures are not defined anywhere the
 * upper layers can see: the port owns them, these two names only say
 * "some session" and "some server".
 */
typedef struct k_socket k_socket_t;
typedef struct k_server k_server_t;

/* Open a TCP connection to host:port, NULL when it does not come up. */
k_socket_t *k_socket_connect(const char *host, k_u16_t port, k_u32_t timeout_ms);

/*
 * Send the block. `wait` false drops what does not fit now, which is what
 * the console of telnet needs: a client that stopped reading must not hold
 * the shell.
 */
int k_socket_send(k_socket_t *socket, const void *data, k_size_t size, bool wait);

/* Read at most `size` bytes: the count, 0 when the peer closed, -1 on error. */
int k_socket_recv(k_socket_t *socket, void *data, k_size_t size);

/* Close a session. NULL is not an error. */
void k_socket_close(k_socket_t *socket);

/* Listen on a TCP port; NULL when the port cannot be taken. */
k_server_t *k_server_listen(k_u16_t port);

/* Wait for a client for at most the timeout: a session, or NULL. */
k_socket_t *k_server_accept(k_server_t *server, k_u32_t timeout_ms);

/* Close a server. The sessions that are already open stay open. */
void k_server_close(k_server_t *server);

/*
 * One UDP request and one answer - the whole of the clock of NTP. The
 * size of the answer, or -1 on error.
 */
int k_net_udp_exchange(const char *host, k_u16_t port, const void *request, k_size_t request_size,
                       void *answer, k_size_t answer_size, k_u32_t timeout_ms);

#endif /* MICRO_OS_KERNEL_NET_H */
