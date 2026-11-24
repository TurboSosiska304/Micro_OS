/*
 * Micro OS — the client of DHCP, the portable half (net/dhcp.h).
 *
 * Four messages and a lease, in C11 that knows nothing of a socket: the
 * caller gives the datagrams to a transport and the clock to measure them
 * with, and this file decides what to write and what an answer means. That is
 * what lets the same client run over the stack of the x86-64 port, over lwIP
 * on the board and against a scripted server in the tests of the host.
 *
 * The message is the one of RFC 2131 §2: a BOOTP request with the magic
 * cookie and options, and the few options a client of this stack needs -
 * the type, the address it asks for, the server it asks, the mask, the
 * router, the resolver and the three numbers of the lease's life. A DHCP
 * message is at most 576 bytes and the client writes 240 plus a dozen.
 */
#include "net/dhcp.h"

#include <string.h>

/* --- the bytes of the message ---------------------------------------------- */

#define DHCP_OP_REQUEST 1u
#define DHCP_OP_REPLY 2u
#define DHCP_HTYPE_ETHERNET 1u
#define DHCP_HLEN_ETHERNET 6u
#define DHCP_FLAG_BROADCAST 0x8000u

#define DHCP_OFF_OP 0u
#define DHCP_OFF_HTYPE 1u
#define DHCP_OFF_HLEN 2u
#define DHCP_OFF_XID 4u
#define DHCP_OFF_FLAGS 10u
#define DHCP_OFF_CIADDR 12u
#define DHCP_OFF_YIADDR 16u
#define DHCP_OFF_SIADDR 20u
#define DHCP_OFF_CHADDR 28u
#define DHCP_OFF_COOKIE 236u
#define DHCP_OFF_OPTIONS 240u

static const k_u8_t g_dhcp_cookie[4] = {99u, 130u, 83u, 99u};

/* --- the options and the types -------------------------------------------- */

#define DHCP_OPTION_PAD 0u
#define DHCP_OPTION_SUBNET 1u
#define DHCP_OPTION_ROUTER 3u
#define DHCP_OPTION_DNS 6u
#define DHCP_OPTION_REQUESTED 50u
#define DHCP_OPTION_LEASE 51u
#define DHCP_OPTION_TYPE 53u
#define DHCP_OPTION_SERVER 54u
#define DHCP_OPTION_PARAMS 55u
#define DHCP_OPTION_T1 58u
#define DHCP_OPTION_T2 59u
#define DHCP_OPTION_END 255u

#define DHCP_DISCOVER 1u
#define DHCP_OFFER 2u
#define DHCP_REQUEST 3u
#define DHCP_ACK 5u
#define DHCP_NAK 6u

/* The default mask of a lease whose server named none: a /24 is what a home
 * network is, and the option is one the servers of this century send. */
#define DHCP_DEFAULT_MASK 0xFFFFFF00u

/* How long one look at the wire is: the wait of an answer is cut into pieces
 * so that the budget of the exchange is checked against the clock of the
 * caller and not against a count of timeouts. */
#define DHCP_POLL_MS 100u

/* The attempts of a DISCOVER inside one budget: one lost datagram is a wire,
 * three lost ones are a network without a server. */
#define DHCP_ATTEMPTS 3u

/* --- the bytes of an address ----------------------------------------------- */

static k_u32_t dhcp_get32(const k_u8_t *data)
{
    return ((k_u32_t)data[0] << 24) | ((k_u32_t)data[1] << 16) | ((k_u32_t)data[2] << 8) |
           (k_u32_t)data[3];
}

static void dhcp_put16(k_u8_t *data, k_u16_t value)
{
    data[0] = (k_u8_t)(value >> 8);
    data[1] = (k_u8_t)(value & 0xFFu);
}

static void dhcp_put32(k_u8_t *data, k_u32_t value)
{
    data[0] = (k_u8_t)(value >> 24);
    data[1] = (k_u8_t)((value >> 16) & 0xFFu);
    data[2] = (k_u8_t)((value >> 8) & 0xFFu);
    data[3] = (k_u8_t)(value & 0xFFu);
}

/* --- what goes out --------------------------------------------------------- */

/*
 * One message: a DISCOVER, or a REQUEST that asks for an address from a
 * server. The request is the one of a client that was offered the address
 * (options 50 and 54); a renewal uses the same shape, which is the REBINDING
 * of RFC 2131 §4.3.2 and is accepted by the servers that would answer a
 * unicast renewal too - and a client of this stack cannot be sure that its
 * address is still routable enough to be reached at.
 */
