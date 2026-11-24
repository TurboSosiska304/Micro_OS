/*
 * Micro OS — the x86-64 port: a wire of the USB as the network, on the class
 * of CDC-ECM (ADR 0040, M1 of 1.8).
 *
 * A telephone that shares its connection over the cable, a dongle of
 * ethernet and a board of a laboratory all appear to a laptop as one of two
 * classes: CDC-ECM, which is the standard of the USB and speaks ethernet
 * frames over two bulk endpoints, and RNDIS, which is the same idea wrapped
 * in a protocol of Microsoft with a control channel of its own. This file
 * speaks the first: a control interface of class 2, sub-class 6, the
 * functional descriptor of the ethernet (the MAC is its string), a data
 * interface of class 10 whose alternate setting 1 carries the two bulk
 * endpoints, one class request to accept the frames
 * (SET_ETHERNET_PACKET_FILTER), and then a frame in and a frame out.
 *
 * RNDIS is not spoken and the reason is the one of the plan: a telephone
 * that offers ECM (the firmware of the gadget, a Linux, an iPhone) is the
 * standard, the device of QEMU carries ECM in its second configuration, and
 * the protocol of Microsoft - INITIALIZE, SET, QUERY, KEEPALIVE, a message
 * per control transfer - is a driver of a control channel larger than the
 * whole data path of this one, for a device that is a telephone and not a
 * card. A machine of a person with the RNDIS of Android is a row of
 * docs/debts.md.
 *
 * The notification endpoint of the control interface is not configured. It
 * carries the speed of the link and the notifications of the device, and this
 * stack has one speed and a cable that is in when the device is there: a
 * second endpoint to poll for a change nobody acts on is a cost of every
 * frame, and the device of QEMU sends nothing on it that the frames do not
 * say by themselves.
 *
 * The device is optional: a machine without one keeps the "no network" of
 * the contract, and the card of virtio of the PCI bus is still the first
 * candidate when both are there (src/netdev.c).
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "port_x86_64_net.h"
#include "port_x86_64_xhci.h"

/* The classes and the requests of CDC-ECM (the CDC specification 1.2). */
#define USB_CLASS_COMM 0x02u
#define USB_SUBCLASS_ETHERNET 0x06u
#define USB_CLASS_CDC_DATA 0x0Au

#define CDC_SET_ETHERNET_PACKET_FILTER 0x43u
#define CDC_PACKET_TYPE_ALL 0x000Fu

/* The functional descriptor of the ethernet: its type, its sub-type and the
 * offset of the index of the string of the MAC. */
#define USB_DT_CS_INTERFACE 0x24u
#define USB_CDC_ETHERNET_TYPE 0x0Fu
#define CDC_ETHERNET_MAC_STRING 3u

#define USB_DT_STRING 3u

/* The longest frame of the wire and the longest string of the device. */
#define USB_NET_FRAME_MAX PORT_X86_64_FRAME_MAX
#define USB_NET_STRING_MAX 256u

/* The wait of a frame in: the stack polls, so a device that says nothing for
 * a moment is not an error - it is a poll with no frame. */
#define USB_NET_RX_TIMEOUT_MS 2u
#define USB_NET_TX_TIMEOUT_MS 100u

struct usb_net {
    struct port_x86_64_usb_device *device;
    k_u8_t control_interface;
    k_u8_t data_interface;
    k_u8_t data_alt;
    k_u8_t mac[6];
    bool present;
    k_u64_t rx_frames;
    k_u64_t tx_frames;
    k_u64_t rx_dropped;
};

static struct usb_net g_usbnet;
static k_u8_t g_frame[USB_NET_FRAME_MAX] __attribute__((aligned(64)));
static k_u8_t g_string[USB_NET_STRING_MAX] __attribute__((aligned(64)));

static bool hex_digit(char character, k_u8_t *value)
{
    if (character >= '0' && character <= '9') {
        *value = (k_u8_t)(character - '0');
        return true;
    }

    if (character >= 'a' && character <= 'f') {
        *value = (k_u8_t)(character - 'a' + 10);
        return true;
    }

    if (character >= 'A' && character <= 'F') {
        *value = (k_u8_t)(character - 'A' + 10);
        return true;
    }

    return false;
}

/*
 * The MAC out of the string descriptor of the interface, which is the twelve
 * hexadecimal digits of the address in UTF-16: the answer of the request is
 * a descriptor (length, type 3) and then the characters, two bytes each.
 */
