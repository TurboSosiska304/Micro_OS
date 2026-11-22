/*
 * Micro OS — the image without TLS.
 *
 * The HTTP client knows `https` as a scheme whatever the image carries: the
 * URL parser is the same on both machines (net/http.h), and a build that
 * leaves mbedTLS out answers NULL here. The client then refuses the URL
 * with the reason of this file instead of linking a layer that is not
 * there.
 *
 * The board of 1.x is one such image: it keeps the copy of ESP-IDF and no
 * command of its shell speaks TLS yet (ADR 0018, the compatibility port).
 */
#include "net/tls.h"

const struct http_transport *net_tls_transport(void)
{
    return NULL;
}

const char *net_tls_last_error(void)
{
    return "TLS is not in this image";
}
