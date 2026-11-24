/*
 * Micro OS — the address nobody typed: a client of DHCP (RFC 2131).
 *
 * The protocol part of the client is here and the wire is not: the file
 * writes the four messages of the exchange - DISCOVER, OFFER, REQUEST, ACK -
 * into a buffer and hands each one to a transport of the caller, which is the
 * stack of the port on the machine and a scripted server in the test. Nothing
 * of this file knows a socket, a frame or a port.
 *
 * The exchange is the one of a client that has no address at all when it
 * starts: a broadcast DISCOVER from 0.0.0.0:68 to 255.255.255.255:67, the
 * OFFER of a server, a REQUEST that names the address and the server, and the
 * ACK that makes the lease true. The broadcast flag in the message asks the
 * server to answer with a broadcast frame, because a client without an
 * address cannot be reached any other way.
 *
 * The lease lives as long as the server says (option 51) and is renewed
 * before it ends: at T1 (half of it, option 58) the client asks the same
 * server for the same address, and at T2 (seven eighths, option 59) it stops
 * being polite and broadcasts for any server at all. The numbers below are
 * the answer of the protocol to a lease that named neither.
 */
#ifndef MICRO_OS_NET_DHCP_H
#define MICRO_OS_NET_DHCP_H

#include "kernel/types.h"

/* The doors of the protocol: the client asks on 68, the server answers on 67. */
#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u

/* The fixed part of a message (RFC 2131 §2) and the room of the options the
 * client writes. A DHCP message is a BOOTP one with a cookie and options. */
#define DHCP_MESSAGE_MIN 240u
#define DHCP_MESSAGE_MAX 576u

/* A lease: the address, the way out of the subnet, the resolver and the
 * server, with the moments of its life in seconds from the ACK. */
struct dhcp_lease {
    k_u32_t address;
    k_u32_t mask;
    k_u32_t gateway;
    k_u32_t dns;
    k_u32_t server;
    k_u32_t lease_s;
    k_u32_t t1_s;
    k_u32_t t2_s;
    k_u64_t obtained_ms; /* the clock of the caller when the ACK came */
};

/* The answer of one exchange. */
enum dhcp_result {
    DHCP_OK = 0,      /* an ACK: the lease is filled and applied */
    DHCP_TIMEOUT = 1, /* no server said anything in the budget */
    DHCP_REFUSED = 2, /* a NAK: the server will not give this address */
    DHCP_ERROR = 3,   /* the transport could not send or the answer is not DHCP */
};

/*
 * The wire, as the client sees it. `send` hands one datagram to the network;
 * `receive` waits up to the timeout for one and returns its size, 0 on
 * timeout and -1 on error; `now_ms` is the clock the budget and the lease are
 * measured with (the port's uptime in a machine, a counter in the test);
 * `apply` is called once, with the lease, when the ACK of a server has made
 * it true.
 */
struct dhcp_transport {
    bool (*send)(void *context, const void *datagram, k_size_t size);
    int (*receive)(void *context, void *datagram, k_size_t size, k_u32_t timeout_ms);
    k_u64_t (*now_ms)(void *context);
    void (*apply)(void *context, const struct dhcp_lease *lease);
    void *context;
};

/*
 * One full exchange, in the budget given (a few seconds is a lifetime on a
 * wire and the boot cannot wait for more). On DHCP_OK the lease is filled,
 * applied through the transport and `obtained_ms` is the moment of the ACK.
 */
enum dhcp_result dhcp_client_acquire(const struct dhcp_transport *transport, const k_u8_t mac[6],
                                     k_u32_t xid, k_u32_t timeout_ms, struct dhcp_lease *lease);

/*
 * The renewal of a lease: a REQUEST that names its address and the server
 * that gave it, and the ACK that moves the moments of the lease. The address
 * of the lease is the one the caller keeps using while the answer is not
 * here, so a renewal that times out is not an address lost.
 */
enum dhcp_result dhcp_client_renew(const struct dhcp_transport *transport, const k_u8_t mac[6],
                                   const struct dhcp_lease *current, k_u32_t xid,
                                   k_u32_t timeout_ms, struct dhcp_lease *lease);

/* T1 and T2 of a lease: what the server named, or half and seven eighths of
 * the lease when it named nothing. Zero when the lease has no end. */
k_u32_t dhcp_t1(const struct dhcp_lease *lease);
k_u32_t dhcp_t2(const struct dhcp_lease *lease);

/* True when the lease asks for its renewal (T1) or for any server (T2). */
bool dhcp_renew_due(const struct dhcp_lease *lease, k_u64_t now_ms);
bool dhcp_rebind_due(const struct dhcp_lease *lease, k_u64_t now_ms);

/*
 * The transaction id of an exchange: a mix of the MAC of the card, the clock
 * and a counter, so that two exchanges of one machine and the exchanges of
 * two machines of a network do not share an id.
 */
k_u32_t dhcp_xid(const k_u8_t mac[6], k_u64_t now_ms, k_u32_t counter);

#endif /* MICRO_OS_NET_DHCP_H */
