/*
 * Micro OS — MQTT 3.1.1 over the sockets of the kernel.
 *
 * The protocol is a fixed header, a remaining length in the variable-length
 * encoding of the specification and a body whose strings carry their length
 * in two bytes. There is no allocation: the packet that is being built lives
 * on the stack of the caller and the packet that is being read lives in the
 * client, because a board with a few kilobytes of free RAM is not the place
 * for a queue.
 *
 * Reads go through one helper that turns the "-1 after a timeout" of the
 * session into a deadline of our own: the caller waits no longer than
 * MQTT_TIMEOUT_MS for the bytes it asked for, whichever stack is under it.
 */
#include "net/mqtt.h"

#include <string.h>

#include "kernel/sched.h"
#include "kernel/sysinfo.h"

/* The pause after a session that answered "-1 but not yet out of time". */
#define MQTT_POLL_MS 10

/* The fields of a packet, as the reader hands them over. */
struct mqtt_packet {
    k_u8_t type;
    k_u8_t flags;
    k_u8_t *body;
    k_size_t length;
};

/* --- the bytes --- */

static k_size_t put_u16(k_u8_t *buf, k_u16_t value)
{
    buf[0] = (k_u8_t)(value >> 8);
    buf[1] = (k_u8_t)value;

    return 2;
}

/*
 * The remaining length: seven bits of the value per byte, the highest bit
 * says that another byte follows. Four bytes carry the whole of the range of
 * the specification; our packets are far below that.
 */
static k_size_t put_varint(k_u8_t *buf, k_size_t value)
{
    k_size_t used = 0;

    do {
        k_u8_t byte = (k_u8_t)(value % 128u);

        value /= 128u;
        if (value > 0) {
            byte |= 0x80;
        }

        buf[used++] = byte;
    } while (value > 0);

    return used;
}

/* An MQTT string: its length in two bytes and then the bytes themselves. */
static k_size_t put_string(k_u8_t *buf, const char *text, k_size_t length)
{
    put_u16(buf, (k_u16_t)length);
    memcpy(buf + 2, text, length);

    return 2 + length;
}

/*
 * Read exactly `length` bytes. The session answers -1 both for a real error
 * and for a silence of its own timeout, so the deadline is kept here and the
 * two are the same thing to the caller: a broker that did not answer.
 */
static int read_bytes(struct mqtt_client *client, void *buf, k_size_t length)
{
    k_u64_t deadline = k_sysinfo_uptime_ms() + MQTT_TIMEOUT_MS;
    k_u8_t *p = buf;
    k_size_t got = 0;

    while (got < length) {
        int n = k_socket_recv(client->socket, p + got, length - got);

        if (n > 0) {
            got += (k_size_t)n;
            continue;
        }

        if (n == 0) {
            return MQTT_ERROR; /* the broker closed the session */
        }

        if (k_sysinfo_uptime_ms() >= deadline) {
            return MQTT_NO_ANSWER;
        }

        /* A session may answer short of its timeout; do not spin on it. */
        k_sched_sleep_ms(MQTT_POLL_MS);
    }

    return MQTT_OK;
}

static int send_bytes(struct mqtt_client *client, const void *data, k_size_t length)
{
    if (k_socket_send(client->socket, data, length, true) != (int)length) {
        return MQTT_ERROR;
    }

    /* The clock of the keepalive follows every packet that goes out. */
    client->last_sent_ms = k_sysinfo_uptime_ms();

    return MQTT_OK;
}

