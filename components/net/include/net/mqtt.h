/*
 * Micro OS — an MQTT 3.1.1 client for the network of the owner.
 *
 * Plain TCP and no TLS: the flash has no room for a certificate store and the
 * board is on a home network (ADR 0003). The client speaks the smallest useful
 * part of 3.1.1 — CONNECT, PUBLISH (QoS 0 and 1), SUBSCRIBE, PING and
 * DISCONNECT — and reads one packet at a time into buffers of a fixed size.
 *
 * The sockets come from the kernel (kernel/net.h), so the same client runs
 * over lwIP on the board and over the sockets of the host in the tests. The
 * caller owns the client: the structure is small enough for the stack and the
 * client allocates nothing, which is the rule of the portable components.
 *
 * A session is synchronous. `mqtt_publish` with QoS 1 and `mqtt_subscribe`
 * wait for their acknowledgement, and a PUBLISH that arrives during such a
 * wait is dropped: the board has one message in flight, not a queue of them.
 * A program that subscribes and then reads in a loop sees every message.
 */
#ifndef MICRO_OS_NET_MQTT_H
#define MICRO_OS_NET_MQTT_H

#include "kernel/net.h"
#include "kernel/types.h"

/* The port of an unencrypted broker, and the wait of one exchange. */
#define MQTT_PORT 1883
#define MQTT_TIMEOUT_MS 5000

/* The bounds of the one message the client keeps. */
#define MQTT_TOPIC_MAX 128
#define MQTT_PAYLOAD_MAX 256
#define MQTT_CLIENT_ID_MAX 32

/* The largest packet the reader accepts: the body of a PUBLISH and its head. */
#define MQTT_PACKET_MAX (MQTT_TOPIC_MAX + MQTT_PAYLOAD_MAX + 32)

#define MQTT_OK 0
#define MQTT_ERROR (-1)     /* the session failed */
#define MQTT_INVALID (-2)   /* an argument is not one we speak */
#define MQTT_TOO_LONG (-3)  /* a topic, a payload or a packet does not fit */
#define MQTT_NO_ANSWER (-4) /* the broker said nothing in time */
#define MQTT_REFUSED (-5)   /* the broker refused the session or a subscription */

/*
 * The session and the packet buffer of the reader. A caller starts with
 * `mqtt_connect`, which fills the whole structure, and ends with
 * `mqtt_disconnect`; nothing else in it is for the caller to touch.
 */
struct mqtt_client {
    k_socket_t *socket;
    k_u8_t packet[MQTT_PACKET_MAX];
    k_u16_t next_packet_id;
    k_u16_t keepalive_s;
    k_u64_t last_sent_ms;
    bool connected;
};

/* One message that came from the broker, in the buffers of the client. */
struct mqtt_message {
    char topic[MQTT_TOPIC_MAX + 1];
    k_u8_t payload[MQTT_PAYLOAD_MAX + 1];
    k_size_t length;
    int qos;
};

/*
 * Open a session and wait for CONNACK. `keepalive_s` is the interval the
 * broker expects a packet within, in seconds (0 turns the clock off, which
 * is what the one-shot command wants). The client id has to be unique on the
 * broker; it is copied before the call returns.
 */
int mqtt_connect(struct mqtt_client *client, const char *host, k_u16_t port, const char *client_id,
                 k_u16_t keepalive_s);

/*
 * Send DISCONNECT (when the session is up) and close it. The structure is
 * cleared, so a second `mqtt_connect` may reuse it. NULL is not an error.
 */
void mqtt_disconnect(struct mqtt_client *client);

/*
 * Publish `len` bytes of `payload` on `topic`. `qos` is 0 or 1; with 1 the
 * call waits for the PUBACK of the broker. A zero-length payload is allowed
 * and NULL is only a payload when the length is zero.
 */
int mqtt_publish(struct mqtt_client *client, const char *topic, const void *payload, k_size_t len,
                 int qos);

/*
 * Subscribe to `topic` with `qos` (0 or 1) and wait for SUBACK. The messages
 * of the subscription arrive through `mqtt_poll`.
 */
int mqtt_subscribe(struct mqtt_client *client, const char *topic, int qos);

/*
 * Read one packet. Returns 1 and fills `message` for a PUBLISH (QoS 1 is
 * acknowledged here), 0 for a packet that is not a message (PUBACK, SUBACK,
 * PINGRESP) and a negative code on a failure. The call waits for the socket
 * timeout of the session before it answers MQTT_NO_ANSWER.
 */
int mqtt_poll(struct mqtt_client *client, struct mqtt_message *message);

/*
 * Send PINGREQ and wait for PINGRESP: the question that keeps the session
 * open and tells a broker that has gone away from one that is quiet.
 */
int mqtt_ping(struct mqtt_client *client);

/*
 * Send a PINGREQ when half of the keepalive has passed since the last packet
 * went out. Returns 1 when a ping was sent and answered, 0 when the clock has
 * not come round yet, and a negative code on a failure.
 */
int mqtt_keepalive(struct mqtt_client *client);

#endif /* MICRO_OS_NET_MQTT_H */
