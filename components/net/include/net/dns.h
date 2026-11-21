/*
 * Micro OS — the resolver, as arithmetic and a small cache.
 *
 * A query is one name and one A record: the packet of 1987, a header, the
 * labels of the name, the root and the four bytes of the question. The answer
 * is read for its first record of type A and its time to live; everything
 * else - a CNAME on the way, a record of another type, the authority of the
 * server - is skipped, because this machine asks for an address and nothing
 * more.
 *
 * The cache keeps a handful of names for as long as the server said, with the
 * moment of the answer as the only clock: the caller passes "now" in, so the
 * arithmetic is the same under the tests and on a machine, and nothing here
 * reads a timer or opens a socket. The port does the exchange (the UDP
 * contract of kernel/port/net.h) and stores what was read.
 *
 * The type of an address is the one of the stack: the four bytes of the
 * record in the order of the machine, so that 93.184.216.34 is 0x5DB8D822
 * and the printing code is the same for a name and for a dotted quad.
 */
#ifndef MICRO_OS_NET_DNS_H
#define MICRO_OS_NET_DNS_H

#include "kernel/types.h"

#define DNS_PORT 53

/* The largest query and answer this client speaks. A name of a network of
 * this size fits many times, and a machine with 100 KB of SRAM has no room
 * for the 4 KB a server of the internet may send. */
#define DNS_PACKET_MAX 512

/* The longest name the cache keeps, including the zero. */
#define DNS_NAME_MAX 128

/* How many names are remembered: the shell, the clock and the page of the
 * state ask for a handful of hosts, and a name that fell out costs a query. */
#define DNS_CACHE_ENTRIES 8

/* A server may say a day or a week; a name of a home network is not worth
 * more of the little memory than this. */
#define DNS_TTL_MAX_S 86400u

/* The answer of one query: the address and the seconds it may be kept. */
struct dns_answer {
    k_u32_t address;
    k_u32_t ttl_s;
};

/*
 * Build the query of `name` with `identifier` in its header. Returns the
 * length of the packet, or -1 when the name is not one (empty, a label of
 * more than 63, a name the cache could not keep, or one that does not fit
 * `size`). A trailing dot is the root and is accepted.
 */
int dns_build_query(const char *name, k_u16_t identifier, void *packet, k_size_t size);

/*
 * Read the first A record of an answer. Returns 0 and fills `out` when the
 * identifier is the one of the query, the message is an answer with no error,
 * and a record of type A class IN of four bytes is there; -1 otherwise.
 */
int dns_parse_answer(const void *packet, k_size_t length, k_u16_t identifier,
                     struct dns_answer *out);

/* --- the cache of the names ---------------------------------------------- */

/* Forget every name. */
void dns_cache_clear(void);

/* How many names are in the cache, expired ones included. */
k_size_t dns_cache_count(void);

/*
 * The address of `name` when it was stored at `now_ms` less than its time to
 * live ago. `ttl_left_s` is the rest of that time (0 when the caller does not
 * ask). Returns false on a miss and on an entry that has expired.
 */
bool dns_cache_lookup(const char *name, k_u64_t now_ms, k_u32_t *address, k_u32_t *ttl_left_s);

/*
 * Keep `name` -> `address` for `ttl_s` seconds from `now_ms`, capped at
 * DNS_TTL_MAX_S. Returns 0 when the name is kept, 1 when it is not worth it
 * (a time to live of zero), -1 when the name does not fit the cache.
 */
int dns_cache_store(const char *name, k_u32_t address, k_u32_t ttl_s, k_u64_t now_ms);

/*
 * The name of a slot, for a command that prints the cache. Slots are a
 * property of the cache and not of an order: a caller walks them from 0 to
 * DNS_CACHE_ENTRIES - 1 and skips the NULL answers. An expired entry is not
 * reported, and `ttl_left_s` is the rest of its time when it is.
 */
const char *dns_cache_name(k_size_t slot, k_u64_t now_ms, k_u32_t *address, k_u32_t *ttl_left_s);

#endif /* MICRO_OS_NET_DNS_H */
