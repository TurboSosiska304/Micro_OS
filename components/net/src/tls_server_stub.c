/*
 * Micro OS — the serving side of TLS when this image has none.
 *
 * The board keeps its copy of ESP-IDF for the client of the shell and does
 * not build third_party/mbedtls (ADR 0018): its HTTP server is plain, and a
 * command that asked for TLS says so instead of pretending. The host build
 * without MICRO_OS_TLS is the same machine for the same reason, and the tests
 * of the plain server run against this file.
 */
#include "net/tls_server.h"

bool net_tls_server_supported(void)
{
    return false;
}

int net_tls_server_open(k_socket_t *socket, void **context)
{
    (void)socket;

    if (context != NULL) {
        *context = NULL;
    }

    return -1;
}

int net_tls_server_send(void *context, const void *data, k_size_t size)
{
    (void)context;
    (void)data;
    (void)size;

    return -1;
}

int net_tls_server_recv(void *context, void *data, k_size_t size)
{
    (void)context;
    (void)data;
    (void)size;

    return -1;
}

void net_tls_server_close(void *context)
{
    (void)context;
}

const char *net_tls_server_last_error(void)
{
    return "this image has no serving side of TLS";
}

int net_tls_server_set_pair(const char *certificate, const char *key)
{
    (void)certificate;
    (void)key;

    return -1;
}
