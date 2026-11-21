/*
 * Micro OS — the resolver: the packet of a query, the record of an answer and
 * a cache of the names the machine asked for.
 *
 * Nothing here knows a socket. The port builds a query, hands it to the UDP
 * exchange of its own stack, reads the answer with dns_parse_answer() and
 * keeps it, so the same parser serves the x86-64 port and the tests, and the
 * tests of the wire format need no network at all.
 */
#include "net/dns.h"

#include <string.h>

static k_u16_t dns_get_be16(const k_u8_t *p)
{
    return (k_u16_t)(((k_u16_t)p[0] << 8) | (k_u16_t)p[1]);
}

static k_u32_t dns_get_be32(const k_u8_t *p)
{
    return ((k_u32_t)p[0] << 24) | ((k_u32_t)p[1] << 16) | ((k_u32_t)p[2] << 8) | (k_u32_t)p[3];
}

static void dns_put_be16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value >> 8);
    p[1] = (k_u8_t)(value & 0xFFu);
}

/* --- the query ------------------------------------------------------------ */

/* A name of the cache is a name of a query: the same bytes and the same
 * limit, so a query that cannot be remembered is not asked either. */
static bool dns_name_too_long(const char *name)
{
    return strlen(name) >= DNS_NAME_MAX;
}

int dns_build_query(const char *name, k_u16_t identifier, void *packet, k_size_t size)
{
    k_u8_t *bytes = packet;
    const char *cursor;
    k_size_t length = 12u;

    if (name == NULL || packet == NULL || name[0] == '\0' || dns_name_too_long(name) ||
        size < length) {
        return -1;
    }

    /* The header: the identifier of the exchange, a query with recursion
     * wanted, one question and none of the three sections that follow. */
    dns_put_be16(bytes + 0, identifier);
    dns_put_be16(bytes + 2, 0x0100u);
    dns_put_be16(bytes + 4, 1u);
    dns_put_be16(bytes + 6, 0u);
    dns_put_be16(bytes + 8, 0u);
    dns_put_be16(bytes + 10, 0u);

    /* The name as labels: a length byte, the characters, and a zero for the
     * root. A trailing dot is the root label and not an empty one; an empty
     * label in the middle is a name this client does not build. */
    cursor = name;

    while (*cursor != '\0') {
        k_size_t label = length++;
        k_size_t count = 0;

        while (*cursor != '\0' && *cursor != '.') {
            if (count >= 63u || length + 6u >= size) {
                return -1;
            }

            bytes[length++] = (k_u8_t)*cursor++;
            count++;
        }

        if (count == 0u) {
            return -1;
        }

        bytes[label] = (k_u8_t)count;

        if (*cursor == '.') {
            cursor++;
        }
    }

    if (length + 5u > size) {
        return -1;
    }

    bytes[length++] = 0u;
    dns_put_be16(bytes + length, 1u);      /* the type: an address */
    dns_put_be16(bytes + length + 2u, 1u); /* the class: the internet */
    length += 4u;

    return (int)length;
}

/* --- the answer ----------------------------------------------------------- */

/*
 * Skip a name of a message: a label and its bytes, and a pointer of two
 * bytes in place of the rest. Nothing is resolved here - a pointer is only
 * skipped - so a message that points at itself costs two bytes and not a
 * turn of the parser.
 */
static bool dns_skip_name(const k_u8_t *packet, k_size_t length, k_size_t *offset)
{
    while (*offset < length) {
        k_u8_t size = packet[*offset];

        if (size == 0u) {
            (*offset)++;
            return true;
        }

        if ((size & 0xC0u) == 0xC0u) {
            *offset += 2u;
            return *offset <= length;
        }

        *offset += 1u + size;
    }

    return false;
}

int dns_parse_answer(const void *packet, k_size_t length, k_u16_t identifier,
                     struct dns_answer *out)
{
    const k_u8_t *bytes = packet;
    k_size_t offset = 12u;
    k_u16_t questions;
    k_u16_t answers;
    k_u16_t flags;
    k_size_t i;

    if (packet == NULL || out == NULL || length < 12u || length > DNS_PACKET_MAX) {
        return -1;
    }

    if (dns_get_be16(bytes) != identifier) {
        return -1;
    }

    /* A query is not an answer, and a name that does not exist is not a
     * record: the bit of the answer and the code of the error say both. */
    flags = dns_get_be16(bytes + 2);

    if ((flags & 0x8000u) == 0u || (flags & 0x000Fu) != 0u) {
        return -1;
    }

    questions = dns_get_be16(bytes + 4);
    answers = dns_get_be16(bytes + 6);

    for (i = 0; i < questions; i++) {
        if (!dns_skip_name(bytes, length, &offset) || offset + 4u > length) {
            return -1;
        }

        offset += 4u;
    }

