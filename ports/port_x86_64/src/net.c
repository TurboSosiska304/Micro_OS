/*
 * Micro OS — the x86-64 port: IPv4, ARP, ICMP, UDP, a TCP client and a TCP
 * server.
 *
 * The contract of the port (kernel/port/net.h) promises a session of TCP, a
 * server of TCP and one UDP exchange. The board fills it with lwIP and the
 * host with its own sockets; this port answered "no" until K6. This file is
 * the answer: the smallest stack that makes the contract true on a machine
 * with one network card, the 10.0.2.15 of the user-mode network of QEMU and
 * no DHCP.
 *
 * Why a stack of its own and not lwIP: the machine has no libc, no memory
 * protection and a kernel of a few hundred kilobytes, and lwIP is a
 * component of a system with sockets and a heap of its own. What the
 * contract needs is a few sessions at a time, opened by a command of the
 * shell or by the page of the state: an ARP cache of eight entries, an IPv4
 * header with a checksum, an echo reply, a datagram, a client of TCP with one
 * segment in flight, and a listener that takes a session the way the server
 * half of the contract says. A window larger than one segment and a
 * reassembly of fragments are the parts left out on purpose, and each one
 * says so where it would be.
 *
 * The configuration is the one QEMU user-net gives every guest, and there is
 * no DHCP: 10.0.2.15/24, the gateway 10.0.2.2 (which is also the host) and
 * the resolver 10.0.2.3. A machine on another network changes the four
 * constants below. The addresses arrive as names in the contract, so dotted
 * quads are parsed here and everything else goes to the resolver over UDP.
 * The answer is read by the resolver of components/net (net/dns.h) and kept
 * for its time to live, so the second command for one name costs no packet.
 *
 * Everything is polled from the task that made the call: the driver has no
 * interrupt (src/virtio_net.c) and the waits of the stack are a poll of the
 * card and a sleep of one tick. The tables of the sessions and of the
 * servers are one state behind one mutex, because the shell and the task of
 * the page of the state are two tasks on the same card; a frame that arrives
 * while the other task polls is still dispatched by its addresses, and the
 * stack has no queue of its own to fill. A timer of its own would need a
 * task; the retransmissions are the wait of the caller.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/lock.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kshell/kshell.h"
#include "net/dns.h"
#include "port_x86_64_net.h"

/* --- the sizes and the numbers of the protocols --------------------------- */

#define NET_ETH_HDR 14u
#define NET_IP_HDR 20u
#define NET_UDP_HDR 8u
#define NET_TCP_HDR 20u
#define NET_ICMP_HDR 8u

#define NET_ETHERTYPE_IPV4 0x0800u
#define NET_ETHERTYPE_ARP 0x0806u

#define NET_IP_ICMP 1u
#define NET_IP_TCP 6u
#define NET_IP_UDP 17u

#define NET_TTL 64u
#define NET_MSS 1400u

/* The receive side of a session: the window the client advertises and the
 * room its data has. One segment in flight means the peer never sends more
 * than the window of the last acknowledgement, so the buffer is the window. */
#define NET_TCP_RX_SIZE 4096u

#define NET_TCP_SOCKETS 4u

/* The doors of the machine: the page of the state and a shell of telnet are
 * enough, and a listener is twenty bytes. */
#define NET_TCP_SERVERS 2u

/* A session of a listener that never confirmed the handshake is dropped
 * after this long: the table is small and a half-open session has no owner
 * to close it. */
#define NET_TCP_HALFOPEN_MS 5000u

/* One UDP exchange at a time, which is the whole of NTP and of the resolver. */
#define NET_UDP_PAYLOAD_MAX 1400u

/* The waits of the stack: the patience of one answer and the interval
 * between two attempts. */
#define NET_WAIT_DEFAULT_MS 3000u
#define NET_RETRY_MS 400u
#define NET_ARP_TIMEOUT_MS 1000u
#define NET_DNS_TIMEOUT_MS 3000u

#define NET_ARP_ENTRIES 8u

/* The flags of a TCP header. */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

/* The type of an ICMP message. */
#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQUEST 8u

/* --- the configuration of the machine ------------------------------------- */

#define NET_IP(a, b, c, d) (((k_u32_t)(a) << 24) | ((k_u32_t)(b) << 16) | ((k_u32_t)(c) << 8) | (d))

static k_u32_t g_ip = NET_IP(10, 0, 2, 15);
static k_u32_t g_mask = NET_IP(255, 255, 255, 0);
static k_u32_t g_gateway = NET_IP(10, 0, 2, 2);
static k_u32_t g_dns = NET_IP(10, 0, 2, 3);
static k_u8_t g_mac[6];
static bool g_configured;

static k_u64_t g_ip_tx;
static k_u64_t g_ip_rx;
static k_u64_t g_ip_dropped;

/* The names: the queries that left the machine, the answers that came from
 * the cache of the resolver, and the packets a fragmented datagram cost. */
static k_u64_t g_dns_queries;
static k_u64_t g_dns_hits;
static k_u64_t g_fragments_dropped;

/* --- the bytes of a packet ------------------------------------------------- */

static k_u16_t net_get_be16(const k_u8_t *p)
{
    return (k_u16_t)(((k_u16_t)p[0] << 8) | (k_u16_t)p[1]);
}

static k_u32_t net_get_be32(const k_u8_t *p)
{
    return ((k_u32_t)p[0] << 24) | ((k_u32_t)p[1] << 16) | ((k_u32_t)p[2] << 8) | (k_u32_t)p[3];
}

static void net_put_be16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value >> 8);
    p[1] = (k_u8_t)(value & 0xFFu);
}

static void net_put_be32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value >> 24);
    p[1] = (k_u8_t)((value >> 16) & 0xFFu);
    p[2] = (k_u8_t)((value >> 8) & 0xFFu);
    p[3] = (k_u8_t)(value & 0xFFu);
}

static k_u32_t net_sum(k_u32_t sum, const k_u8_t *data, k_size_t len)
{
    k_size_t i;

    for (i = 0; i + 1u < len; i += 2u) {
        sum += ((k_u32_t)data[i] << 8) | (k_u32_t)data[i + 1u];
    }

    /* An odd byte is the high half of a word of zeros. */
    if ((len & 1u) != 0u) {
        sum += (k_u32_t)data[len - 1u] << 8;
    }

    return sum;
}

static k_u16_t net_sum_finish(k_u32_t sum)
{
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (k_u16_t)~sum;
}

static k_u16_t net_checksum(const k_u8_t *data, k_size_t len)
{
    return net_sum_finish(net_sum(0u, data, len));
}

/* The sum of the pseudo header of TCP and UDP: the two addresses, the
 * protocol and the length of the segment, which is what makes the transport
 * checksum cover the addresses as well. */
static k_u32_t net_pseudo_sum(k_u32_t source, k_u32_t destination, k_u8_t protocol, k_size_t len)
{
    k_u32_t sum = 0u;

    sum += (source >> 16) & 0xFFFFu;
    sum += source & 0xFFFFu;
    sum += (destination >> 16) & 0xFFFFu;
    sum += destination & 0xFFFFu;
    sum += protocol;
    sum += (k_u32_t)len;

    return sum;
}

/* --- the ARP cache --------------------------------------------------------- */

struct arp_entry {
    k_u32_t ip;
    k_u8_t mac[6];
    bool valid;
};

