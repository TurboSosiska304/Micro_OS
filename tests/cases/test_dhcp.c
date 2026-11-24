/*
 * Micro OS — the client of DHCP against a server of the test.
 *
 * The wire is a transport of this file: the client's datagrams go into a
 * buffer the test reads, and the answers are the ones a server would send,
 * built here byte by byte. What the case proves is the exchange of RFC 2131 -
 * a DISCOVER, an OFFER, a REQUEST that names the address and the server, an
 * ACK - and the life of the lease after it: T1, T2, a renewal, a refusal and
 * the timeout of a network without a server.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "net/dhcp.h"

/* --- the server of the test ------------------------------------------------ */

#define TEST_SERVER 0x0A000202u  /* 10.0.2.2  */
#define TEST_ADDRESS 0x0A00020Fu /* 10.0.2.15 */
#define TEST_MASK 0xFFFFFF00u
#define TEST_DNS 0x0A000203u
#define TEST_XID 0x4D4F5301u

static const k_u8_t g_mac[6] = {0x52u, 0x54u, 0x00u, 0x12u, 0x34u, 0x56u};

struct test_server {
    /* The clock of the test: the client reads it and the server moves it. */
    k_u64_t now_ms;
    k_u32_t slice_ms; /* what one look at an empty wire advances */

    /* What the client sent, in the order it sent it. */
    k_u8_t last[DHCP_MESSAGE_MAX];
    k_size_t last_size;
    k_size_t sent;

    /* The answers, one per look at the wire; an empty slot is a timeout. */
    k_u8_t answer[4][DHCP_MESSAGE_MAX];
    k_size_t answer_size[4];
    k_size_t answers;
    k_size_t next;

    struct dhcp_lease applied;
    bool applied_called;
    k_size_t applied_count;
};

static struct test_server g_server;

static k_u64_t server_now(void *context)
{
    (void)context;

    return g_server.now_ms;
}

static bool server_send(void *context, const void *datagram, k_size_t size)
{
    (void)context;

    if (size > sizeof(g_server.last)) {
        return false;
    }

    memcpy(g_server.last, datagram, size);
    g_server.last_size = size;
    g_server.sent++;

    return true;
}

static int server_receive(void *context, void *datagram, k_size_t size, k_u32_t timeout_ms)
{
    (void)context;

    if (g_server.next < g_server.answers) {
        k_size_t answer = g_server.answer_size[g_server.next];

        if (answer > size) {
            answer = size;
        }

        memcpy(datagram, g_server.answer[g_server.next], answer);
        g_server.next++;
        g_server.now_ms += 1u;

        return (int)answer;
    }

    /* The wire is empty: the look costs its timeout, which is the promise the
     * transport makes to the client. */
    g_server.now_ms += timeout_ms > 0u ? timeout_ms : g_server.slice_ms;

    return 0;
}

static void server_apply(void *context, const struct dhcp_lease *lease)
{
    (void)context;

    g_server.applied = *lease;
    g_server.applied_called = true;
    g_server.applied_count++;
}

static const struct dhcp_transport g_transport = {
    .send = server_send,
    .receive = server_receive,
    .now_ms = server_now,
    .apply = server_apply,
    .context = NULL,
};

/* --- the bytes of an answer ------------------------------------------------ */

static void put16(k_u8_t *data, k_u16_t value)
{
    data[0] = (k_u8_t)(value >> 8);
    data[1] = (k_u8_t)value;
}

static void put32(k_u8_t *data, k_u32_t value)
{
    data[0] = (k_u8_t)(value >> 24);
    data[1] = (k_u8_t)(value >> 16);
    data[2] = (k_u8_t)(value >> 8);
    data[3] = (k_u8_t)value;
}

static k_size_t option(k_u8_t *message, k_size_t at, k_u8_t code, const k_u8_t *value, k_u8_t size)
{
    message[at++] = code;
    message[at++] = size;
    memcpy(message + at, value, size);

    return at + size;
}

