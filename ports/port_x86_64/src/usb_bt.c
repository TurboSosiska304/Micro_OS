/*
 * Micro OS — the x86-64 port: a Bluetooth controller over the USB (HCI).
 *
 * A laptop's Bluetooth radio is often the USB function of the same M.2 card
 * as the Wi-Fi. It speaks the HCI over the USB transport (Bluetooth Core
 * Spec, Vol 4 Part B): class 0xE0/0x01/0x01, commands on EP0 as class control
 * transfers, events on an interrupt IN endpoint, ACL on bulk endpoints.
 *
 * This first slice only proves the controller answers: it finds the
 * interface, configures its endpoints, sends HCI_Reset, Read Local Version
 * Information and Read BD_ADDR, and logs the identity. It never loads
 * firmware, never enables scanning, and never changes the radio state beyond
 * the reset the specification requires before use.
 */
#include "port_x86_64_bt.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kernel/log.h"

#include "port_x86_64_xhci.h"

#define BT_HCI_EVT_BUF 260u
#define BT_HCI_CMD_TIMEOUT_MS 500u
#define BT_HCI_EVT_TIMEOUT_MS 1000u

struct bt_usb_ep {
    k_u8_t address;
    k_u8_t attributes;
    k_u16_t max_packet;
    k_u8_t interval;
};

struct bt_usb_if {
    bool found;
    k_u8_t interface;
    struct bt_usb_ep intr_in;
    struct bt_usb_ep bulk_in;
    struct bt_usb_ep bulk_out;
};

static struct port_x86_64_bt_state g_bt_state;
static bool g_bt_registered;
static kmod_state_t g_bt_status = KMOD_SKIP;
static char g_bt_info[KMOD_INFO_MAX] = "no Bluetooth controller found";

static k_u8_t g_hci_cmd[8] __attribute__((aligned(64)));
static k_u8_t g_hci_evt[BT_HCI_EVT_BUF] __attribute__((aligned(64)));

static int bt_check(void)
{
    if (g_bt_status == KMOD_OK) {
        return 0;
    }
    if (g_bt_status == KMOD_DEGRADED) {
        return 1;
    }
    if (g_bt_status == KMOD_SKIP) {
        return 2;
    }
    return -1;
}

static const char *bt_detail(void)
{
    return g_bt_info;
}

static void bt_set_kmod(kmod_state_t state, const char *info)
{
    k_size_t i;

    g_bt_status = state;
    if (info == NULL) {
        g_bt_info[0] = '\0';
    } else {
        snprintf(g_bt_info, sizeof(g_bt_info), "%s", info);
    }

    for (i = 0; i < kmod_count(); i++) {
        const struct kmod *mod = kmod_get(i);
        if (mod != NULL && mod->name != NULL && strcmp(mod->name, "bt") == 0) {
            struct kmod *mw = (struct kmod *)mod;
            mw->state = state;
            snprintf(mw->info, sizeof(mw->info), "%s", g_bt_info);
            break;
        }
    }
}

const struct port_x86_64_bt_state *port_x86_64_bt_state(void)
{
    return &g_bt_state;
}

const char *port_x86_64_bt_kmod_state_name(void)
{
    if (!g_bt_registered) {
        return kmod_state_name(KMOD_UNKNOWN);
    }
    return kmod_state_name(g_bt_status);
}

static void bt_parse_config(const struct port_x86_64_usb_config *config, struct bt_usb_if *out)
{
    k_u32_t offset = 0;
    bool in_bt = false;
    k_u8_t current_if = 0;

    memset(out, 0, sizeof(*out));

    if (config == NULL || config->bytes == NULL) {
        return;
    }

    while (offset + 2u <= config->length) {
        k_u8_t len = config->bytes[offset];
        k_u8_t type = config->bytes[offset + 1u];

        if (len < 2u || offset + len > config->length) {
            break;
        }

        if (type == 4u && len >= 9u) {
            /* Interface descriptor. */
            current_if = config->bytes[offset + 2u];
            in_bt = config->bytes[offset + 5u] == 0xE0u && config->bytes[offset + 6u] == 0x01u &&
                    config->bytes[offset + 7u] == 0x01u;
            if (in_bt && !out->found) {
                out->found = true;
                out->interface = current_if;
            } else if (in_bt) {
                /* keep parsing first BT interface only */
            } else if (out->found) {
                /* moved past our BT interface */
                in_bt = false;
            }
        } else if (type == 5u && len >= 7u && in_bt && out->found && current_if == out->interface) {
            struct bt_usb_ep ep;
            k_u8_t ep_type;

            ep.address = config->bytes[offset + 2u];
            ep.attributes = config->bytes[offset + 3u];
            ep.max_packet = (k_u16_t)config->bytes[offset + 4u] |
                            (k_u16_t)((k_u16_t)config->bytes[offset + 5u] << 8);
            ep.interval = config->bytes[offset + 6u];
            ep_type = ep.attributes & 0x03u;

            if ((ep.address & 0x80u) != 0u && ep_type == 0x03u) {
                out->intr_in = ep;
            } else if ((ep.address & 0x80u) != 0u && ep_type == 0x02u) {
                out->bulk_in = ep;
            } else if ((ep.address & 0x80u) == 0u && ep_type == 0x02u) {
                out->bulk_out = ep;
            }
        }

        offset += (k_u32_t)len;
    }
}