static struct arp_entry g_arp[NET_ARP_ENTRIES];
static k_size_t g_arp_next;
static bool g_arp_resolving;

static void arp_learn(k_u32_t ip, const k_u8_t mac[6])
{
    k_size_t i;
    struct arp_entry *slot = NULL;

    for (i = 0; i < NET_ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            slot = &g_arp[i];
            break;
        }
    }

    if (slot == NULL) {
        for (i = 0; i < NET_ARP_ENTRIES; i++) {
            if (!g_arp[i].valid) {
                slot = &g_arp[i];
                break;
            }
        }
    }

    if (slot == NULL) {
        /* A full cache loses its oldest entry: the network of a guest has a
         * handful of neighbours and a miss costs one request. */
        slot = &g_arp[g_arp_next % NET_ARP_ENTRIES];
        g_arp_next++;
    }

    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->valid = true;
}

static bool arp_lookup(k_u32_t ip, k_u8_t mac[6])
{
    k_size_t i;

    for (i = 0; i < NET_ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(mac, g_arp[i].mac, 6);
            return true;
        }
    }

    return false;
}

/* --- the configuration ----------------------------------------------------- */

static void net_config_apply(void)
{
    if (g_configured) {
        return;
    }

    if (!port_x86_64_nic_mac(g_mac)) {
        return;
    }

    g_configured = true;

    kinfo("net: %u.%u.%u.%u/24, gateway %u.%u.%u.%u, dns %u.%u.%u.%u", (unsigned)(g_ip >> 24),
          (unsigned)((g_ip >> 16) & 0xFFu), (unsigned)((g_ip >> 8) & 0xFFu),
          (unsigned)(g_ip & 0xFFu), (unsigned)(g_gateway >> 24),
          (unsigned)((g_gateway >> 16) & 0xFFu), (unsigned)((g_gateway >> 8) & 0xFFu),
          (unsigned)(g_gateway & 0xFFu), (unsigned)(g_dns >> 24), (unsigned)((g_dns >> 16) & 0xFFu),
          (unsigned)((g_dns >> 8) & 0xFFu), (unsigned)(g_dns & 0xFFu));
}

static bool net_ready(void)
{
    net_config_apply();

    return g_configured;
}

/* The next hop of an address: a neighbour of the subnet is its own next hop,
 * anything else goes through the gateway. */
static k_u32_t net_next_hop(k_u32_t destination)
{
    if ((destination & g_mask) == (g_ip & g_mask)) {
        return destination;
    }

    return g_gateway;
}

/* --- the input path -------------------------------------------------------- */

static bool net_resolve(const char *host, k_u32_t *address);

static void net_input(const k_u8_t *frame, k_size_t length);
static void tcp_input(k_u32_t source, const k_u8_t *segment, k_size_t length);
static void udp_input(k_u32_t source, const k_u8_t *datagram, k_size_t length);
static bool tcp_send_segment(struct k_socket *socket, k_u8_t flags, k_u32_t sequence,
                             const k_u8_t *payload, k_size_t length);

static bool net_poll_once(void)
{
    k_u8_t frame[PORT_X86_64_FRAME_MAX];
    k_size_t length = 0;

    if (!port_x86_64_nic_receive(frame, sizeof(frame), &length)) {
        return false;
    }

    net_input(frame, length);

    return true;
}

/* Drain the card: a wait looks at everything that arrived before it decides
 * that nothing did. */
static void net_poll(void)
{
    while (net_poll_once()) {
    }
}

/* --- the output path ------------------------------------------------------- */

static bool net_send_frame(const k_u8_t destination[6], k_u16_t ethertype, const k_u8_t *payload,
                           k_size_t length)
{
    k_u8_t frame[PORT_X86_64_FRAME_MAX];
    k_size_t total = NET_ETH_HDR + length;

    if (!g_configured || total > sizeof(frame)) {
        return false;
    }

    memcpy(frame, destination, 6);
    memcpy(frame + 6, g_mac, 6);
    net_put_be16(frame + 12, ethertype);
    memcpy(frame + NET_ETH_HDR, payload, length);

    if (!port_x86_64_nic_send(frame, total)) {
        g_ip_dropped++;
        return false;
    }

    return true;
}

static bool arp_request(k_u32_t ip)
{
    k_u8_t request[28];
    k_u8_t broadcast[6] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

    net_put_be16(request + 0, 0x0001u); /* ethernet */
    net_put_be16(request + 2, NET_ETHERTYPE_IPV4);
    request[4] = 6u;
    request[5] = 4u;
    net_put_be16(request + 6, 0x0001u); /* a request */
    memcpy(request + 8, g_mac, 6);
    net_put_be32(request + 14, g_ip);
    memset(request + 18, 0, 6);
    net_put_be32(request + 24, ip);

    return net_send_frame(broadcast, NET_ETHERTYPE_ARP, request, sizeof(request));
}

static bool arp_resolve(k_u32_t ip, k_u8_t mac[6], k_u32_t timeout_ms)
{
    k_u64_t deadline;

    if (arp_lookup(ip, mac)) {
        return true;
    }

    /* A resolution that starts inside another one does not wait: it asks and
     * gives up, which is the right answer for a frame built while the receive
     * path was running. The outer wait sees the reply and the caller of the
     * frame retransmits. */
    if (g_arp_resolving) {
        (void)arp_request(ip);
        return false;
    }

    g_arp_resolving = true;
    deadline = k_port_uptime_ms() + timeout_ms;

    while (k_port_uptime_ms() < deadline) {
        k_u64_t retry;

        (void)arp_request(ip);
        retry = k_port_uptime_ms() + NET_RETRY_MS;

        while (k_port_uptime_ms() < retry && k_port_uptime_ms() < deadline) {
            net_poll();

            if (arp_lookup(ip, mac)) {
                g_arp_resolving = false;
                return true;
            }

            k_sched_sleep_ms(1);
        }

        if (arp_lookup(ip, mac)) {
            g_arp_resolving = false;
            return true;
        }
    }

    g_arp_resolving = false;

    return false;
}

static bool net_send_ip(k_u32_t destination, k_u8_t protocol, const k_u8_t *payload,
                        k_size_t length)
{
    k_u8_t packet[PORT_X86_64_FRAME_MAX - NET_ETH_HDR];
    k_u8_t mac[6];
    k_u32_t hop;
    k_u16_t total = (k_u16_t)(NET_IP_HDR + length);
    static k_u16_t identification;

    if (length > sizeof(packet) - NET_IP_HDR || total > 1500u) {
        return false;
    }

    hop = net_next_hop(destination);

    if (!arp_resolve(hop, mac, NET_ARP_TIMEOUT_MS)) {
        return false;
    }

    packet[0] = 0x45u; /* IPv4, five words of header */
    packet[1] = 0x00u;
    net_put_be16(packet + 2, total);
    net_put_be16(packet + 4, ++identification);
    net_put_be16(packet + 6, 0x4000u); /* do not fragment */
    packet[8] = (k_u8_t)NET_TTL;
    packet[9] = protocol;
    net_put_be16(packet + 10, 0u);
    net_put_be32(packet + 12, g_ip);
    net_put_be32(packet + 16, destination);
    net_put_be16(packet + 10, net_checksum(packet, NET_IP_HDR));

    memcpy(packet + NET_IP_HDR, payload, length);

    if (!net_send_frame(mac, NET_ETHERTYPE_IPV4, packet, total)) {
        return false;
    }

    g_ip_tx++;

    return true;
}

