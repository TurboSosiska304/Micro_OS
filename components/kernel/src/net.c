/*
 * Micro OS — the network, as the kernel offers it.
 *
 * The calls are the calls of the port. The types are the same underlying
 * structures (k_socket_t is k_port_socket_t under another name), so these
 * are forwarders and not a second implementation: the kernel adds the door
 * and no meaning, exactly as it does for the store of the configuration.
 */
#include "kernel/net.h"

#include "kernel/port.h"

k_socket_t *k_socket_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    return k_port_net_connect(host, port, timeout_ms);
}

int k_socket_send(k_socket_t *socket, const void *data, k_size_t size, bool wait)
{
    return k_port_net_send(socket, data, size, wait);
}

int k_socket_recv(k_socket_t *socket, void *data, k_size_t size)
{
    return k_port_net_recv(socket, data, size);
}

void k_socket_close(k_socket_t *socket)
{
    k_port_net_close(socket);
}

k_server_t *k_server_listen(k_u16_t port)
{
    return k_port_net_listen(port);
}

k_socket_t *k_server_accept(k_server_t *server, k_u32_t timeout_ms)
{
    return k_port_net_accept(server, timeout_ms);
}

void k_server_close(k_server_t *server)
{
    k_port_net_server_close(server);
}

int k_net_udp_exchange(const char *host, k_u16_t port, const void *request, k_size_t request_size,
                       void *answer, k_size_t answer_size, k_u32_t timeout_ms)
{
    return k_port_net_udp_exchange(host, port, request, request_size, answer, answer_size,
                                   timeout_ms);
}