static k_size_t option32(k_u8_t *message, k_size_t at, k_u8_t code, k_u32_t value)
{
    k_u8_t bytes[4];

    put32(bytes, value);

    return option(message, at, code, bytes, 4u);
}

/*
 * An answer of a server: a BOOTP reply with the transaction id of the client,
 * its MAC, the cookie and the options of the case. `type` is OFFER, ACK or
 * NAK; a NAK carries no address.
 */
static k_size_t build_answer(k_u8_t *message, k_u8_t type, k_u32_t xid,
                             const struct dhcp_lease *lease)
{
    k_size_t at = 240u;

    memset(message, 0, DHCP_MESSAGE_MIN);

    message[0] = 2u; /* a reply */
    message[1] = 1u;
    message[2] = 6u;
    put32(message + 4, xid);
    put16(message + 10, 0x8000u);
    memcpy(message + 28, g_mac, 6);

    if (lease != NULL) {
        put32(message + 16, lease->address);
        put32(message + 20, lease->server);
    }

    message[236] = 99u;
    message[237] = 130u;
    message[238] = 83u;
    message[239] = 99u;

    at = option(message, at, 53u, &type, 1u);

    if (lease != NULL) {
        at = option32(message, at, 54u, lease->server);

        if (type != 6u) {
            at = option32(message, at, 1u, lease->mask);
            at = option32(message, at, 3u, lease->gateway);
            at = option32(message, at, 6u, lease->dns);
            at = option32(message, at, 51u, lease->lease_s);
        }
    }

    message[at++] = 255u;

    return at;
}

static struct dhcp_lease test_lease(void)
{
    struct dhcp_lease lease;

    memset(&lease, 0, sizeof(lease));
    lease.address = TEST_ADDRESS;
    lease.mask = TEST_MASK;
    lease.gateway = TEST_SERVER;
    lease.dns = TEST_DNS;
    lease.server = TEST_SERVER;
    lease.lease_s = 3600u;

    return lease;
}

static void server_reset(void)
{
    memset(&g_server, 0, sizeof(g_server));
    g_server.slice_ms = 100u;
}

static void server_queue(k_u8_t type, const struct dhcp_lease *lease)
{
    struct dhcp_lease answer = lease != NULL ? *lease : test_lease();

    g_server.answer_size[g_server.answers] =
        build_answer(g_server.answer[g_server.answers], type, TEST_XID, &answer);
    g_server.answers++;
}

/* The option walk of the test: the value of one option of the last message
 * the client sent, or NULL. */
static const k_u8_t *sent_option(k_u8_t code, k_u8_t *size)
{
    k_size_t at = 240u;

    while (at + 1u < g_server.last_size) {
        k_u8_t option_code = g_server.last[at++];

        if (option_code == 0u) {
            continue;
        }

        if (option_code == 255u) {
            return NULL;
        }

        if (at >= g_server.last_size) {
            return NULL;
        }

        *size = g_server.last[at++];

        if ((k_size_t)*size > g_server.last_size - at) {
            return NULL;
        }

        if (option_code == code) {
            return g_server.last + at;
        }

        at += *size;
    }

    return NULL;
}

static k_u32_t option_address(const k_u8_t *value)
{
    return value == NULL ? 0u
                         : ((k_u32_t)value[0] << 24) | ((k_u32_t)value[1] << 16) |
                               ((k_u32_t)value[2] << 8) | (k_u32_t)value[3];
}

/* --- the cases ------------------------------------------------------------- */