/* --- ICMP ------------------------------------------------------------------ */

static k_u8_t g_icmp_answer[128];
static k_size_t g_icmp_answer_length;
static k_u32_t g_icmp_from;
static bool g_icmp_answered;

static void icmp_input(k_u32_t source, const k_u8_t *data, k_size_t length)
{
    k_u8_t reply[PORT_X86_64_FRAME_MAX - NET_ETH_HDR - NET_IP_HDR];

    if (length < NET_ICMP_HDR) {
        return;
    }

    if (net_checksum(data, length) != 0u) {
        g_ip_dropped++;
        return;
    }

    if (data[0] == ICMP_ECHO_REQUEST) {
        /* Someone pings the guest: the answer is the request with the type
         * swapped and a fresh checksum. */
        if (length > sizeof(reply)) {
            return;
        }

        memcpy(reply, data, length);
        reply[0] = ICMP_ECHO_REPLY;
        reply[1] = 0u;
        net_put_be16(reply + 2, 0u);
        net_put_be16(reply + 2, net_checksum(reply, length));
        (void)net_send_ip(source, NET_IP_ICMP, reply, length);

        return;
    }

    if (data[0] != ICMP_ECHO_REPLY) {
        return;
    }

    g_icmp_answer_length = length > sizeof(g_icmp_answer) ? sizeof(g_icmp_answer) : length;
    memcpy(g_icmp_answer, data, g_icmp_answer_length);
    g_icmp_from = source;
    g_icmp_answered = true;
}

/* --- UDP ------------------------------------------------------------------- */

struct udp_pending {
    bool active;
    k_u32_t remote_ip;
    k_u16_t remote_port;
    k_u16_t local_port;
    k_u8_t *answer;
    k_size_t answer_size;
    int length; /* -1 while the answer has not come */
};

static struct udp_pending g_udp;

static void udp_input(k_u32_t source, const k_u8_t *data, k_size_t length)
{
    k_u16_t source_port;
    k_u16_t destination_port;
    k_u16_t datagram_length;
    k_u16_t checksum;
    k_size_t payload;

    if (length < NET_UDP_HDR) {
        return;
    }

    source_port = net_get_be16(data);
    destination_port = net_get_be16(data + 2);
    datagram_length = net_get_be16(data + 4);
    checksum = net_get_be16(data + 6);

    if (datagram_length < NET_UDP_HDR || datagram_length > length) {
        g_ip_dropped++;
        return;
    }

    /* A zero checksum is "the sender did not compute one", which IPv4 allows
     * for UDP; anything else has to add up to zero over the pseudo header. */
    if (checksum != 0u) {
        k_u32_t sum = net_pseudo_sum(source, g_ip, NET_IP_UDP, datagram_length);

        sum = net_sum(sum, data, datagram_length);

        if (net_sum_finish(sum) != 0u) {
            g_ip_dropped++;
            return;
        }
    }

    if (!g_udp.active || destination_port != g_udp.local_port || source_port != g_udp.remote_port ||
        source != g_udp.remote_ip) {
        return;
    }

    payload = (k_size_t)(datagram_length - NET_UDP_HDR);

    if (payload > g_udp.answer_size) {
        g_udp.length = -2;
        return;
    }

    memcpy(g_udp.answer, data + NET_UDP_HDR, payload);
    g_udp.length = (int)payload;
}

static bool udp_send(k_u32_t destination, k_u16_t destination_port, k_u16_t source_port,
                     const k_u8_t *payload, k_size_t length)
{
    k_u8_t datagram[NET_UDP_HDR + NET_UDP_PAYLOAD_MAX];
    k_u32_t sum;

    if (length > NET_UDP_PAYLOAD_MAX) {
        return false;
    }

    net_put_be16(datagram + 0, source_port);
    net_put_be16(datagram + 2, destination_port);
    net_put_be16(datagram + 4, (k_u16_t)(NET_UDP_HDR + length));
    net_put_be16(datagram + 6, 0u);
    memcpy(datagram + NET_UDP_HDR, payload, length);

    sum = net_pseudo_sum(g_ip, destination, NET_IP_UDP, NET_UDP_HDR + length);
    sum = net_sum(sum, datagram, NET_UDP_HDR + length);

    /* A checksum of zero means "none" on the wire, and a real zero is sent
     * as 0xFFFF. */
    {
        k_u16_t checksum = net_sum_finish(sum);

        net_put_be16(datagram + 6, checksum == 0u ? 0xFFFFu : checksum);
    }

    return net_send_ip(destination, NET_IP_UDP, datagram, NET_UDP_HDR + length);
}

/* --- TCP ------------------------------------------------------------------- */

/*
 * A session of TCP. The peer is named by its address and port, the session by
 * the port of the guest; the three sequence numbers are the next byte to send
 * (snd_nxt), the oldest byte the peer has not acknowledged (snd_una) and the
 * next byte expected from the peer (rcv_nxt). One segment is in flight at a
 * time, so a retransmission is the segment again from the same sequence
 * number and nothing has to be kept between the calls.
 *
 * A session of a listener is the same structure: `listener` is the door it
 * came from, `accepted` says that accept() handed it out, and a session that
 * is not established yet is a handshake in flight. The client of the port
 * leaves `listener` NULL.
 */
struct k_socket {
    bool used;
    k_u32_t remote_ip;
    k_u16_t remote_port;
    k_u16_t local_port;
    k_u32_t snd_una;
    k_u32_t snd_nxt;
    k_u32_t rcv_nxt;
    k_u8_t rx[NET_TCP_RX_SIZE];
    k_size_t rx_len;
    struct k_server *listener;
    k_u64_t born_ms;
    bool established;
    bool fin_sent;
    bool fin_received;
    bool reset;
    bool accepted;
};

struct k_server {
    bool used;
    k_u16_t port;
};

static struct k_socket g_tcp_sockets[NET_TCP_SOCKETS];
static struct k_server g_tcp_servers[NET_TCP_SERVERS];
static k_u16_t g_next_port = 49152u;

/*
 * The tables of the sessions and of the doors are one state, and two tasks
 * reach them: the shell and the task of the page of the state. The mutex is
 * held for a look and a change, never across a wait - the waits of both tasks
 * are what polls the card for both, and a lock held by an accept that waits
 * would keep the connecting task out of the table it needs.
 */
static k_mutex_t g_net_lock = K_MUTEX_INITIALIZER;

static bool net_lock(void)
{
    return k_mutex_lock(&g_net_lock) == 0;
}

static void net_unlock(void)
{
    (void)k_mutex_unlock(&g_net_lock);
}

static k_u16_t net_next_port(void)
{
    k_u16_t port = g_next_port++;

    if (g_next_port < 49152u) {
        g_next_port = 49152u;
    }

    return port;
}

/*
 * The sequence number of the first byte has no security meaning on this
 * machine and no clock of its own to borrow: the uptime and a constant are
 * enough to keep two sessions apart.
 */
static k_u32_t tcp_initial_sequence(void)
{
    return (k_u32_t)k_port_uptime_us() ^ 0x5A5A0000u;
}

/* A session of a listener that never confirmed the handshake has no owner to
 * close it: the table is small, and the oldest of them makes room. */
static struct k_socket *tcp_socket_expired(k_u64_t now_ms)
{
    k_size_t i;

