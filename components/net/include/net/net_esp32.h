/*
 * Micro OS — the network services of the board: HTTP and NTP over lwIP.
 *
 * The protocol parts (the URL, the request, the packet of NTP) live in
 * http.c and ntp.c and are tested on the host; this file is the socket under
 * them and the two commands of the shell.
 */
#ifndef MICRO_OS_NET_NET_ESP32_H
#define MICRO_OS_NET_NET_ESP32_H

#include "kernel/types.h"
#include "net/http.h"

/* Register the commands `http` and `ntp`. */
int net_register(void);

/* GET a URL over the sockets of lwIP. */
int net_http_get(const char *url, char *body, k_size_t size, struct http_response *out);

/* The time of an NTP server, in the seconds of Unix (UTC). */
int net_ntp_sync(const char *server, k_u64_t *unix_seconds);

#endif /* MICRO_OS_NET_NET_ESP32_H */