static void test_dhcp_exchange(void)
{
    ktest_begin("dhcp: the exchange takes the address of the server");

    struct dhcp_lease lease;

    server_reset();
    server_queue(2u, NULL); /* the OFFER */
    server_queue(5u, NULL); /* the ACK   */

    KT_CHECK_INT_EQ(dhcp_client_acquire(&g_transport, g_mac, TEST_XID, 3000u, &lease), DHCP_OK);

    KT_CHECK(g_server.sent >= 2u);
    KT_CHECK(g_server.applied_called);
    KT_CHECK_INT_EQ((int)g_server.applied.address, (int)TEST_ADDRESS);
    KT_CHECK_INT_EQ((int)g_server.applied.gateway, (int)TEST_SERVER);
    KT_CHECK_INT_EQ((int)g_server.applied.dns, (int)TEST_DNS);
    KT_CHECK_INT_EQ((int)g_server.applied.lease_s, 3600);
    KT_CHECK_INT_EQ((int)lease.address, (int)TEST_ADDRESS);
    KT_CHECK_INT_EQ((int)lease.mask, (int)TEST_MASK);

    /* The last message is the REQUEST: a BOOTP request with the cookie of
     * DHCP and the MAC of the client, which is what a server answers. */
    KT_CHECK_INT_EQ((int)g_server.last[0], 1);
    KT_CHECK_INT_EQ((int)g_server.last[2], 6);
    KT_CHECK_INT_EQ((int)g_server.last[236], 99);
    KT_CHECK_INT_EQ((int)g_server.last[237], 130);
    KT_CHECK_INT_EQ((int)g_server.last[238], 83);
    KT_CHECK_INT_EQ((int)g_server.last[239], 99);
    KT_CHECK_INT_EQ((int)memcmp(g_server.last + 28, g_mac, 6), 0);
    KT_CHECK_INT_EQ((int)g_server.last[1], 1); /* ethernet */
}

static void test_dhcp_request_names(void)
{
    ktest_begin("dhcp: the request names the address and the server");

    struct dhcp_lease lease;
    const k_u8_t *value;
    k_u8_t size = 0;

    server_reset();
    server_queue(2u, NULL);
    server_queue(5u, NULL);

    KT_CHECK_INT_EQ(dhcp_client_acquire(&g_transport, g_mac, TEST_XID, 3000u, &lease), DHCP_OK);

    value = sent_option(50u, &size);
    KT_CHECK_INT_EQ((int)size, 4);
    KT_CHECK_INT_EQ((int)option_address(value), (int)TEST_ADDRESS);

    value = sent_option(54u, &size);
    KT_CHECK_INT_EQ((int)size, 4);
    KT_CHECK_INT_EQ((int)option_address(value), (int)TEST_SERVER);
}

static void test_dhcp_no_server(void)
{
    ktest_begin("dhcp: a wire without a server is three datagrams and a timeout");

    struct dhcp_lease lease;

    server_reset();
    lease = test_lease();

    KT_CHECK_INT_EQ(dhcp_client_acquire(&g_transport, g_mac, TEST_XID, 1500u, &lease),
                    DHCP_TIMEOUT);

    /* Three DISCOVERs and no request: a wire without a server is three
     * datagrams and the fixed address stays where it was. */
    KT_CHECK_INT_EQ((int)g_server.sent, 3);
    KT_CHECK(!g_server.applied_called);
    KT_CHECK(g_server.now_ms >= 1500u);
}

static void test_dhcp_refused(void)
{
    ktest_begin("dhcp: a NAK is a refusal and not an address");

    struct dhcp_lease lease;
    const k_u8_t *value;
    k_u8_t size = 0;

    server_reset();
    server_queue(2u, NULL);
    server_queue(6u, NULL); /* a NAK */

    lease = test_lease();

    KT_CHECK_INT_EQ(dhcp_client_acquire(&g_transport, g_mac, TEST_XID, 3000u, &lease),
                    DHCP_REFUSED);
    KT_CHECK(!g_server.applied_called);

    /* The refusal is of the REQUEST, so the datagram of the client names the
     * address it asked for. */
    value = sent_option(50u, &size);
    KT_CHECK_INT_EQ((int)size, 4);
    KT_CHECK_INT_EQ((int)option_address(value), (int)TEST_ADDRESS);
}