    /*
     * The records: the name, the type, the class, the time to live and the
     * data. The first address of the internet is the answer; a CNAME of the
     * server on the way is skipped like any other record.
     */
    for (i = 0; i < answers; i++) {
        k_u16_t type;
        k_u16_t class_code;
        k_u32_t ttl;
        k_u16_t data_length;

        if (!dns_skip_name(bytes, length, &offset) || offset + 10u > length) {
            return -1;
        }

        type = dns_get_be16(bytes + offset);
        class_code = dns_get_be16(bytes + offset + 2u);
        ttl = dns_get_be32(bytes + offset + 4u);
        data_length = dns_get_be16(bytes + offset + 8u);
        offset += 10u;

        if (offset + data_length > length) {
            return -1;
        }

        if (type == 1u && class_code == 1u && data_length == 4u) {
            out->address = dns_get_be32(bytes + offset);
            out->ttl_s = ttl > DNS_TTL_MAX_S ? DNS_TTL_MAX_S : ttl;

            return 0;
        }

        offset += data_length;
    }

    return -1;
}

/* --- the cache of the names ---------------------------------------------- */

struct dns_cache_entry {
    bool valid;
    char name[DNS_NAME_MAX];
    k_u32_t address;
    k_u64_t expires_ms;
};

static struct dns_cache_entry g_cache[DNS_CACHE_ENTRIES];

/* The names of the protocol are not case sensitive: EXAMPLE.com and
 * example.com are one host, and one entry serves both. */
static bool dns_name_same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char left = *a;
        char right = *b;

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }

        if (right >= 'A' && right <= 'Z') {
            right = (char)(right - 'A' + 'a');
        }

        if (left != right) {
            return false;
        }

        a++;
        b++;
    }

    return *a == *b;
}

static k_u32_t dns_time_left_s(const struct dns_cache_entry *entry, k_u64_t now_ms)
{
    k_u64_t left_ms = entry->expires_ms - now_ms;

    /* The caller reads whole seconds and a name lives at least one: a
     * fraction of a second rounds up, so a "0 s left" means expired. */
    return (k_u32_t)((left_ms + 999u) / 1000u);
}

void dns_cache_clear(void)
{
    memset(g_cache, 0, sizeof(g_cache));
}

k_size_t dns_cache_count(void)
{
    k_size_t count = 0;
    k_size_t i;

    for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
        if (g_cache[i].valid) {
            count++;
        }
    }

    return count;
}

bool dns_cache_lookup(const char *name, k_u64_t now_ms, k_u32_t *address, k_u32_t *ttl_left_s)
{
    k_size_t i;

    if (name == NULL) {
        return false;
    }

    for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
        struct dns_cache_entry *entry = &g_cache[i];

        if (!entry->valid || !dns_name_same(entry->name, name)) {
            continue;
        }

        if (now_ms >= entry->expires_ms) {
            entry->valid = false;
            return false;
        }

        if (address != NULL) {
            *address = entry->address;
        }

        if (ttl_left_s != NULL) {
            *ttl_left_s = dns_time_left_s(entry, now_ms);
        }

        return true;
    }

    return false;
}

int dns_cache_store(const char *name, k_u32_t address, k_u32_t ttl_s, k_u64_t now_ms)
{
    struct dns_cache_entry *slot = NULL;
    k_size_t i;

    if (name == NULL || name[0] == '\0' || dns_name_too_long(name)) {
        return -1;
    }

    /* A time to live of zero is "ask me again": the server has no answer to
     * keep, and the cache does not pretend it has one. */
    if (ttl_s == 0u) {
        return 1;
    }

    if (ttl_s > DNS_TTL_MAX_S) {
        ttl_s = DNS_TTL_MAX_S;
    }

    for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
        if (g_cache[i].valid && dns_name_same(g_cache[i].name, name)) {
            slot = &g_cache[i];
            break;
        }
    }

    if (slot == NULL) {
        for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
            if (!g_cache[i].valid) {
                slot = &g_cache[i];
                break;
            }
        }
    }

    if (slot == NULL) {
        /* The entry that expires first has done the least for the caller: a
         * name asked for a moment ago stays, an old one makes room. */
        slot = &g_cache[0];

        for (i = 1; i < DNS_CACHE_ENTRIES; i++) {
            if (g_cache[i].expires_ms < slot->expires_ms) {
                slot = &g_cache[i];
            }
        }
    }

    slot->valid = true;
    memcpy(slot->name, name, strlen(name) + 1u);
    slot->address = address;
    slot->expires_ms = now_ms + (k_u64_t)ttl_s * 1000u;

    return 0;
}

const char *dns_cache_name(k_size_t slot, k_u64_t now_ms, k_u32_t *address, k_u32_t *ttl_left_s)
{
    if (slot >= DNS_CACHE_ENTRIES || !g_cache[slot].valid) {
        return NULL;
    }

    if (now_ms >= g_cache[slot].expires_ms) {
        return NULL;
    }

    if (address != NULL) {
        *address = g_cache[slot].address;
    }

    if (ttl_left_s != NULL) {
        *ttl_left_s = dns_time_left_s(&g_cache[slot], now_ms);
    }

    return g_cache[slot].name;
}