    for (i = 0; i < NET_TCP_SOCKETS; i++) {
        struct k_socket *socket = &g_tcp_sockets[i];

        if (socket->used && socket->listener != NULL && !socket->accepted && !socket->established &&
            now_ms - socket->born_ms >= NET_TCP_HALFOPEN_MS) {
            socket->used = false;

            return socket;
        }
    }

    return NULL;
}

static struct k_socket *tcp_socket_new(k_u32_t remote_ip, k_u16_t remote_port, k_u16_t local_port)
{
    struct k_socket *socket = NULL;
    k_size_t i;

    for (i = 0; i < NET_TCP_SOCKETS; i++) {
        if (!g_tcp_sockets[i].used) {
            socket = &g_tcp_sockets[i];
            break;
        }
    }

    if (socket == NULL) {
        socket = tcp_socket_expired(k_port_uptime_ms());
    }

    if (socket == NULL) {
        return NULL;
    }

    memset(socket, 0, sizeof(*socket));
    socket->used = true;
    socket->remote_ip = remote_ip;
    socket->remote_port = remote_port;
    socket->local_port = local_port;
    socket->born_ms = k_port_uptime_ms();

    return socket;
}

static struct k_socket *tcp_find(k_u32_t source, k_u16_t source_port, k_u16_t destination_port)
{
    k_size_t i;

    for (i = 0; i < NET_TCP_SOCKETS; i++) {
        struct k_socket *socket = &g_tcp_sockets[i];

        if (socket->used && socket->remote_ip == source && socket->remote_port == source_port &&
            socket->local_port == destination_port) {
            return socket;
        }
    }

    return NULL;
}

static struct k_server *tcp_server_find(k_u16_t port)
{
    k_size_t i;

    for (i = 0; i < NET_TCP_SERVERS; i++) {
        if (g_tcp_servers[i].used && g_tcp_servers[i].port == port) {
            return &g_tcp_servers[i];
        }
    }

    return NULL;
}

static bool tcp_send_raw(k_u16_t local_port, k_u32_t remote_ip, k_u16_t remote_port,
                         k_u32_t sequence, k_u32_t acknowledgement, k_u16_t window, k_u8_t flags,
                         const k_u8_t *payload, k_size_t length)
{
    k_u8_t segment[NET_TCP_HDR + NET_MSS];
    k_size_t total = NET_TCP_HDR + length;
    k_u32_t sum;

    if (length > NET_MSS) {
        return false;
    }

    net_put_be16(segment + 0, local_port);
    net_put_be16(segment + 2, remote_port);
    net_put_be32(segment + 4, sequence);
    net_put_be32(segment + 8, acknowledgement);
    segment[12] = (k_u8_t)((NET_TCP_HDR / 4u) << 4);
    segment[13] = flags;
    net_put_be16(segment + 14, window);
    net_put_be16(segment + 16, 0u);
    net_put_be16(segment + 18, 0u);

    if (length > 0 && payload != NULL) {
        memcpy(segment + NET_TCP_HDR, payload, length);
    }

    sum = net_pseudo_sum(g_ip, remote_ip, NET_IP_TCP, total);
    sum = net_sum(sum, segment, total);
    net_put_be16(segment + 16, net_sum_finish(sum));

    return net_send_ip(remote_ip, NET_IP_TCP, segment, total);
}

static bool tcp_send_segment(struct k_socket *socket, k_u8_t flags, k_u32_t sequence,
                             const k_u8_t *payload, k_size_t length)
{
    return tcp_send_raw(socket->local_port, socket->remote_ip, socket->remote_port, sequence,
                        socket->rcv_nxt, (k_u16_t)(NET_TCP_RX_SIZE - socket->rx_len), flags,
                        payload, length);
}

/*
 * A reset for a port nobody listens on: the peer learns at once that the door
 * is shut instead of retrying its SYN for a minute. The 20 bytes are built
 * here and not on a session, because there is no session to build them from.
 */
static void tcp_send_reset(k_u32_t source, k_u16_t source_port, k_u16_t destination_port,
                           k_u32_t sequence, k_u32_t acknowledgement)
{
    k_u8_t segment[NET_TCP_HDR];
    k_u32_t sum;

    net_put_be16(segment + 0, destination_port);
    net_put_be16(segment + 2, source_port);
    net_put_be32(segment + 4, sequence);
    net_put_be32(segment + 8, acknowledgement);
    segment[12] = (k_u8_t)((NET_TCP_HDR / 4u) << 4);
    segment[13] = TCP_RST | TCP_ACK;
    net_put_be16(segment + 14, 0u);
    net_put_be16(segment + 16, 0u);
    net_put_be16(segment + 18, 0u);

    sum = net_pseudo_sum(g_ip, source, NET_IP_TCP, NET_TCP_HDR);
    sum = net_sum(sum, segment, NET_TCP_HDR);
    net_put_be16(segment + 16, net_sum_finish(sum));

    (void)net_send_ip(source, NET_IP_TCP, segment, NET_TCP_HDR);
}

/*
 * One segment, and the wait for its acknowledgement. The segment is sent
 * again from the same sequence number every NET_RETRY_MS until the peer has
 * acknowledged every byte of it or the budget is over. This is the whole of
 * the reliability of a session: a lost segment costs a repeat and not a
 * session, and a peer that sends a duplicate costs one acknowledgement.
 */
static bool tcp_send_and_wait(struct k_socket *socket, k_u8_t flags, k_u32_t sequence,
                              const k_u8_t *payload, k_size_t length, k_u32_t timeout_ms)
{
    k_u32_t target = sequence + (k_u32_t)length;
    k_u64_t deadline = k_port_uptime_ms() + timeout_ms;
    k_u64_t retry = 0;

    if ((flags & (TCP_SYN | TCP_FIN)) != 0u) {
        target++;
    }

    while (!socket->reset && (k_i32_t)(socket->snd_una - target) < 0) {
        k_u64_t now = k_port_uptime_ms();

        if (now >= deadline) {
            return false;
        }

        if (now >= retry) {
            if (!tcp_send_segment(socket, flags, sequence, payload, length)) {
                return false;
            }

            retry = now + NET_RETRY_MS;
        }

        net_poll();

        if (!socket->reset && (k_i32_t)(socket->snd_una - target) < 0) {
            k_sched_sleep_ms(1);
        }
    }

    return !socket->reset;
}

/*
 * A SYN for a door: the session takes the port of the listener as its own
 * and answers SYN|ACK from a number of its own. It is not a session of
 * accept() until the peer confirms the handshake with the last ACK, which
 * tcp_input() sees as the acknowledgement of our SYN.
 */
static void tcp_session_open(struct k_server *server, k_u32_t source, k_u16_t source_port,
                             k_u16_t destination_port, k_u32_t sequence)
{
    struct k_socket *socket;

    if (!net_lock()) {
        return;
    }

    socket = tcp_socket_new(source, source_port, destination_port);

    if (socket != NULL) {
        socket->listener = server;
        socket->snd_una = tcp_initial_sequence();
        socket->snd_nxt = socket->snd_una + 1u;
        socket->rcv_nxt = sequence + 1u;
    }

    net_unlock();

    if (socket == NULL) {
        /* A full table is a client that waits: the retransmitted SYN finds a
         * session as soon as one of the four is closed. */
        return;
    }

    (void)tcp_send_segment(socket, TCP_SYN | TCP_ACK, socket->snd_una, NULL, 0);
}