/* One packet: the header byte, the remaining length and the body. */
static int read_packet(struct mqtt_client *client, struct mqtt_packet *out)
{
    k_u8_t header;
    k_u8_t byte;
    k_size_t remaining = 0;
    k_size_t multiplier = 1;
    int i;
    int rc;

    rc = read_bytes(client, &header, 1);
    if (rc != MQTT_OK) {
        return rc;
    }

    for (i = 0; i < 4; i++) {
        rc = read_bytes(client, &byte, 1);
        if (rc != MQTT_OK) {
            return rc;
        }

        remaining += (k_size_t)(byte & 0x7f) * multiplier;
        if ((byte & 0x80) == 0) {
            break;
        }

        multiplier *= 128u;
    }

    if (i == 4) {
        return MQTT_ERROR; /* a remaining length of five bytes is not one */
    }

    if (remaining > sizeof(client->packet)) {
        return MQTT_TOO_LONG;
    }

    if (remaining > 0) {
        rc = read_bytes(client, client->packet, remaining);
        if (rc != MQTT_OK) {
            return rc;
        }
    }

    out->type = (k_u8_t)(header >> 4);
    out->flags = (k_u8_t)(header & 0x0f);
    out->body = client->packet;
    out->length = remaining;

    return MQTT_OK;
}

static void close_client(struct mqtt_client *client)
{
    if (client->socket != NULL) {
        k_socket_close(client->socket);
    }

    memset(client, 0, sizeof(*client));
}

/* --- the packet id --- */

/*
 * A packet id is 1..65535 and never zero. One session has one packet in
 * flight at a time, so the counter alone is enough: no table of the ids in
 * use is kept.
 */
static k_u16_t next_packet_id(struct mqtt_client *client)
{
    client->next_packet_id++;
    if (client->next_packet_id == 0) {
        client->next_packet_id = 1;
    }

    return client->next_packet_id;
}

/*
 * Wait for the acknowledgement of `type` and `id`. A PINGRESP that arrives
 * here is the answer to an earlier keepalive and is not an error; anything
 * else that is not the packet we wait for is not part of this exchange.
 */
static int wait_for_ack(struct mqtt_client *client, k_u8_t type, k_u16_t id)
{
    k_u64_t deadline = k_sysinfo_uptime_ms() + MQTT_TIMEOUT_MS;
    struct mqtt_packet packet;
    int rc;

    for (;;) {
        if (k_sysinfo_uptime_ms() >= deadline) {
            return MQTT_NO_ANSWER;
        }

        rc = read_packet(client, &packet);
        if (rc != MQTT_OK) {
            return rc;
        }

        if (packet.type == 13) { /* PINGRESP */
            if (packet.length != 0) {
                return MQTT_ERROR;
            }
            if (type == 13) {
                return MQTT_OK;
            }
            continue;
        }

        if (packet.type != type) {
            continue;
        }

        if (packet.length < 2) {
            return MQTT_ERROR;
        }

        if ((((k_u16_t)packet.body[0] << 8) | packet.body[1]) != id) {
            return MQTT_ERROR;
        }

        /*
         * A SUBACK carries the granted QoS after the packet id; 0x80 is the
         * refusal of the subscription, and the caller has to know it.
         */
        if (type == 9) {
            if (packet.length < 3) {
                return MQTT_ERROR;
            }
            if (packet.body[2] == 0x80) {
                return MQTT_REFUSED;
            }
        }

        return MQTT_OK;
    }
}

/* --- the session --- */

int mqtt_connect(struct mqtt_client *client, const char *host, k_u16_t port, const char *client_id,
                 k_u16_t keepalive_s)
{
    k_u8_t packet[MQTT_PACKET_MAX];
    struct mqtt_packet answer;
    k_size_t id_length;
    k_size_t body_length;
    k_size_t pos = 0;
    int rc;

    if (client == NULL || host == NULL || client_id == NULL || port == 0) {
        return MQTT_INVALID;
    }

    id_length = strlen(client_id);
    if (id_length == 0 || id_length > MQTT_CLIENT_ID_MAX) {
        return MQTT_INVALID;
    }

    memset(client, 0, sizeof(*client));

    /* The protocol name, the level, the flags, the clock and the client id. */
    body_length = 2 + 4 + 1 + 1 + 2 + 2 + id_length;

    packet[pos++] = 0x10; /* CONNECT, flags zero */
    pos += put_varint(packet + pos, body_length);
    pos += put_string(packet + pos, "MQTT", 4);
    packet[pos++] = 4;    /* the level of 3.1.1 */
    packet[pos++] = 0x02; /* clean session: the board has no store of its own */
    pos += put_u16(packet + pos, keepalive_s);
    pos += put_string(packet + pos, client_id, id_length);

    client->socket = k_socket_connect(host, port, MQTT_TIMEOUT_MS);
    if (client->socket == NULL) {
        return MQTT_ERROR;
    }

    rc = send_bytes(client, packet, pos);
    if (rc != MQTT_OK) {
        close_client(client);
        return rc;
    }

    rc = read_packet(client, &answer);
    if (rc != MQTT_OK) {
        close_client(client);
        return rc;
    }

    if (answer.type != 2 || answer.length != 2) {
        close_client(client);
        return MQTT_ERROR;
    }

    if (answer.body[1] != 0) {
        close_client(client);
        return MQTT_REFUSED;
    }

    client->keepalive_s = keepalive_s;
    client->last_sent_ms = k_sysinfo_uptime_ms();
    client->connected = true;

    return MQTT_OK;
}