static bool string_mac(const k_u8_t *descriptor, k_size_t length, k_u8_t mac[6])
{
    k_u8_t digits[12];
    k_size_t index;
    k_size_t count = 0;

    if (length < 4u || descriptor[0] < 4u || descriptor[1] != USB_DT_STRING) {
        return false;
    }

    for (index = 2u; index + 1u < descriptor[0] && index + 1u < length; index += 2u) {
        char character = (char)descriptor[index];
        k_u8_t value;

        if (descriptor[index + 1u] != 0u) {
            continue;
        }

        if (!hex_digit(character, &value)) {
            continue;
        }

        if (count >= sizeof(digits)) {
            break;
        }

        digits[count++] = value;
    }

    if (count < sizeof(digits)) {
        return false;
    }

    for (index = 0; index < 6u; index++) {
        mac[index] = (k_u8_t)((digits[index * 2u] << 4) | digits[index * 2u + 1u]);
    }

    return true;
}

/*
 * The descriptors of the configuration: the control interface of CDC-ECM
 * with the descriptor of its ethernet (the string of the MAC) and the data
 * interface of the alt setting that carries the two bulk endpoints.
 */
struct cdc_ecm {
    k_u8_t control_interface;
    k_u8_t data_interface;
    k_u8_t data_alt;
    k_u8_t mac_string;
    k_u8_t bulk_in;
    k_u8_t bulk_out;
    k_u16_t in_packet;
    k_u16_t out_packet;
    bool found;
};

static void cdc_ecm_parse(const k_u8_t *bytes, k_u32_t length, struct cdc_ecm *ecm)
{
    k_u32_t offset = 0;
    k_u8_t interface = 0;
    bool in_control = false;
    bool in_data = false;

    memset(ecm, 0, sizeof(*ecm));

    while (offset + 2u <= length) {
        k_u8_t size = bytes[offset];
        k_u8_t type = bytes[offset + 1u];

        if (size < 2u || offset + size > length) {
            return;
        }

        if (type == 4u && size >= 9u) {
            k_u8_t base = bytes[offset + 5u];
            k_u8_t sub = bytes[offset + 6u];

            interface = bytes[offset + 2u];
            in_control = base == USB_CLASS_COMM && sub == USB_SUBCLASS_ETHERNET;
            in_data = base == USB_CLASS_CDC_DATA;

            if (in_control) {
                ecm->control_interface = interface;
            }

            if (in_data && bytes[offset + 4u] >= 2u) {
                ecm->data_interface = interface;
                ecm->data_alt = bytes[offset + 3u];
            }
        } else if (type == USB_DT_CS_INTERFACE && size >= 4u &&
                   bytes[offset + 2u] == USB_CDC_ETHERNET_TYPE && in_control) {
            ecm->mac_string = bytes[offset + CDC_ETHERNET_MAC_STRING];
        } else if (type == 5u && size >= 7u && in_data) {
            k_u8_t address = bytes[offset + 2u];
            k_u8_t attributes = bytes[offset + 3u];
            k_u16_t packet = (k_u16_t)(bytes[offset + 4u] | ((k_u16_t)bytes[offset + 5u] << 8));

            if ((attributes & 0x03u) == 0x02u) {
                if ((address & 0x80u) != 0u) {
                    ecm->bulk_in = address;
                    ecm->in_packet = packet;
                } else {
                    ecm->bulk_out = address;
                    ecm->out_packet = packet;
                }
            }
        }

        offset += size;
    }

    ecm->found = ecm->mac_string != 0u && ecm->bulk_in != 0u && ecm->bulk_out != 0u &&
                 ecm->in_packet != 0u && ecm->out_packet != 0u;
}

/* --- the seam the stack of src/net.c reads -------------------------------- */

static bool usbnet_nic_mac(k_u8_t mac[6])
{
    if (!g_usbnet.present) {
        return false;
    }

    memcpy(mac, g_usbnet.mac, sizeof(g_usbnet.mac));

    return true;
}

static bool usbnet_nic_link(void)
{
    return g_usbnet.present;
}

static bool usbnet_nic_receive(void *buf, k_size_t size, k_size_t *length)
{
    k_u32_t moved = 0;
    k_u32_t want;

    if (!g_usbnet.present || buf == NULL || length == NULL) {
        return false;
    }

    want = size > USB_NET_FRAME_MAX ? USB_NET_FRAME_MAX : (k_u32_t)size;

    if (port_x86_64_xhci_bulk(g_usbnet.device, true, g_frame, want, USB_NET_RX_TIMEOUT_MS,
                              &moved) != 0 ||
        moved == 0u) {
        return false;
    }

    if (moved > size) {
        g_usbnet.rx_dropped++;
        return false;
    }

    memcpy(buf, g_frame, moved);
    *length = moved;
    g_usbnet.rx_frames++;

    return true;
}