static void tcp_input(k_u32_t source, const k_u8_t *data, k_size_t length)
{
    struct k_socket *socket;
    k_u16_t source_port;
    k_u16_t destination_port;
    k_u32_t sequence;
    k_u32_t acknowledgement;
    k_size_t header;
    k_size_t payload_length;
    k_u8_t flags;

    if (length < NET_TCP_HDR) {
        return;
    }

    source_port = net_get_be16(data);
    destination_port = net_get_be16(data + 2);
    sequence = net_get_be32(data + 4);
    acknowledgement = net_get_be32(data + 8);
    header = (k_size_t)(data[12] >> 4) * 4u;
    flags = data[13];

    if (header < NET_TCP_HDR || header > length) {
        return;
    }

    payload_length = length - header;

    if (!net_lock()) {
        return;
    }

    socket = tcp_find(source, source_port, destination_port);

    if (socket == NULL) {
        struct k_server *server = NULL;

        if ((flags & TCP_RST) == 0u && (flags & TCP_SYN) != 0u && (flags & TCP_ACK) == 0u) {
            server = tcp_server_find(destination_port);
        }

        net_unlock();

        if (server != NULL) {
            tcp_session_open(server, source, source_port, destination_port, sequence);
            return;
        }

        if ((flags & TCP_RST) != 0u) {
            /* A reset for a session this machine does not have is not
             * answered with another: two of them would never stop. */
            return;
        }

        /* No session and no door: the peer is told, and its connect() fails
         * with a reset instead of a timeout. */
        tcp_send_reset(source, source_port, destination_port, acknowledgement,
                       sequence + (k_u32_t)payload_length + (((flags & TCP_SYN) != 0u) ? 1u : 0u) +
                           (((flags & TCP_FIN) != 0u) ? 1u : 0u));
        return;
    }

    net_unlock();

    if ((flags & TCP_RST) != 0u) {
        socket->reset = true;
        return;
    }

    if ((flags & TCP_SYN) != 0u) {
        if (socket->listener != NULL && !socket->established) {
            /* The SYN/ACK was lost and the client asks again: the same
             * answer from the same number, which is a retransmission. */
            (void)tcp_send_segment(socket, TCP_SYN | TCP_ACK, socket->snd_una, NULL, 0);
            return;
        }

        if ((flags & TCP_ACK) != 0u && acknowledgement == socket->snd_nxt && !socket->established) {
            socket->snd_una = acknowledgement;
            socket->rcv_nxt = sequence + 1u;
            socket->established = true;
            (void)tcp_send_segment(socket, TCP_ACK, socket->snd_nxt, NULL, 0);
        }

        return;
    }

    if (!socket->established) {
        /* The client of a listener confirms the handshake with the ACK of our
         * SYN|ACK; anything else on a session that is not up is not ours to
         * read yet. */
        if (socket->listener != NULL && (flags & TCP_ACK) != 0u &&
            acknowledgement == socket->snd_nxt) {
            socket->snd_una = acknowledgement;
            socket->established = true;
        } else {
            return;
        }
    }

    if ((flags & TCP_ACK) != 0u) {
        if ((k_i32_t)(acknowledgement - socket->snd_una) > 0 &&
            (k_i32_t)(acknowledgement - socket->snd_nxt) <= 0) {
            socket->snd_una = acknowledgement;
        }
    }

    if (payload_length > 0) {
        if (sequence == socket->rcv_nxt) {
            k_size_t room = NET_TCP_RX_SIZE - socket->rx_len;
            k_size_t copy = payload_length > room ? room : payload_length;

            memcpy(socket->rx + socket->rx_len, data + header, copy);
            socket->rx_len += copy;

            /* The bytes the window had no room for are counted as received
             * and lost: the window was advertised, so this is a peer that
             * ignored it, and the session is better ended than desynced. */
            socket->rcv_nxt += (k_u32_t)payload_length;
        }

        /* A duplicate or an out-of-order segment is answered with the
         * acknowledgement of what was received; nothing is reassembled. */
        (void)tcp_send_segment(socket, TCP_ACK, socket->snd_nxt, NULL, 0);
    }

    if ((flags & TCP_FIN) != 0u) {
        if (sequence + (k_u32_t)payload_length == socket->rcv_nxt) {
            socket->rcv_nxt++;
            socket->fin_received = true;
            (void)tcp_send_segment(socket, TCP_ACK, socket->snd_nxt, NULL, 0);
        }
    }
}

/* --- the dispatch of a frame ---------------------------------------------- */

static void arp_input(const k_u8_t *frame, k_size_t length)
{
    k_u32_t sender_ip;
    k_u32_t target_ip;

    if (length < NET_ETH_HDR + 28u) {
        return;
    }

    sender_ip = net_get_be32(frame + NET_ETH_HDR + 14);
    target_ip = net_get_be32(frame + NET_ETH_HDR + 24);

    arp_learn(sender_ip, frame + 6);

    /* A request for the guest is answered; the reply to ours was learned
     * above and the wait of arp_resolve sees it. */
    if (net_get_be16(frame + NET_ETH_HDR + 6) == 0x0001u && target_ip == g_ip) {
        k_u8_t answer[28];
        k_u8_t mac[6];

        memcpy(mac, frame + 6, 6);

        net_put_be16(answer + 0, 0x0001u);
        net_put_be16(answer + 2, NET_ETHERTYPE_IPV4);
        answer[4] = 6u;
        answer[5] = 4u;
        net_put_be16(answer + 6, 0x0002u); /* a reply */
        memcpy(answer + 8, g_mac, 6);
        net_put_be32(answer + 14, g_ip);
        memcpy(answer + 18, mac, 6);
        net_put_be32(answer + 24, sender_ip);

        (void)net_send_frame(mac, NET_ETHERTYPE_ARP, answer, sizeof(answer));
    }
}

static void ip_input(const k_u8_t *frame, k_size_t length)
{
    const k_u8_t *packet = frame + NET_ETH_HDR;
    k_size_t packet_length = length - NET_ETH_HDR;
    k_size_t header_length;
    k_u16_t total;
    k_u32_t source;
    k_u32_t destination;

    if (packet_length < NET_IP_HDR) {
        return;
    }

    if ((packet[0] >> 4) != 4u) {
        return;
    }

    header_length = (k_size_t)(packet[0] & 0x0Fu) * 4u;

    if (header_length < NET_IP_HDR || header_length > packet_length) {
        return;
    }

    total = net_get_be16(packet + 2);

    if (total < header_length || total > packet_length) {
        g_ip_dropped++;
        return;
    }

    if (net_checksum(packet, header_length) != 0u) {
        g_ip_dropped++;
        return;
    }

    /* A fragment is not reassembled: the stack sends with DF and the frames
     * it accepts are the ones a peer of one ethernet segment sends. The
     * packet is counted and dropped, so a peer that fragments sees silence
     * and a person sees the number in `netinfo`. */
    if ((net_get_be16(packet + 6) & 0x3FFFu) != 0u) {
        g_ip_dropped++;
        g_fragments_dropped++;
        return;
    }

    source = net_get_be32(packet + 12);
    destination = net_get_be32(packet + 16);

    if (destination != g_ip && destination != 0xFFFFFFFFu) {
        return;
    }

    g_ip_rx++;

    /* Every frame teaches the cache the address of the neighbour that sent
     * it, which is what keeps the answers of the stack from asking the bus
     * to repeat itself. */
    arp_learn(source, frame + 6);

    switch (packet[9]) {
    case NET_IP_ICMP:
        icmp_input(source, packet + header_length, total - header_length);
        break;
    case NET_IP_UDP:
        udp_input(source, packet + header_length, total - header_length);
        break;
    case NET_IP_TCP:
        tcp_input(source, packet + header_length, total - header_length);
        break;
    default:
        break;
    }
}

