/*
 * Micro OS — the network of the ESP32: the sockets of lwIP.
 *
 * This file is the port side of kernel/port/net.h.
 *
 * role-backends-net: the body is filled by the portability work; until
 * then every call answers "no network", which is what a board without a
 * stack does.
 */
#include "kernel/port.h"

k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    K_UNUSED(host);
    K_UNUSED(port);
    K_UNUSED(timeout_ms);
    return NULL;
}

int k_port_net_send(k_port_socket_t *socket, const void *data, k_size_t size, bool wait)
{
    K_UNUSED(socket);
    K_UNUSED(data);
    K_UNUSED(size);
    K_UNUSED(wait);
    return -1;
}

int k_port_net_recv(k_port_socket_t *socket, void *data, k_size_t size)
{
    K_UNUSED(socket);
    K_UNUSED(data);
    K_UNUSED(size);
    return -1;
}

void k_port_net_close(k_port_socket_t *socket)
{
    K_UNUSED(socket);
}

k_port_server_t *k_port_net_listen(k_u16_t port)
{
    K_UNUSED(port);
    return NULL;
}

k_port_socket_t *k_port_net_accept(k_port_server_t *server, k_u32_t timeout_ms)
{
    K_UNUSED(server);
    K_UNUSED(timeout_ms);
    return NULL;
}

void k_port_net_server_close(k_port_server_t *server)
{
    K_UNUSED(server);
}

int k_port_net_udp_exchange(const char *host, k_u16_t port, const void *request,
                            k_size_t request_size, void *answer, k_size_t answer_size,
                            k_u32_t timeout_ms)
{
    K_UNUSED(host);
    K_UNUSED(port);
    K_UNUSED(request);
    K_UNUSED(request_size);
    K_UNUSED(answer);
    K_UNUSED(answer_size);
    K_UNUSED(timeout_ms);
    return -1;
}