static k_size_t dhcp_build(k_u8_t *message, k_u8_t type, k_u32_t xid, const k_u8_t mac[6],
                           const struct dhcp_lease *asked)
{
    k_size_t at = DHCP_OFF_OPTIONS;

    memset(message, 0, DHCP_MESSAGE_MIN);

    message[DHCP_OFF_OP] = DHCP_OP_REQUEST;
    message[DHCP_OFF_HTYPE] = DHCP_HTYPE_ETHERNET;
    message[DHCP_OFF_HLEN] = DHCP_HLEN_ETHERNET;
    dhcp_put32(message + DHCP_OFF_XID, xid);
    dhcp_put16(message + DHCP_OFF_FLAGS, DHCP_FLAG_BROADCAST);
    memcpy(message + DHCP_OFF_CHADDR, mac, 6);
    memcpy(message + DHCP_OFF_COOKIE, g_dhcp_cookie, sizeof(g_dhcp_cookie));

    message[at++] = DHCP_OPTION_TYPE;
    message[at++] = 1u;
    message[at++] = type;

    if (type == DHCP_REQUEST && asked != NULL) {
        message[at++] = DHCP_OPTION_REQUESTED;
        message[at++] = 4u;
        dhcp_put32(message + at, asked->address);
        at += 4u;

        if (asked->server != 0u) {
            message[at++] = DHCP_OPTION_SERVER;
            message[at++] = 4u;
            dhcp_put32(message + at, asked->server);
            at += 4u;
        }
    }

    /* What the client would like to be told: the mask, the way out, the
     * resolver and the life of the lease. */
    message[at++] = DHCP_OPTION_PARAMS;
    message[at++] = 6u;
    message[at++] = DHCP_OPTION_SUBNET;
    message[at++] = DHCP_OPTION_ROUTER;
    message[at++] = DHCP_OPTION_DNS;
    message[at++] = DHCP_OPTION_LEASE;
    message[at++] = DHCP_OPTION_T1;
    message[at++] = DHCP_OPTION_T2;

    message[at++] = DHCP_OPTION_END;

    return at;
}

/* --- what comes in --------------------------------------------------------- */

/*
 * The options of one answer, and whether it is the one waited for. The answer
 * of a server is a BOOTP reply with the transaction id of the request and the
 * hardware address of the client: a datagram of another exchange on the same
 * network is not an answer, and neither is a frame of another client.
 */
struct dhcp_answer {
    k_u8_t type;
    struct dhcp_lease lease;
    bool parsed;
};

static bool dhcp_parse(const k_u8_t *message, k_size_t length, k_u32_t xid, const k_u8_t mac[6],
                       struct dhcp_answer *answer)
{
    k_size_t at;
    bool type_seen = false;
    bool end_seen = false;

    if (length < DHCP_MESSAGE_MIN || message[DHCP_OFF_OP] != DHCP_OP_REPLY ||
        message[DHCP_OFF_HTYPE] != DHCP_HTYPE_ETHERNET ||
        message[DHCP_OFF_HLEN] != DHCP_HLEN_ETHERNET || dhcp_get32(message + DHCP_OFF_XID) != xid ||
        memcmp(message + DHCP_OFF_CHADDR, mac, 6) != 0 ||
        memcmp(message + DHCP_OFF_COOKIE, g_dhcp_cookie, sizeof(g_dhcp_cookie)) != 0) {
        return false;
    }

    memset(&answer->lease, 0, sizeof(answer->lease));
    answer->lease.address = dhcp_get32(message + DHCP_OFF_YIADDR);

    at = DHCP_OFF_OPTIONS;

