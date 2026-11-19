/*
 * Micro OS — platform port: the network.
 *
 * The board speaks TCP and UDP and nothing else: no TLS (the flash has no
 * room for a certificate store and the user is on a home network), no
 * IPv6, no raw sockets. This is the whole of the network contract, and it
 * is deliberately narrow: a request and an answer, a session of telnet.
 *
 * The addresses are names: the port resolves them with its own stack
 * (lwIP on the ESP32, the resolver of the process on the host).
 */
#ifndef MICRO_OS_KERNEL_PORT_NET_H
#define MICRO_OS_KERNEL_PORT_NET_H

#include "kernel/types.h"

/* The socket of a session, and the socket that waits for the sessions. */
typedef struct k_port_socket k_port_socket_t;
typedef struct k_port_server k_port_server_t;

/*
 * Open a TCP connection to host:port. Returns NULL when the name does not
 * resolve or the connection does not come up within the timeout.
 */
k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms);

/*
 * Send the whole block. `wait` false means "take what fits now and drop the
 * rest": the console of telnet must not stall because a client stopped
 * reading. Returns the bytes sent, or -1.
 */
int k_port_net_send(k_port_socket_t *socket, const void *data, k_size_t size, bool wait);

/*
 * Read at most `size` bytes: the count read, 0 when the peer closed the
 * session, -1 on an error or a timeout.
 */
int k_port_net_recv(k_port_socket_t *socket, void *data, k_size_t size);

/* Close a session. NULL is not an error. */
void k_port_net_close(k_port_socket_t *socket);

/* Open a TCP server on a port. NULL when the port cannot be taken. */
k_port_server_t *k_port_net_listen(k_u16_t port);

/* Wait for a client for at most the timeout: a session, or NULL. */
k_port_socket_t *k_port_net_accept(k_port_server_t *server, k_u32_t timeout_ms);

/* Close the server. The open sessions stay open. */
void k_port_net_server_close(k_port_server_t *server);

/*
 * One UDP request and one answer, which is the whole of NTP: the packet
 * goes to host:port and the size of the answer comes back, -1 on error.
 */
int k_port_net_udp_exchange(const char *host, k_u16_t port, const void *request,
                            k_size_t request_size, void *answer, k_size_t answer_size,
                            k_u32_t timeout_ms);

#endif /* MICRO_OS_KERNEL_PORT_NET_H */