static void net_input(const k_u8_t *frame, k_size_t length)
{
    k_u16_t ethertype;

    if (length < NET_ETH_HDR) {
        return;
    }

    ethertype = net_get_be16(frame + 12);

    if (ethertype == NET_ETHERTYPE_ARP) {
        arp_input(frame, length);
    } else if (ethertype == NET_ETHERTYPE_IPV4) {
        ip_input(frame, length);
    }
}

/* --- the contract: a session of TCP --------------------------------------- */

k_port_socket_t *k_port_net_connect(const char *host, k_u16_t port, k_u32_t timeout_ms)
{
    struct k_socket *socket;
    k_u32_t address;
    k_u32_t sequence;

    if (host == NULL || port == 0u || !net_ready()) {
        return NULL;
    }

    if (!net_resolve(host, &address)) {
        kwarn("net: cannot resolve %s", host);
        return NULL;
    }

    if (!net_lock()) {
        return NULL;
    }

    socket = tcp_socket_new(address, port, net_next_port());

    if (socket != NULL) {
        sequence = tcp_initial_sequence();
        socket->snd_una = sequence;
        socket->snd_nxt = sequence + 1u;
    }

    net_unlock();

    if (socket == NULL) {
        kwarn("net: no free session");
        return NULL;
    }

    if (!tcp_send_and_wait(socket, TCP_SYN, sequence, NULL, 0, timeout_ms) ||
        !socket->established) {
        socket->used = false;
        return NULL;
    }

    return socket;
}

int k_port_net_send(k_port_socket_t *handle, const void *data, k_size_t size, bool wait)
{
    struct k_socket *socket = handle;
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_size_t sent = 0;

    if (socket == NULL || !socket->used || socket->reset || !socket->established || data == NULL) {
        return -1;
    }

    while (sent < size) {
        k_size_t chunk = size - sent > NET_MSS ? NET_MSS : size - sent;
        k_u32_t sequence = socket->snd_nxt;

        if (!wait && (k_i32_t)(socket->snd_nxt - socket->snd_una) > 0) {
            /* The previous piece is not acknowledged and the caller does not
             * want to wait: what did not fit now is dropped, as the contract
             * says. */
            break;
        }

        socket->snd_nxt += (k_u32_t)chunk;

        if (!wait) {
            if (!tcp_send_segment(socket, TCP_PSH | TCP_ACK, sequence, bytes + sent, chunk)) {
                return -1;
            }

            sent += chunk;
            break;
        }

        if (!tcp_send_and_wait(socket, TCP_PSH | TCP_ACK, sequence, bytes + sent, chunk,
                               NET_WAIT_DEFAULT_MS)) {
            return -1;
        }

        sent += chunk;
    }

    return (int)sent;
}

int k_port_net_recv(k_port_socket_t *handle, void *data, k_size_t size)
{
    struct k_socket *socket = handle;
    k_u64_t deadline;

    if (socket == NULL || !socket->used || data == NULL || size == 0u) {
        return -1;
    }

    deadline = k_port_uptime_ms() + NET_WAIT_DEFAULT_MS;

    for (;;) {
        if (socket->rx_len > 0) {
            k_size_t take = socket->rx_len > size ? size : socket->rx_len;

            memcpy(data, socket->rx, take);
            memmove(socket->rx, socket->rx + take, socket->rx_len - take);
            socket->rx_len -= take;

            return (int)take;
        }

        if (socket->reset) {
            return -1;
        }

        if (socket->fin_received) {
            return 0;
        }

        if (k_port_uptime_ms() >= deadline) {
            return -1;
        }

        net_poll();

        if (socket->rx_len == 0 && !socket->fin_received && !socket->reset) {
            k_sched_sleep_ms(1);
        }
    }
}

void k_port_net_close(k_port_socket_t *handle)
{
    struct k_socket *socket = handle;
    k_u64_t deadline;

    if (socket == NULL) {
        return;
    }

    if (socket->used && socket->established && !socket->fin_sent && !socket->reset) {
        k_u32_t sequence = socket->snd_nxt;

        socket->snd_nxt++;
        socket->fin_sent = true;

        (void)tcp_send_and_wait(socket, TCP_FIN | TCP_ACK, sequence, NULL, 0, 1000u);

        /* The peer closes its half too; the last acknowledgement of the
         * session is not worth another second of the console. */
        deadline = k_port_uptime_ms() + 1000u;

        while (!socket->fin_received && !socket->reset && k_port_uptime_ms() < deadline) {
            net_poll();
            k_sched_sleep_ms(1);
        }
    }

    socket->used = false;
}

/* --- the contract: a server of TCP ---------------------------------------- */

/*
 * A door: the table of the servers holds the ports of the machine, and the
 * sessions that come to one wait in the same table of sessions as the client
 * of the port. A port that is already ours, or the fifth door, is a NULL: the
 * command that asked says which.
 */
k_port_server_t *k_port_net_listen(k_u16_t port)
{
    struct k_server *server = NULL;
    k_size_t i;

    if (port == 0u || !net_ready()) {
        return NULL;
    }

    if (!net_lock()) {
        return NULL;
    }

    if (tcp_server_find(port) != NULL) {
        net_unlock();
        return NULL;
    }

    for (i = 0; i < NET_TCP_SERVERS; i++) {
        if (!g_tcp_servers[i].used) {
            server = &g_tcp_servers[i];
            break;
        }
    }

    if (server != NULL) {
        server->used = true;
        server->port = port;
    }

    net_unlock();

    if (server == NULL) {
        return NULL;
    }

    kinfo("net: listening on port %u", (unsigned)port);

    return server;
}

/*
 * A session of the door that confirmed its handshake and was not handed out
 * yet. The wait is a poll of the card: the SYN comes to whoever looks, and
 * the task that waits is the task that looks.
 */
k_port_socket_t *k_port_net_accept(k_port_server_t *handle, k_u32_t timeout_ms)
{
    struct k_server *server = handle;
    k_u64_t deadline;

    if (server == NULL || !server->used) {
        return NULL;
    }

    deadline = k_port_uptime_ms() + timeout_ms;

    for (;;) {
        struct k_socket *ready = NULL;
        k_size_t i;

        if (net_lock()) {
            for (i = 0; i < NET_TCP_SOCKETS; i++) {
                struct k_socket *socket = &g_tcp_sockets[i];

                if (socket->used && socket->listener == server && socket->established &&
                    !socket->accepted) {
                    socket->accepted = true;
                    ready = socket;
                    break;
                }
            }

            net_unlock();
        }

        if (ready != NULL) {
            return ready;
        }

        if (k_port_uptime_ms() >= deadline) {
            return NULL;
        }

        net_poll();

        if (k_port_uptime_ms() < deadline) {
            k_sched_sleep_ms(1);
        }
    }
}

