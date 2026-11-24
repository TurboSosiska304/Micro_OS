/*
 * Micro OS — the x86-64 port: the network device under the stack.
 *
 * The stack of `src/net.c` speaks to one seam - a name, a MAC, the link, a
 * frame in and a frame out - and since 1.8 there are two drivers behind it:
 * the card of virtio on the PCI bus (`src/virtio_net.c`) and the wire of the
 * USB (`src/usb_net.c`, the class of CDC-ECM of ADR 0040). This file is the
 * seam: the first driver that brings its device up registers its operations
 * and the stack reads them; the driver that arrives second leaves the stack
 * to the one that is there, because a machine of the release has one network
 * of its own and a stick of the network is the second candidate and not a
 * second address.
 *
 * The card of the PCI bus is probed before the controller of the USB is
 * enumerated (the port's boot calls `port_x86_64_net_init()` before the
 * interrupts and the xHCI after them), so a machine with both has the card
 * and a machine with only the dongle has the dongle.
 */
#include "port_x86_64_net.h"

static const struct port_x86_64_nic_ops *g_nic;

void port_x86_64_nic_register(const struct port_x86_64_nic_ops *ops)
{
    if (g_nic == NULL && ops != NULL) {
        g_nic = ops;
    }
}

const char *port_x86_64_nic_name(void)
{
    return g_nic != NULL ? g_nic->name : NULL;
}

bool port_x86_64_nic_present(void)
{
    return g_nic != NULL;
}

bool port_x86_64_nic_mac(k_u8_t mac[6])
{
    if (g_nic == NULL || g_nic->mac == NULL || mac == NULL) {
        return false;
    }

    return g_nic->mac(mac);
}

bool port_x86_64_nic_link(void)
{
    if (g_nic == NULL || g_nic->link == NULL) {
        return false;
    }

    return g_nic->link();
}

bool port_x86_64_nic_receive(void *buf, k_size_t size, k_size_t *length)
{
    if (g_nic == NULL || g_nic->receive == NULL) {
        return false;
    }

    return g_nic->receive(buf, size, length);
}

bool port_x86_64_nic_send(const void *frame, k_size_t length)
{
    if (g_nic == NULL || g_nic->send == NULL) {
        return false;
    }

    return g_nic->send(frame, length);
}

k_u64_t port_x86_64_nic_rx_frames(void)
{
    return g_nic != NULL && g_nic->rx_frames != NULL ? g_nic->rx_frames() : 0u;
}

k_u64_t port_x86_64_nic_tx_frames(void)
{
    return g_nic != NULL && g_nic->tx_frames != NULL ? g_nic->tx_frames() : 0u;
}

k_u64_t port_x86_64_nic_rx_dropped(void)
{
    return g_nic != NULL && g_nic->rx_dropped != NULL ? g_nic->rx_dropped() : 0u;
}