void mqtt_disconnect(struct mqtt_client *client)
{
    static const k_u8_t goodbye[2] = {0xe0, 0x00};

    if (client == NULL) {
        return;
    }

    if (client->socket != NULL && client->connected) {
        /* The answer to DISCONNECT is the close of the session, not a packet. */
        (void)send_bytes(client, goodbye, sizeof(goodbye));
    }

    close_client(client);
}

int mqtt_publish(struct mqtt_client *client, const char *topic, const void *payload, k_size_t len,
                 int qos)
{
    k_u8_t packet[MQTT_PACKET_MAX];
    k_size_t topic_length;
    k_size_t body_length;
    k_size_t pos = 0;
    k_u16_t id = 0;
    int rc;

    if (client == NULL || !client->connected || topic == NULL) {
        return MQTT_INVALID;
    }

    if (qos < 0 || qos > 1) {
        return MQTT_INVALID;
    }

    if (len > 0 && payload == NULL) {
        return MQTT_INVALID;
    }

    topic_length = strlen(topic);
    if (topic_length == 0 || topic_length > MQTT_TOPIC_MAX || len > MQTT_PAYLOAD_MAX) {
        return MQTT_TOO_LONG;
    }

    body_length = 2 + topic_length + len + (qos == 1 ? 2 : 0);

    /* The header is one byte and the length at most four. */
    if (body_length + 5 > sizeof(packet)) {
        return MQTT_TOO_LONG;
    }

    /* PUBLISH is the one packet whose flags carry the QoS of the message. */
    packet[pos++] = (k_u8_t)(0x30 | (qos == 1 ? 0x02 : 0x00));
    pos += put_varint(packet + pos, body_length);
    pos += put_string(packet + pos, topic, topic_length);

    if (qos == 1) {
        id = next_packet_id(client);
        pos += put_u16(packet + pos, id);
    }

    if (len > 0) {
        memcpy(packet + pos, payload, len);
        pos += len;
    }

    rc = send_bytes(client, packet, pos);
    if (rc != MQTT_OK) {
        return rc;
    }

    if (qos == 1) {
        return wait_for_ack(client, 4, id); /* PUBACK */
    }

    return MQTT_OK;
}

int mqtt_subscribe(struct mqtt_client *client, const char *topic, int qos)
{
    k_u8_t packet[MQTT_PACKET_MAX];
    k_size_t topic_length;
    k_size_t body_length;
    k_size_t pos = 0;
    k_u16_t id;
    int rc;

    if (client == NULL || !client->connected || topic == NULL) {
        return MQTT_INVALID;
    }

    if (qos < 0 || qos > 1) {
        return MQTT_INVALID;
    }

    topic_length = strlen(topic);
    if (topic_length == 0 || topic_length > MQTT_TOPIC_MAX) {
        return MQTT_TOO_LONG;
    }

    /* The packet id, the filter and the QoS that is asked for. */
    body_length = 2 + 2 + topic_length + 1;
    if (body_length + 5 > sizeof(packet)) {
        return MQTT_TOO_LONG;
    }

    id = next_packet_id(client);

    packet[pos++] = 0x82; /* SUBSCRIBE, the reserved flags are 0b0010 */
    pos += put_varint(packet + pos, body_length);
    pos += put_u16(packet + pos, id);
    pos += put_string(packet + pos, topic, topic_length);
    packet[pos++] = (k_u8_t)qos;

    rc = send_bytes(client, packet, pos);
    if (rc != MQTT_OK) {
        return rc;
    }

    return wait_for_ack(client, 9, id); /* SUBACK */
}