void k_port_net_server_close(k_port_server_t *handle)
{
    struct k_server *server = handle;
    k_size_t i;

    if (server == NULL) {
        return;
    }

    if (!net_lock()) {
        server->used = false;
        return;
    }

    /* The sessions accept() handed out belong to their task and stay open:
     * the contract says so, and the page of the state answers its client
     * while the door closes. */
    for (i = 0; i < NET_TCP_SOCKETS; i++) {
        struct k_socket *socket = &g_tcp_sockets[i];

        if (socket->used && socket->listener == server && !socket->accepted) {
            socket->used = false;
        }
    }

    server->used = false;

    net_unlock();
}

/* --- the contract: one exchange of UDP ------------------------------------ */

static int udp_exchange_address(k_u32_t address, k_u16_t port, const void *request,
                                k_size_t request_size, void *answer, k_size_t answer_size,
                                k_u32_t timeout_ms)
{
    k_u64_t deadline;
    k_u16_t local_port;
    int result;

    if (request == NULL || answer == NULL || request_size > NET_UDP_PAYLOAD_MAX ||
        answer_size == 0u) {
        return -1;
    }

    local_port = net_next_port();

    g_udp.active = true;
    g_udp.remote_ip = address;
    g_udp.remote_port = port;
    g_udp.local_port = local_port;
    g_udp.answer = (k_u8_t *)answer;
    g_udp.answer_size = answer_size;
    g_udp.length = -1;

    if (!udp_send(address, port, local_port, (const k_u8_t *)request, request_size)) {
        g_udp.active = false;
        return -1;
    }

    deadline = k_port_uptime_ms() + timeout_ms;

    while (g_udp.length < 0 && k_port_uptime_ms() < deadline) {
        net_poll();

        if (g_udp.length < 0) {
            k_sched_sleep_ms(1);
        }
    }

    result = g_udp.length;
    g_udp.active = false;

    return result;
}

int k_port_net_udp_exchange(const char *host, k_u16_t port, const void *request,
                            k_size_t request_size, void *answer, k_size_t answer_size,
                            k_u32_t timeout_ms)
{
    k_u32_t address;

    if (host == NULL || !net_ready()) {
        return -1;
    }

    if (!net_resolve(host, &address)) {
        kwarn("net: cannot resolve %s", host);
        return -1;
    }

    return udp_exchange_address(address, port, request, request_size, answer, answer_size,
                                timeout_ms);
}

/* --- the names ------------------------------------------------------------- */

static bool net_parse_ip(const char *text, k_u32_t *address)
{
    k_u32_t value = 0;
    k_u32_t part = 0;
    int digits = 0;
    int parts = 0;
    k_size_t i;

    if (text == NULL) {
        return false;
    }

    for (i = 0;; i++) {
        char c = text[i];

        if (c >= '0' && c <= '9') {
            part = part * 10u + (k_u32_t)(c - '0');

            if (++digits > 3 || part > 255u) {
                return false;
            }

            continue;
        }

        if (c == '.' || c == '\0') {
            if (digits == 0) {
                return false;
            }

            value = (value << 8) | part;
            part = 0;
            digits = 0;
            parts++;

            if (c == '\0') {
                break;
            }

            if (parts >= 4) {
                /* A fifth field is not an address this machine speaks. */
                return false;
            }

            continue;
        }

        return false;
    }

    if (parts != 4) {
        return false;
    }

    *address = value;

    return true;
}

/* --- the names ------------------------------------------------------------- */

/*
 * One query over the UDP exchange of the port, read by the resolver of
 * components/net. A name that the server does not answer and a packet that is
 * not an answer of type A are the same "no" here, and the caller says so.
 */
static bool dns_lookup(const char *name, struct dns_answer *record)
{
    k_u8_t query[DNS_PACKET_MAX];
    k_u8_t answer[DNS_PACKET_MAX];
    k_u16_t identifier = (k_u16_t)(k_port_uptime_us() & 0xFFFFu);
    int length;
    int reply;

    length = dns_build_query(name, identifier, query, sizeof(query));

    if (length < 0) {
        return false;
    }

    g_dns_queries++;

    reply = udp_exchange_address(g_dns, DNS_PORT, query, (k_size_t)length, answer, sizeof(answer),
                                 NET_DNS_TIMEOUT_MS);

    if (reply < 0) {
        return false;
    }

    return dns_parse_answer(answer, (k_size_t)reply, identifier, record) == 0;
}

/*
 * A name is an address, a name in the cache, or one query. The cache is the
 * one of components/net and its time is the uptime of the machine; it is read
 * and written under the lock of the tables, because the shell and the page of
 * the state both resolve names. `cached` tells the caller which of the three
 * answered, and the command of the resolver prints it.
 */
static bool net_resolve_cached(const char *host, k_u32_t *address, bool *cached)
{
    struct dns_answer record;
    k_u64_t now;

    if (cached != NULL) {
        *cached = false;
    }

    if (net_parse_ip(host, address)) {
        return true;
    }

    if (host == NULL || host[0] == '\0') {
        return false;
    }

    now = k_port_uptime_ms();

    if (net_lock()) {
        bool hit = dns_cache_lookup(host, now, address, NULL);

        net_unlock();

        if (hit) {
            g_dns_hits++;

            if (cached != NULL) {
                *cached = true;
            }

            return true;
        }
    }

    if (!dns_lookup(host, &record)) {
        return false;
    }

    *address = record.address;

    if (net_lock()) {
        (void)dns_cache_store(host, record.address, record.ttl_s, k_port_uptime_ms());
        net_unlock();
    }

    return true;
}

static bool net_resolve(const char *host, k_u32_t *address)
{
    return net_resolve_cached(host, address, NULL);
}

/* --- the commands of the port ---------------------------------------------- */

static void print_ip(k_u32_t address)
{
    kshell_printf("%u.%u.%u.%u", (unsigned)(address >> 24), (unsigned)((address >> 16) & 0xFFu),
                  (unsigned)((address >> 8) & 0xFFu), (unsigned)(address & 0xFFu));
}