static bool usbnet_nic_send(const void *frame, k_size_t length)
{
    k_u32_t moved = 0;

    if (!g_usbnet.present || frame == NULL || length == 0u || length > USB_NET_FRAME_MAX) {
        return false;
    }

    if (port_x86_64_xhci_bulk(g_usbnet.device, false, (void *)(uintptr_t)frame, (k_u32_t)length,
                              USB_NET_TX_TIMEOUT_MS, &moved) != 0 ||
        moved != length) {
        return false;
    }

    g_usbnet.tx_frames++;

    return true;
}

static k_u64_t usbnet_nic_rx_frames(void)
{
    return g_usbnet.rx_frames;
}

static k_u64_t usbnet_nic_tx_frames(void)
{
    return g_usbnet.tx_frames;
}

static k_u64_t usbnet_nic_rx_dropped(void)
{
    return g_usbnet.rx_dropped;
}

static const struct port_x86_64_nic_ops g_usbnet_ops = {
    .name = "usbnet0",
    .mac = usbnet_nic_mac,
    .link = usbnet_nic_link,
    .receive = usbnet_nic_receive,
    .send = usbnet_nic_send,
    .rx_frames = usbnet_nic_rx_frames,
    .tx_frames = usbnet_nic_tx_frames,
    .rx_dropped = usbnet_nic_rx_dropped,
};

/* --- the attach of the class ---------------------------------------------- */

bool port_x86_64_usb_net_attach(struct port_x86_64_usb_device *device,
                                const struct port_x86_64_usb_config *config)
{
    struct cdc_ecm ecm;

    if (device == NULL || config == NULL || config->bytes == NULL) {
        return false;
    }

    cdc_ecm_parse(config->bytes, config->length, &ecm);

    if (!ecm.found) {
        kinfo("usb: port %u: the wire is not a CDC-ECM this driver reads", (unsigned)device->port);
        return false;
    }

    if (port_x86_64_xhci_endpoint(device, ecm.bulk_out, true, 0u, ecm.out_packet) != 0 ||
        port_x86_64_xhci_endpoint(device, ecm.bulk_in, true, 0u, ecm.in_packet) != 0) {
        kinfo("usb: port %u: the endpoints of the wire did not come up", (unsigned)device->port);
        return false;
    }

    memset(&g_usbnet, 0, sizeof(g_usbnet));
    g_usbnet.device = device;
    g_usbnet.control_interface = ecm.control_interface;
    g_usbnet.data_interface = ecm.data_interface;
    g_usbnet.data_alt = ecm.data_alt;
    device->interface = ecm.control_interface;

    /* The data interface of an ECM has a setting of no endpoints and the
     * setting that carries the two of them: the device is told which one. */
    if (ecm.data_alt != 0u && port_x86_64_xhci_control(device, 0x01u, 0x0Bu, ecm.data_alt,
                                                       ecm.data_interface, 0u, NULL) != 0) {
        kinfo("usb: port %u: the wire refused its alternate setting", (unsigned)device->port);
        return false;
    }

    if (port_x86_64_xhci_control(device, 0x80u, 6u,
                                 (k_u16_t)((USB_DT_STRING << 8) | ecm.mac_string), 0x0409u,
                                 USB_NET_STRING_MAX, g_string) != 0 ||
        !string_mac(g_string, sizeof(g_string), g_usbnet.mac)) {
        kinfo("usb: port %u: the wire has no MAC in its string", (unsigned)device->port);
        return false;
    }

    /* The frames of the wire are the ones the device has for the host: the
     * filter of the class, all of them. A device that refuses the request is
     * still a device whose data path is there. */
    if (port_x86_64_xhci_control(device, 0x21u, CDC_SET_ETHERNET_PACKET_FILTER, CDC_PACKET_TYPE_ALL,
                                 ecm.control_interface, 0u, NULL) != 0) {
        kwarn("usb: the wire did not take the packet filter, the frames are the ones it sends");
    }

    g_usbnet.present = true;

    kinfo("usbnet0: CDC-ECM %04x:%04x on port %u, %02x:%02x:%02x:%02x:%02x:%02x, "
          "bulk packets of %u bytes",
          (unsigned)device->vendor, (unsigned)device->product, (unsigned)device->port,
          (unsigned)g_usbnet.mac[0], (unsigned)g_usbnet.mac[1], (unsigned)g_usbnet.mac[2],
          (unsigned)g_usbnet.mac[3], (unsigned)g_usbnet.mac[4], (unsigned)g_usbnet.mac[5],
          (unsigned)((ecm.in_packet & 0x07FFu)));

    port_x86_64_nic_register(&g_usbnet_ops);

    return true;
}