int mqtt_poll(struct mqtt_client *client, struct mqtt_message *message)
{
    struct mqtt_packet packet;
    k_size_t pos;
    k_size_t topic_length;
    k_size_t payload_length;
    k_u16_t id = 0;
    int qos;
    int rc;

    if (client == NULL || !client->connected || message == NULL) {
        return MQTT_INVALID;
    }

    rc = read_packet(client, &packet);
    if (rc != MQTT_OK) {
        return rc;
    }

    /*
     * The acknowledgements and the keepalive are the business of the calls
     * that asked for them. A packet that reaches the reader is a message.
     */
    if (packet.type != 3) {
        if (packet.type == 4 || packet.type == 9 || packet.type == 13) {
            return 0;
        }
        return MQTT_ERROR;
    }

    qos = (packet.flags >> 1) & 0x03;
    if (qos > 1) {
        /* QoS 2 has a handshake of its own, and the board does not need it. */
        return MQTT_INVALID;
    }

    if (packet.length < 2) {
        return MQTT_ERROR;
    }

    topic_length = ((k_size_t)packet.body[0] << 8) | packet.body[1];
    pos = 2;

    if (topic_length == 0 || topic_length > MQTT_TOPIC_MAX || pos + topic_length > packet.length) {
        return MQTT_TOO_LONG;
    }

    if (qos == 1) {
        if (pos + topic_length + 2 > packet.length) {
            return MQTT_ERROR;
        }
        id = (k_u16_t)(((k_u16_t)packet.body[pos + topic_length] << 8) |
                       packet.body[pos + topic_length + 1]);
    }

    /*
     * The packet id of a QoS 1 message sits between the topic and the
     * payload, so the payload starts two bytes further, not the topic.
     */
    pos += topic_length + (qos == 1 ? 2 : 0);

    payload_length = packet.length - pos;
    if (payload_length > MQTT_PAYLOAD_MAX) {
        return MQTT_TOO_LONG;
    }

    memcpy(message->topic, packet.body + 2, topic_length);
    message->topic[topic_length] = '\0';

    if (payload_length > 0) {
        memcpy(message->payload, packet.body + pos, payload_length);
    }
    message->payload[payload_length] = 0;
    message->length = payload_length;
    message->qos = qos;

    if (qos == 1) {
        k_u8_t ack[4];

        ack[0] = 0x40; /* PUBACK */
        ack[1] = 0x02;
        put_u16(ack + 2, id);

        rc = send_bytes(client, ack, sizeof(ack));
        if (rc != MQTT_OK) {
            return rc;
        }
    }

    return 1;
}

int mqtt_ping(struct mqtt_client *client)
{
    static const k_u8_t ping[2] = {0xc0, 0x00}; /* PINGREQ */

    if (client == NULL || !client->connected) {
        return MQTT_INVALID;
    }

    if (send_bytes(client, ping, sizeof(ping)) != MQTT_OK) {
        return MQTT_ERROR;
    }

    return wait_for_ack(client, 13, 0); /* PINGRESP */
}

int mqtt_keepalive(struct mqtt_client *client)
{
    k_u64_t interval_ms;
    k_u64_t elapsed_ms;
    int rc;

    if (client == NULL || !client->connected) {
        return MQTT_INVALID;
    }

    if (client->keepalive_s == 0) {
        return 0;
    }

    /*
     * Half of the interval, not all of it: the packet has to reach the broker
     * before the broker counts the session as silent, and a late PINGREQ is a
     * lost session.
     */
    interval_ms = (k_u64_t)client->keepalive_s * 1000u;
    elapsed_ms = k_sysinfo_uptime_ms() - client->last_sent_ms;

    if (elapsed_ms * 2u < interval_ms) {
        return 0;
    }

    rc = mqtt_ping(client);

    return rc == MQTT_OK ? 1 : rc;
}