static void test_dhcp_another_exchange(void)
{
    ktest_begin("dhcp: another transaction is not an answer");

    struct dhcp_lease lease;
    struct dhcp_lease answer = test_lease();
    k_size_t length;

    server_reset();

    /* An OFFER of another transaction, then the one of this client. */
    length = build_answer(g_server.answer[0], 2u, TEST_XID + 1u, &answer);
    g_server.answer_size[0] = length;
    g_server.answers = 1;
    server_queue(2u, NULL);
    server_queue(5u, NULL);

    KT_CHECK_INT_EQ(dhcp_client_acquire(&g_transport, g_mac, TEST_XID, 3000u, &lease), DHCP_OK);
    KT_CHECK_INT_EQ((int)lease.address, (int)TEST_ADDRESS);
}

static void test_dhcp_renewal(void)
{
    ktest_begin("dhcp: the renewal asks the same server for the same address");

    struct dhcp_lease lease;
    struct dhcp_lease renewed;
    const k_u8_t *value;
    k_u8_t size = 0;

    server_reset();
    server_queue(5u, NULL);

    lease = test_lease();
    lease.obtained_ms = 0u;

    KT_CHECK_INT_EQ(dhcp_client_renew(&g_transport, g_mac, &lease, TEST_XID, 2000u, &renewed),
                    DHCP_OK);
    KT_CHECK_INT_EQ((int)renewed.address, (int)TEST_ADDRESS);
    KT_CHECK(g_server.applied_called);

    value = sent_option(50u, &size);
    KT_CHECK_INT_EQ((int)option_address(value), (int)TEST_ADDRESS);
    value = sent_option(54u, &size);
    KT_CHECK_INT_EQ((int)option_address(value), (int)TEST_SERVER);
}

static void test_dhcp_lease_life(void)
{
    ktest_begin("dhcp: the moments of a lease and the numbers of the RFC");

    struct dhcp_lease lease = test_lease();

    /* A server that named neither T1 nor T2 gets the numbers of the RFC. */
    KT_CHECK_INT_EQ((int)dhcp_t1(&lease), 1800);
    KT_CHECK_INT_EQ((int)dhcp_t2(&lease), 3150);

    /* A server that named them is obeyed. */
    lease.t1_s = 600u;
    lease.t2_s = 3000u;
    KT_CHECK_INT_EQ((int)dhcp_t1(&lease), 600);
    KT_CHECK_INT_EQ((int)dhcp_t2(&lease), 3000);

    lease.obtained_ms = 1000u;

    KT_CHECK(!dhcp_renew_due(&lease, 1000u + 599u * 1000u));
    KT_CHECK(dhcp_renew_due(&lease, 1000u + 600u * 1000u));
    KT_CHECK(!dhcp_rebind_due(&lease, 1000u + 2999u * 1000u));
    KT_CHECK(dhcp_rebind_due(&lease, 1000u + 3000u * 1000u));

    /* A lease without an end is renewed by nobody. */
    lease.lease_s = 0u;
    KT_CHECK(!dhcp_renew_due(&lease, 1000u + 4000u * 1000u));
    KT_CHECK_INT_EQ((int)dhcp_t1(&lease), 0);
}

static void test_dhcp_xid(void)
{
    ktest_begin("dhcp: a transaction id that is not zero and moves");

    k_u32_t first = dhcp_xid(g_mac, 1000u, 1u);
    k_u32_t second = dhcp_xid(g_mac, 1000u, 2u);
    k_u32_t other = dhcp_xid(g_mac, 2000u, 1u);

    KT_CHECK(first != 0u);
    KT_CHECK(first != second);
    KT_CHECK(first != other);
}

void test_dhcp(void)
{
    test_dhcp_exchange();
    test_dhcp_request_names();
    test_dhcp_no_server();
    test_dhcp_refused();
    test_dhcp_another_exchange();
    test_dhcp_renewal();
    test_dhcp_lease_life();
    test_dhcp_xid();
}
