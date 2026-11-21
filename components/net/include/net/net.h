/*
 * Micro OS — the network services of the board: HTTP, NTP and MQTT.
 *
 * The protocol parts (the URL, the request, the packet of NTP, the page of
 * the state, the packet of MQTT) live in http.c, ntp.c, net_http_server.c
 * and mqtt.c and are tested on the host; this file is the four commands of
 * the shell, and the sockets under them belong to the kernel
 * (kernel/net.h), which lends them from the port. Nothing here knows which
 * stack resolved the name.
 */
#ifndef MICRO_OS_NET_NET_H
#define MICRO_OS_NET_NET_H

#include "kernel/types.h"
#include "net/http.h"

/*
 * The milliseconds one exchange spends before it gives up. The board sits
 * on a home network: a command of the shell has to answer even when that
 * network, or the server of the moment, has gone away.
 */
#define NET_TIMEOUT_MS 5000

/* Register the commands `http`, `ntp`, `httpd` and `mqtt`. */
int net_register(void);

/* GET a URL through the sockets of the port. */
int net_http_get(const char *url, char *body, k_size_t size, struct http_response *out);

/* The time of an NTP server, in the seconds of Unix (UTC). */
int net_ntp_sync(const char *server, k_u64_t *unix_seconds);

#endif /* MICRO_OS_NET_NET_H */