static int cmd_netinfo(int argc, char **argv)
{
    k_u8_t mac[6];
    const char *name = port_x86_64_nic_name();

    K_UNUSED(argc);
    K_UNUSED(argv);

    if (name == NULL) {
        kshell_printf("netinfo: there is no network device\n");
        return KSHELL_ERROR;
    }

    net_config_apply();

    kshell_printf("netinfo: %s, %s\n", name, port_x86_64_nic_link() ? "link up" : "link down");

    if (port_x86_64_nic_mac(mac)) {
        kshell_printf("  mac %02x:%02x:%02x:%02x:%02x:%02x\n", (unsigned)mac[0], (unsigned)mac[1],
                      (unsigned)mac[2], (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    }

    kshell_printf("  ip ");
    print_ip(g_ip);
    kshell_printf("/24, gateway ");
    print_ip(g_gateway);
    kshell_printf(", dns ");
    print_ip(g_dns);
    kshell_printf("\n");

    kshell_printf("  %u frame(s) in, %u out, %u dropped; %u ip in, %u out\n",
                  (unsigned)port_x86_64_nic_rx_frames(), (unsigned)port_x86_64_nic_tx_frames(),
                  (unsigned)(port_x86_64_nic_rx_dropped() + g_ip_dropped), (unsigned)g_ip_rx,
                  (unsigned)g_ip_tx);

    kshell_printf("  %u name(s) asked, %u from the cache, %u kept; %u fragment(s) dropped\n",
                  (unsigned)g_dns_queries, (unsigned)g_dns_hits, (unsigned)dns_cache_count(),
                  (unsigned)g_fragments_dropped);

    return KSHELL_OK;
}

/* The payload of the echo: 32 bytes of a pattern the reply has to carry. */
#define PING_PAYLOAD 32u

static bool icmp_ping(k_u32_t address, k_u32_t *rtt_ms)
{
    static k_u16_t sequence;
    k_u16_t identifier = 0x4D4Fu;
    k_u8_t request[NET_ICMP_HDR + PING_PAYLOAD];
    k_u64_t sent;
    k_u64_t deadline;
    k_size_t i;

    for (i = 0; i < PING_PAYLOAD; i++) {
        request[NET_ICMP_HDR + i] = (k_u8_t)('a' + (i % 26u));
    }

    request[0] = ICMP_ECHO_REQUEST;
    request[1] = 0u;
    net_put_be16(request + 2, 0u);
    net_put_be16(request + 4, identifier);
    net_put_be16(request + 6, ++sequence);
    net_put_be16(request + 2, net_checksum(request, sizeof(request)));

    g_icmp_answered = false;
    sent = k_port_uptime_ms();

    if (!net_send_ip(address, NET_IP_ICMP, request, sizeof(request))) {
        return false;
    }

    deadline = sent + NET_WAIT_DEFAULT_MS;

    while (k_port_uptime_ms() < deadline) {
        net_poll();

        if (g_icmp_answered && g_icmp_answer_length >= NET_ICMP_HDR &&
            g_icmp_answer[0] == ICMP_ECHO_REPLY && net_get_be16(g_icmp_answer + 4) == identifier &&
            net_get_be16(g_icmp_answer + 6) == sequence && g_icmp_from == address) {
            *rtt_ms = (k_u32_t)(k_port_uptime_ms() - sent);
            return true;
        }

        k_sched_sleep_ms(1);
    }

    return false;
}

static int cmd_ping(int argc, char **argv)
{
    k_u32_t address;
    k_u32_t rtt = 0;

    if (argc != 2 || !net_parse_ip(argv[1], &address)) {
        kshell_printf("usage: ping <address>\n");
        return KSHELL_USAGE;
    }

    if (!net_ready()) {
        kshell_printf("ping: there is no network device\n");
        return KSHELL_ERROR;
    }

    if (!icmp_ping(address, &rtt)) {
        kshell_printf("ping: no answer from %s\n", argv[1]);
        return KSHELL_ERROR;
    }

    kshell_printf("ping: %u bytes from %s, %u ms\n", (unsigned)(NET_ICMP_HDR + PING_PAYLOAD),
                  argv[1], (unsigned)rtt);

    return KSHELL_OK;
}

static int cmd_netudp(int argc, char **argv)
{
    k_u8_t answer[NET_UDP_PAYLOAD_MAX + 1u];
    k_u32_t address;
    k_u32_t port = 0;
    k_size_t i;
    int reply;

    if (argc != 4 || !net_parse_ip(argv[1], &address)) {
        kshell_printf("usage: netudp <address> <port> <text>\n");
        return KSHELL_USAGE;
    }

    for (i = 0; argv[2][i] != '\0'; i++) {
        if (argv[2][i] < '0' || argv[2][i] > '9') {
            kshell_printf("usage: netudp <address> <port> <text>\n");
            return KSHELL_USAGE;
        }

        port = port * 10u + (k_u32_t)(argv[2][i] - '0');

        if (port > 65535u) {
            kshell_printf("netudp: the port is not one\n");
            return KSHELL_ERROR;
        }
    }

    if (port == 0u || !net_ready()) {
        kshell_printf("netudp: there is no network device or no port\n");
        return KSHELL_ERROR;
    }

    reply = udp_exchange_address(address, (k_u16_t)port, argv[3], strlen(argv[3]), answer,
                                 sizeof(answer) - 1u, NET_WAIT_DEFAULT_MS);

    if (reply < 0) {
        kshell_printf("netudp: no answer from %s:%u\n", argv[1], (unsigned)port);
        return KSHELL_ERROR;
    }

    answer[reply] = '\0';
    kshell_printf("netudp: %d byte(s): %s\n", reply, (char *)answer);

    return KSHELL_OK;
}

/*
 * The command of the resolver: `dns <name>` asks for one address and says
 * where it came from, `dns` prints the names the cache keeps and their rest.
 * The list is a copy of the slots under the lock and not a pointer into them:
 * another task may resolve a name while this one prints.
 */
static int cmd_dns(int argc, char **argv)
{
    k_u32_t address;
    k_u32_t left = 0;

    if (argc > 2) {
        kshell_printf("usage: dns [name]\n");
        return KSHELL_USAGE;
    }

    if (!net_ready()) {
        kshell_printf("dns: there is no network device\n");
        return KSHELL_ERROR;
    }

    if (argc == 1) {
        k_u64_t now = k_port_uptime_ms();
        k_size_t shown = 0;
        k_size_t i;

        for (i = 0; i < DNS_CACHE_ENTRIES; i++) {
            char name[DNS_NAME_MAX];
            const char *entry;

            if (!net_lock()) {
                return KSHELL_ERROR;
            }

            entry = dns_cache_name(i, now, &address, &left);

            if (entry != NULL) {
                memcpy(name, entry, strlen(entry) + 1u);
            }

            net_unlock();

            if (entry == NULL) {
                continue;
            }

            kshell_printf("dns: %s is ", name);
            print_ip(address);
            kshell_printf(" (%u s left)\n", (unsigned)left);
            shown++;
        }

        if (shown == 0) {
            kshell_printf("dns: the cache is empty\n");
        }

        return KSHELL_OK;
    }

    if (net_parse_ip(argv[1], &address)) {
        kshell_printf("dns: %s is an address, not a name\n", argv[1]);
        return KSHELL_ERROR;
    }

    {
        bool cached = false;

        if (!net_resolve_cached(argv[1], &address, &cached)) {
            kshell_printf("dns: %s: no answer from the resolver\n", argv[1]);
            return KSHELL_ERROR;
        }

        if (cached) {
            (void)dns_cache_lookup(argv[1], k_port_uptime_ms(), NULL, &left);

            kshell_printf("dns: %s is ", argv[1]);
            print_ip(address);
            kshell_printf(" (from the cache, %u s left)\n", (unsigned)left);

            return KSHELL_OK;
        }
    }

    (void)dns_cache_lookup(argv[1], k_port_uptime_ms(), NULL, &left);

    kshell_printf("dns: %s is ", argv[1]);
    print_ip(address);

    if (left > 0) {
        kshell_printf(" (ttl %u s)\n", (unsigned)left);
    } else {
        kshell_printf("\n");
    }

    return KSHELL_OK;
}

int port_x86_64_net_register(void)
{
    int rc = 0;

    rc |= kshell_register("netinfo", "the network of the port: device, address, counters",
                          cmd_netinfo);
    rc |= kshell_register("ping", "test an address: ping <address>", cmd_ping);
    rc |= kshell_register("netudp", "one UDP exchange: netudp <address> <port> <text>", cmd_netudp);
    rc |= kshell_register("dns", "ask the resolver: dns [name]", cmd_dns);

    return rc;
}