static bool bt_send_cmd(struct port_x86_64_usb_device *device, k_u16_t opcode, const void *params,
                        k_u8_t param_len)
{
    k_u32_t cmd_len = (k_u32_t)param_len + 3u;

    if (cmd_len > sizeof(g_hci_cmd)) {
        return false;
    }

    g_hci_cmd[0] = (k_u8_t)(opcode & 0xFFu);
    g_hci_cmd[1] = (k_u8_t)(opcode >> 8);
    g_hci_cmd[2] = param_len;
    if (param_len != 0u && params != NULL) {
        memcpy(g_hci_cmd + 3u, params, param_len);
    }

    /* HCI command over USB: class request, Host-to-Device, Device recipient.
     * Bluetooth Core Spec, Vol 4 Part B, Section 2.2. */
    return port_x86_64_xhci_control(device, 0x20u, 0x00u, 0x0000u, 0x0000u, (k_u16_t)cmd_len,
                                    g_hci_cmd) == 0;
}

static bool bt_wait_event(struct port_x86_64_usb_device *device, k_u16_t opcode, k_u8_t *event_code,
                          k_size_t *event_len)
{
    k_u32_t moved = 0;
    int rc;

    rc = port_x86_64_xhci_interrupt(device, g_hci_evt, sizeof(g_hci_evt), BT_HCI_EVT_TIMEOUT_MS,
                                    &moved);
    if (rc != 0 || moved < 2u) {
        return false;
    }

    if (event_code != NULL) {
        *event_code = g_hci_evt[0];
    }
    if (event_len != NULL) {
        *event_len = (k_size_t)moved;
    }

    /* If it's a Command Complete, check opcode. Other events are ignored by
     * this simple handshake. */
    if (g_hci_evt[0] == BT_HCI_EVT_COMMAND_COMPLETE) {
        if (moved < 6u) {
            return false;
        }
        {
            k_u16_t got = (k_u16_t)g_hci_evt[3] | (k_u16_t)((k_u16_t)g_hci_evt[4] << 8);
            if (got != opcode) {
                return false;
            }
        }
    }

    return true;
}

static bool bt_cmd_complete(struct port_x86_64_usb_device *device, k_u16_t opcode,
                            k_size_t *event_len)
{
    k_u8_t code = 0;
    k_size_t len = 0;

    if (!bt_send_cmd(device, opcode, NULL, 0u)) {
        return false;
    }

    if (!bt_wait_event(device, opcode, &code, &len)) {
        return false;
    }

    if (code != BT_HCI_EVT_COMMAND_COMPLETE) {
        return false;
    }

    if (event_len != NULL) {
        *event_len = len;
    }

    return true;
}

void port_x86_64_bt_init(void)
{
    struct kmod mod;

    memset(&g_bt_state, 0, sizeof(g_bt_state));
    g_bt_status = KMOD_SKIP;
    snprintf(g_bt_info, sizeof(g_bt_info), "no Bluetooth controller found");

    if (g_bt_registered) {
        bt_set_kmod(KMOD_SKIP, g_bt_info);
        return;
    }

    memset(&mod, 0, sizeof(mod));
    mod.name = "bt";
    mod.desc = "Bluetooth HCI (USB)";
    mod.check = bt_check;
    mod.detail = bt_detail;

    if (kmod_register(&mod) == 0) {
        g_bt_registered = true;
        bt_set_kmod(KMOD_SKIP, g_bt_info);
    } else {
        g_bt_registered = true;
    }
}

