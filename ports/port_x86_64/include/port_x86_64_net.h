/*
 * Micro OS — the x86-64 port: the ethernet device under the network stack.
 *
 * This is the seam between the driver of one controller (src/virtio_net.c)
 * and the protocol stack of the port (src/net.c): a frame in, a frame out,
 * the MAC, the link, and nothing about virtqueues or PCI. The stack does not
 * know which controller answered, and a second driver for this machine is
 * another file behind these five calls.
 *
 * The device is polled: the receive queue is asked whether a frame is there
 * and the transmit path waits for the used ring of its one descriptor. The
 * interrupts of the PCI line stay masked, which is the same decision the
 * disk of this port made (ADR 0008): one driver at a time, no vector to
 * route, and the timer of the machine is the only thing that wakes a wait.
 */
#ifndef MICRO_OS_PORT_X86_64_NET_H
#define MICRO_OS_PORT_X86_64_NET_H

#include "kernel/types.h"

/* The largest frame the driver hands over or takes: the ethernet MTU, the
 * header of the frame and no VLAN tag. */
#define PORT_X86_64_FRAME_MAX 1518u

/*
 * The operations of one controller under the stack. A driver registers one
 * of these when it brings its device up (`src/netdev.c`), and the calls
 * below are the stack's view of the first one that did: the virtio card of
 * the PCI bus or the wire of the USB (ADR 0040). A machine has one network
 * device of the release, and a driver that arrives second does not take the
 * stack from the one that is there.
 */
struct port_x86_64_nic_ops {
    const char *name;
    bool (*mac)(k_u8_t mac[6]);
    bool (*link)(void);
    bool (*receive)(void *buf, k_size_t size, k_size_t *length);
    bool (*send)(const void *frame, k_size_t length);
    k_u64_t (*rx_frames)(void);
    k_u64_t (*tx_frames)(void);
    k_u64_t (*rx_dropped)(void);
};

/* Offer a controller to the stack. The first registration wins; the pointer
 * has to live as long as the system does. */
void port_x86_64_nic_register(const struct port_x86_64_nic_ops *ops);

/* The name of the device, "virtio0" or "usbnet0", or NULL when the machine
 * has none. */
const char *port_x86_64_nic_name(void);

/* True when the device was found, brought up and has its queues. */
bool port_x86_64_nic_present(void);

/* The MAC the device gave the driver; false when there is no device. */
bool port_x86_64_nic_mac(k_u8_t mac[6]);

/* The link of the device: true when QEMU says the virtual cable is in. */
bool port_x86_64_nic_link(void);

/*
 * One frame from the receive queue, copied into `buf` (at most `size`
 * bytes). Returns false when the queue is empty or the frame does not fit;
 * `length` is the bytes of the frame when true.
 */
bool port_x86_64_nic_receive(void *buf, k_size_t size, k_size_t *length);

/* Hand one frame to the transmit queue and wait for the device to take it. */
bool port_x86_64_nic_send(const void *frame, k_size_t length);

/* The counters of the driver, for `netinfo`: frames taken and given. */
k_u64_t port_x86_64_nic_rx_frames(void);
k_u64_t port_x86_64_nic_tx_frames(void);
k_u64_t port_x86_64_nic_rx_dropped(void);

#endif /* MICRO_OS_PORT_X86_64_NET_H */