    while (at < length) {
        k_u8_t option = message[at++];
        k_u8_t size;

        if (option == DHCP_OPTION_PAD) {
            continue;
        }

        if (option == DHCP_OPTION_END) {
            end_seen = true;
            break;
        }

        if (at >= length) {
            break;
        }

        size = message[at++];

        if ((k_size_t)size > length - at) {
            break;
        }

        switch (option) {
        case DHCP_OPTION_TYPE:
            if (size >= 1u) {
                answer->type = message[at];
                type_seen = true;
            }
            break;
        case DHCP_OPTION_SERVER:
            if (size >= 4u) {
                answer->lease.server = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_SUBNET:
            if (size >= 4u) {
                answer->lease.mask = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_ROUTER:
            if (size >= 4u) {
                answer->lease.gateway = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_DNS:
            if (size >= 4u) {
                answer->lease.dns = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_LEASE:
            if (size >= 4u) {
                answer->lease.lease_s = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_T1:
            if (size >= 4u) {
                answer->lease.t1_s = dhcp_get32(message + at);
            }
            break;
        case DHCP_OPTION_T2:
            if (size >= 4u) {
                answer->lease.t2_s = dhcp_get32(message + at);
            }
            break;
        default:
            break;
        }

        at += size;
    }

    if (!end_seen && at < length) {
        return false;
    }

    answer->parsed = type_seen;
    answer->lease.obtained_ms = 0u;

    return type_seen;
}

/*
 * Wait for one of the two types of answer that end a step of the exchange.
 * The wait is a loop of short looks: a datagram of another exchange is
 * dropped with no effect, and a budget that ran out is a TIMEOUT.
 */
static enum dhcp_result dhcp_wait(const struct dhcp_transport *transport, k_u32_t xid,
                                  const k_u8_t mac[6], k_u8_t wanted, k_u8_t refusal,
                                  k_u64_t deadline, struct dhcp_answer *answer)
{
    k_u8_t message[DHCP_MESSAGE_MAX];

    for (;;) {
        k_u64_t now = transport->now_ms(transport->context);
        k_u32_t slice;
        int size;

        if (now >= deadline) {
            return DHCP_TIMEOUT;
        }

        slice = deadline - now > DHCP_POLL_MS ? DHCP_POLL_MS : (k_u32_t)(deadline - now);
        size = transport->receive(transport->context, message, sizeof(message), slice);

        if (size < 0) {
            return DHCP_ERROR;
        }

        if (size == 0) {
            continue;
        }

        if (!dhcp_parse(message, (k_size_t)size, xid, mac, answer)) {
            continue;
        }

        if (answer->type == wanted) {
            return DHCP_OK;
        }

        if (refusal != 0u && answer->type == refusal) {
            return DHCP_REFUSED;
        }
    }
}

/* --- the two exchanges ----------------------------------------------------- */

static k_size_t dhcp_send_step(const struct dhcp_transport *transport, k_u8_t type, k_u32_t xid,
                               const k_u8_t mac[6], const struct dhcp_lease *asked)
{
    k_u8_t message[DHCP_MESSAGE_MAX];
    k_size_t size = dhcp_build(message, type, xid, mac, asked);

    if (!transport->send(transport->context, message, size)) {
        return 0u;
    }

    return size;
}

enum dhcp_result dhcp_client_acquire(const struct dhcp_transport *transport, const k_u8_t mac[6],
                                     k_u32_t xid, k_u32_t timeout_ms, struct dhcp_lease *lease)
{
    struct dhcp_lease offered;
    k_u64_t started;
    k_u32_t attempt;

    if (transport == NULL || transport->send == NULL || transport->receive == NULL ||
        transport->now_ms == NULL || transport->apply == NULL || mac == NULL || lease == NULL ||
        timeout_ms == 0u) {
        return DHCP_ERROR;
    }

    started = transport->now_ms(transport->context);

    for (attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
        struct dhcp_answer answer;
        k_u64_t deadline;
        k_u64_t now;
        k_u32_t left;

        now = transport->now_ms(transport->context);

        if (now >= started + timeout_ms) {
            break;
        }

        /* The budget is cut between the attempts that are left, so a wire
         * that lost one DISCOVER still has time to answer the next. */
        left = (k_u32_t)(started + timeout_ms - now);
        deadline = now + left / (DHCP_ATTEMPTS - attempt);

        if (dhcp_send_step(transport, DHCP_DISCOVER, xid, mac, NULL) == 0u) {
            continue;
        }

        if (dhcp_wait(transport, xid, mac, DHCP_OFFER, 0u, deadline, &answer) != DHCP_OK) {
            continue;
        }

        offered = answer.lease;
        offered.mask = offered.mask != 0u ? offered.mask : DHCP_DEFAULT_MASK;

        if (dhcp_send_step(transport, DHCP_REQUEST, xid, mac, &offered) == 0u) {
            continue;
        }

        switch (dhcp_wait(transport, xid, mac, DHCP_ACK, DHCP_NAK, deadline, &answer)) {
        case DHCP_OK:
            *lease = answer.lease;
            /* The ACK may repeat the lease and it may not; either way the
             * transaction is over and the address is the one asked for. */
            lease->address = answer.lease.address != 0u ? answer.lease.address : offered.address;
            lease->server = answer.lease.server != 0u ? answer.lease.server : offered.server;
            lease->mask = answer.lease.mask != 0u ? answer.lease.mask : offered.mask;
            lease->gateway = answer.lease.gateway != 0u ? answer.lease.gateway : offered.gateway;
            lease->dns = answer.lease.dns != 0u ? answer.lease.dns : offered.dns;
            lease->lease_s = answer.lease.lease_s != 0u ? answer.lease.lease_s : offered.lease_s;
            lease->t1_s = answer.lease.t1_s != 0u ? answer.lease.t1_s : offered.t1_s;
            lease->t2_s = answer.lease.t2_s != 0u ? answer.lease.t2_s : offered.t2_s;
            lease->obtained_ms = transport->now_ms(transport->context);
            transport->apply(transport->context, lease);

            return DHCP_OK;
        case DHCP_REFUSED:
            return DHCP_REFUSED;
        default:
            /* No ACK of this server: the next attempt starts the exchange
             * again, because the address of the offer is not ours. */
            break;
        }
    }

    return DHCP_TIMEOUT;
}

enum dhcp_result dhcp_client_renew(const struct dhcp_transport *transport, const k_u8_t mac[6],
                                   const struct dhcp_lease *current, k_u32_t xid,
                                   k_u32_t timeout_ms, struct dhcp_lease *lease)
{
    struct dhcp_answer answer;
    struct dhcp_lease asked;
    k_u64_t deadline;

    if (transport == NULL || transport->send == NULL || transport->receive == NULL ||
        transport->now_ms == NULL || transport->apply == NULL || mac == NULL || current == NULL ||
        lease == NULL || timeout_ms == 0u || current->address == 0u) {
        return DHCP_ERROR;
    }

    asked = *current;

    if (dhcp_send_step(transport, DHCP_REQUEST, xid, mac, &asked) == 0u) {
        return DHCP_ERROR;
    }

    deadline = transport->now_ms(transport->context) + timeout_ms;

    switch (dhcp_wait(transport, xid, mac, DHCP_ACK, DHCP_NAK, deadline, &answer)) {
    case DHCP_OK:
        *lease = *current;
        lease->address = answer.lease.address != 0u ? answer.lease.address : current->address;
        lease->server = answer.lease.server != 0u ? answer.lease.server : current->server;
        lease->mask = answer.lease.mask != 0u ? answer.lease.mask : current->mask;
        lease->gateway = answer.lease.gateway != 0u ? answer.lease.gateway : current->gateway;
        lease->dns = answer.lease.dns != 0u ? answer.lease.dns : current->dns;
        lease->lease_s = answer.lease.lease_s != 0u ? answer.lease.lease_s : current->lease_s;
        lease->t1_s = answer.lease.t1_s != 0u ? answer.lease.t1_s : 0u;
        lease->t2_s = answer.lease.t2_s != 0u ? answer.lease.t2_s : 0u;
        lease->obtained_ms = transport->now_ms(transport->context);
        transport->apply(transport->context, lease);

        return DHCP_OK;
    case DHCP_REFUSED:
        return DHCP_REFUSED;
    case DHCP_ERROR:
        return DHCP_ERROR;
    default:
        return DHCP_TIMEOUT;
    }
}

/* --- the life of a lease --------------------------------------------------- */

k_u32_t dhcp_t1(const struct dhcp_lease *lease)
{
    if (lease == NULL || lease->lease_s == 0u) {
        return 0u;
    }

    if (lease->t1_s != 0u && lease->t1_s < lease->lease_s) {
        return lease->t1_s;
    }

    return lease->lease_s / 2u;
}

k_u32_t dhcp_t2(const struct dhcp_lease *lease)
{
    if (lease == NULL || lease->lease_s == 0u) {
        return 0u;
    }

    if (lease->t2_s != 0u && lease->t2_s < lease->lease_s) {
        return lease->t2_s;
    }

    return (lease->lease_s / 8u) * 7u;
}

bool dhcp_renew_due(const struct dhcp_lease *lease, k_u64_t now_ms)
{
    k_u32_t t1;

    if (lease == NULL || lease->lease_s == 0u) {
        return false;
    }

    t1 = dhcp_t1(lease);

    return now_ms >= lease->obtained_ms + (k_u64_t)t1 * 1000u;
}

bool dhcp_rebind_due(const struct dhcp_lease *lease, k_u64_t now_ms)
{
    k_u32_t t2;

    if (lease == NULL || lease->lease_s == 0u) {
        return false;
    }

    t2 = dhcp_t2(lease);

    return now_ms >= lease->obtained_ms + (k_u64_t)t2 * 1000u;
}

k_u32_t dhcp_xid(const k_u8_t mac[6], k_u64_t now_ms, k_u32_t counter)
{
    k_u32_t value = 0x4D4F5300u; /* "MOS" and a zero */
    k_size_t i;

    for (i = 0; i < 6u; i++) {
        value = (value * 33u) ^ (k_u32_t)mac[i];
    }

    value ^= (k_u32_t)(now_ms >> 3);
    value ^= counter * 2654435761u;

    /* Zero is the xid of the messages of a BOOTP client that has none, and a
     * transaction of this client is never that. */
    return value != 0u ? value : 1u;
}