bool port_x86_64_usb_bt_attach(struct port_x86_64_usb_device *device,
                               const struct port_x86_64_usb_config *config)
{
    struct bt_usb_if bt;
    struct bt_hci_version version;
    struct bt_hci_bdaddr bdaddr;
    k_size_t evt_len = 0;
    k_u8_t status = 0;
    char addr_str[18];
    char info[96];

    if (device == NULL || config == NULL) {
        return false;
    }

    bt_parse_config(config, &bt);
    if (!bt.found) {
        return false;
    }

    device->interface = bt.interface;

    if (bt.intr_in.address == 0u) {
        kwarn("bt: %04x:%04x has no interrupt IN endpoint", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "no interrupt IN endpoint");
        return false;
    }

    if (port_x86_64_xhci_endpoint(device, bt.intr_in.address, false, bt.intr_in.interval,
                                  bt.intr_in.max_packet) != 0) {
        kwarn("bt: %04x:%04x interrupt endpoint configure failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "interrupt endpoint failed");
        return false;
    }

    if (bt.bulk_in.address != 0u) {
        (void)port_x86_64_xhci_endpoint(device, bt.bulk_in.address, true, 0u,
                                        bt.bulk_in.max_packet);
    }
    if (bt.bulk_out.address != 0u) {
        (void)port_x86_64_xhci_endpoint(device, bt.bulk_out.address, true, 0u,
                                        bt.bulk_out.max_packet);
    }

    /* HCI Reset. */
    if (!bt_cmd_complete(device, BT_HCI_OP_RESET, &evt_len) ||
        !port_x86_64_bt_parse_reset(g_hci_evt, evt_len, &status)) {
        kwarn("bt: %04x:%04x HCI_Reset failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "HCI_Reset failed");
        return false;
    }

    /* Read Local Version Information. */
    memset(&version, 0, sizeof(version));
    if (!bt_cmd_complete(device, BT_HCI_OP_READ_LOCAL_VERSION, &evt_len) ||
        !port_x86_64_bt_parse_local_version(g_hci_evt, evt_len, &version)) {
        kwarn("bt: %04x:%04x Read Local Version failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "Read Local Version failed");
        return false;
    }

    /* Read BD_ADDR. */
    memset(&bdaddr, 0, sizeof(bdaddr));
    if (!bt_cmd_complete(device, BT_HCI_OP_READ_BD_ADDR, &evt_len) ||
        !port_x86_64_bt_parse_bdaddr(g_hci_evt, evt_len, &bdaddr)) {
        kwarn("bt: %04x:%04x Read BD_ADDR failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "Read BD_ADDR failed");
        return false;
    }

    port_x86_64_bt_format_bdaddr(bdaddr.addr, addr_str, sizeof(addr_str));

    g_bt_state.present = true;
    g_bt_state.vendor = device->vendor;
    g_bt_state.product = device->product;
    g_bt_state.hci_version = version.hci_version;
    g_bt_state.hci_revision = version.hci_revision;
    g_bt_state.manufacturer = version.manufacturer;
    memcpy(g_bt_state.bdaddr, bdaddr.addr, BT_HCI_BDADDR_LEN);
    snprintf(g_bt_state.name, sizeof(g_bt_state.name), "USB BT %04x:%04x", (unsigned)device->vendor,
             (unsigned)device->product);

    snprintf(info, sizeof(info), "HCI %u.%u rev 0x%04x mfg 0x%04x %s",
             (unsigned)version.hci_version, (unsigned)(version.hci_revision & 0xFFu),
             (unsigned)version.hci_revision, (unsigned)version.manufacturer, addr_str);
    bt_set_kmod(KMOD_OK, info);

    kinfo("bt: %04x:%04x HCI %u.%u rev 0x%04x manufacturer 0x%04x BD_ADDR %s",
          (unsigned)device->vendor, (unsigned)device->product, (unsigned)version.hci_version,
          (unsigned)(version.hci_revision & 0xFFu), (unsigned)version.hci_revision,
          (unsigned)version.manufacturer, addr_str);

    return true;
}

void port_x86_64_usb_bt_detach(struct port_x86_64_usb_device *device)
{
    (void)device;
    memset(&g_bt_state, 0, sizeof(g_bt_state));
    bt_set_kmod(KMOD_SKIP, "no Bluetooth controller found");
    kinfo("bt: controller removed");
}
