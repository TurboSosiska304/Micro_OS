/*
 * Micro OS — the page of the state over HTTP.
 *
 * A server of the board for the network of its owner: plain HTTP, no TLS and
 * no authentication, two addresses and no more. `/` is the page a person
 * reads (the version, the uptime, the memory, the tasks) and `/health` is the
 * word a monitor reads, so that neither has to parse the other.
 *
 * The server is not started by the kernel: whether a machine of this size
 * opens a port is a decision of the platform, and the call below is the door
 * for init or for a program. The sockets are the ones of the kernel
 * (kernel/net.h), so the same server runs over lwIP on the board and over the
 * sockets of the host under the tests.
 */
#ifndef MICRO_OS_NET_NET_HTTP_H
#define MICRO_OS_NET_NET_HTTP_H

#include "kernel/types.h"

/* The port of a page of the web, and the one the server takes by default. */
#define NET_HTTP_PORT 80

/*
 * Start the server on `port` (0 is refused). Returns 0 on success and -1 when
 * the port belongs to somebody else or the task of the server cannot be made.
 * A server that is already on keeps its port and answers 0: a script of init
 * may name it twice.
 */
int net_http_server_start(k_u16_t port);

/*
 * Close the door. The request that is being answered is finished first, and
 * the call returns once the task of the server has closed the listener; the
 * wait is bounded, because a client that stopped reading must not hold init.
 */
void net_http_server_stop(void);

/* Whether the task of the server runs and the door is open. */
bool net_http_server_running(void);

/* The port of a running server: the one it was started on, 0 when it is
 * not running. A caller that repeats `start` reads nothing else. */
k_u16_t net_http_server_port(void);

#endif /* MICRO_OS_NET_NET_HTTP_H */
