/*
 * Micro OS — the x86-64 port: a Bluetooth controller over the USB (HCI).
 *
 * A laptop's Bluetooth radio is often the USB function of the same M.2 card
 * as the Wi-Fi. It speaks the HCI over the USB transport (Bluetooth Core
 * Spec, Vol 4 Part B): class 0xE0/0x01/0x01, commands on EP0 as class control
 * transfers, events on an interrupt IN endpoint, ACL on bulk endpoints.
 *
 * This slice proves the controller answers, reads its identity and exposes
 * Reset, Write Local Name, Inquiry and Inquiry Cancel. It does not load
 * firmware, collect inquiry results, open connections or implement L2CAP.
 */
#include "port_x86_64_bt.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/port/time.h"
#include "kshell/kshell.h"

#include "port_x86_64_xhci.h"

#define BT_HCI_EVT_BUF 260u
#define BT_HCI_CMD_BUF 258u
#define BT_HCI_EVT_TIMEOUT_MS 1000u
#define BT_HCI_INQUIRY_LENGTH 8u

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
static struct port_x86_64_usb_device g_bt_device;
static bool g_bt_registered;
static bool g_bt_command_registered;
static kmod_state_t g_bt_status = KMOD_SKIP;
static char g_bt_info[KMOD_INFO_MAX] = "no Bluetooth controller found";
static k_mutex_t g_bt_lock = K_MUTEX_INITIALIZER;

static k_u8_t g_hci_cmd[BT_HCI_CMD_BUF] __attribute__((aligned(64)));
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

static const struct kmod *bt_find_kmod(void)
{
    k_size_t i;

    for (i = 0; i < kmod_count(); i++) {
        const struct kmod *mod = kmod_get(i);
        if (mod != NULL && mod->name != NULL && strcmp(mod->name, "bt") == 0) {
            return mod;
        }
    }

    return NULL;
}

static void bt_set_kmod(kmod_state_t state, const char *info)
{
    const struct kmod *mod = bt_find_kmod();

    g_bt_status = state;
    if (info == NULL) {
        g_bt_info[0] = '\0';
    } else {
        snprintf(g_bt_info, sizeof(g_bt_info), "%s", info);
    }

    if (mod != NULL) {
        struct kmod *mw = (struct kmod *)mod;
        mw->state = state;
        snprintf(mw->info, sizeof(mw->info), "%s", g_bt_info);
    }
}

static void bt_mark_ready(void)
{
    char info[KMOD_INFO_MAX];

    snprintf(info, sizeof(info), "HCI %u rev 0x%04x mfg 0x%04x", (unsigned)g_bt_state.hci_version,
             (unsigned)g_bt_state.hci_revision, (unsigned)g_bt_state.manufacturer);
    bt_set_kmod(g_bt_state.bulk_in_open && g_bt_state.bulk_out_open ? KMOD_OK : KMOD_DEGRADED,
                info);
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

    if (device == NULL || cmd_len > sizeof(g_hci_cmd) || (param_len != 0u && params == NULL)) {
        return false;
    }

    g_hci_cmd[0] = (k_u8_t)(opcode & 0xFFu);
    g_hci_cmd[1] = (k_u8_t)(opcode >> 8);
    g_hci_cmd[2] = param_len;
    if (param_len != 0u) {
        memcpy(g_hci_cmd + 3u, params, param_len);
    }

    /* HCI command over USB: class request, Host-to-Device, Device recipient.
     * Bluetooth Core Spec, Vol 4 Part B, Section 2.2. */
    return port_x86_64_xhci_control(device, 0x20u, 0x00u, 0x0000u, 0x0000u, (k_u16_t)cmd_len,
                                    g_hci_cmd) == 0;
}

static void bt_dispatch_event(const struct bt_hci_event *evt)
{
    k_size_t i;

    switch (evt->code) {
    case BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS:
        for (i = 0; i < evt->completed_count; i++) {
            k_u32_t accounted = 0;
            k_u16_t completed = evt->completed[i].completed;

            g_bt_state.acl_outstanding =
                port_x86_64_bt_acl_account(g_bt_state.acl_outstanding, completed, &accounted);
            g_bt_state.acl_completed += accounted;
            if (accounted != completed) {
                kwarn("bt: unsolicited ACL completion for handle 0x%04x (%u packets)",
                      (unsigned)evt->completed[i].handle, (unsigned)completed);
            }
            kdebug("bt: ACL handle 0x%04x completed %u packet(s), %u outstanding",
                   (unsigned)evt->completed[i].handle, (unsigned)accounted,
                   (unsigned)g_bt_state.acl_outstanding);
        }
        break;
    case BT_HCI_EVT_HARDWARE_ERROR: {
        char info[KMOD_INFO_MAX];

        snprintf(info, sizeof(info), "hardware error 0x%02x", (unsigned)evt->hardware_code);
        bt_set_kmod(KMOD_FAIL, info);
        kwarn("bt: controller hardware error 0x%02x", (unsigned)evt->hardware_code);
        break;
    }
    case BT_HCI_EVT_DISCONNECTION_COMPLETE:
        kinfo("bt: disconnection handle 0x%04x status 0x%02x reason 0x%02x", (unsigned)evt->handle,
              (unsigned)evt->status, (unsigned)evt->reason);
        break;
    default:
        break;
    }
}

static bool bt_read_event(struct port_x86_64_usb_device *device, struct bt_hci_event *evt,
                          k_u32_t timeout_ms)
{
    k_u32_t moved = 0;
    int rc;

    rc = port_x86_64_xhci_interrupt(device, g_hci_evt, sizeof(g_hci_evt), timeout_ms, &moved);
    if (rc != 0 || moved < 2u || moved > sizeof(g_hci_evt)) {
        return false;
    }

    if (port_x86_64_bt_event_info(g_hci_evt[0]) == NULL) {
        kdebug("bt: ignoring unsupported event 0x%02x", (unsigned)g_hci_evt[0]);
        return false;
    }

    if (!port_x86_64_bt_parse_event(g_hci_evt, moved, evt)) {
        kwarn("bt: malformed event 0x%02x (declared %u bytes, received %u)", (unsigned)g_hci_evt[0],
              (unsigned)g_hci_evt[1], (unsigned)moved);
        return false;
    }

    bt_dispatch_event(evt);
    return true;
}

static bool bt_wait_response(struct port_x86_64_usb_device *device, k_u16_t opcode, k_u8_t *status)
{
    k_u64_t started = k_port_uptime_ms();

    for (;;) {
        k_u64_t now = k_port_uptime_ms();
        k_u32_t remaining = BT_HCI_EVT_TIMEOUT_MS;
        struct bt_hci_event evt;

        if (now - started >= BT_HCI_EVT_TIMEOUT_MS) {
            return false;
        }
        if (BT_HCI_EVT_TIMEOUT_MS - (k_u32_t)(now - started) < remaining) {
            remaining = BT_HCI_EVT_TIMEOUT_MS - (k_u32_t)(now - started);
        }
        if (!bt_read_event(device, &evt, remaining)) {
            continue;
        }

        if (evt.code != BT_HCI_EVT_COMMAND_COMPLETE && evt.code != BT_HCI_EVT_COMMAND_STATUS) {
            continue;
        }
        if (evt.opcode != opcode) {
            continue;
        }

        if (evt.code == BT_HCI_EVT_COMMAND_COMPLETE) {
            /* Every command in this driver returns Status as its first return
             * parameter. Command Complete itself only guarantees the opcode. */
            if (g_hci_evt[1] < 4u) {
                kwarn("bt: command 0x%04x completed without a status", (unsigned)opcode);
                continue;
            }
            *status = g_hci_evt[5];
        } else {
            *status = evt.status;
        }

        return true;
    }
}

static bool bt_command(struct port_x86_64_usb_device *device, k_u16_t opcode, const void *params,
                       k_u8_t param_len, k_u8_t *status)
{
    if (status == NULL || !bt_send_cmd(device, opcode, params, param_len)) {
        return false;
    }

    return bt_wait_response(device, opcode, status);
}

static int cmd_bt_info(void)
{
    const struct kmod *mod;
    const struct port_x86_64_bt_state *state = port_x86_64_bt_state();
    kmod_state_t module_state;
    char addr[18];

    if (k_mutex_lock(&g_bt_lock) != 0) {
        kshell_printf("bt: controller is busy\n");
        return KSHELL_ERROR;
    }

    mod = bt_find_kmod();
    if (mod == NULL) {
        kshell_printf("bt: module not registered\n");
        kshell_printf("bt: state unknown\n");
        (void)k_mutex_unlock(&g_bt_lock);
        return KSHELL_ERROR;
    }

    module_state = mod->state;
    kshell_printf("bt: state %s\n", kmod_state_name(module_state));
    if (mod->info[0] != '\0') {
        kshell_printf("bt: %s\n", mod->info);
    }

    if (state->present) {
        port_x86_64_bt_format_bdaddr(state->bdaddr, addr, sizeof(addr));
        kshell_printf("bt: USB %04x:%04x\n", (unsigned)state->vendor, (unsigned)state->product);
        kshell_printf("bt: BD_ADDR %s\n", addr);
        kshell_printf("bt: local name: %s\n", state->name_known ? state->name : "not read");
        kshell_printf("bt: inquiry requested: %s\n", state->inquiry_requested ? "yes" : "no");
        kshell_printf("bt: ACL bulk IN %s, OUT %s\n", state->bulk_in_open ? "open" : "closed",
                      state->bulk_out_open ? "open" : "closed");
        kshell_printf("bt: ACL outstanding %u, completed %u\n", (unsigned)state->acl_outstanding,
                      (unsigned)state->acl_completed);
        kshell_printf("bt: ACL data not tested: no connection, L2CAP or GATT\n");
    } else {
        kshell_printf("bt: ACL data not tested: no controller\n");
    }

    (void)k_mutex_unlock(&g_bt_lock);
    return module_state == KMOD_OK || module_state == KMOD_SKIP ? KSHELL_OK : KSHELL_ERROR;
}

static int cmd_bt(int argc, char **argv)
{
    k_u8_t status = 0;
    bool responded = false;
    int rc = KSHELL_ERROR;

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        return cmd_bt_info();
    }

    if (k_mutex_lock(&g_bt_lock) != 0) {
        kshell_printf("bt: controller is busy\n");
        return KSHELL_ERROR;
    }

    if (!g_bt_state.present) {
        kshell_printf("bt: no Bluetooth controller found\n");
        goto out;
    }

    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        responded = bt_command(&g_bt_device, BT_HCI_OP_RESET, NULL, 0u, &status);
        if (responded && status == 0u) {
            g_bt_state.name_known = false;
            g_bt_state.name[0] = '\0';
            g_bt_state.inquiry_requested = false;
            g_bt_state.acl_outstanding = 0u;
            bt_mark_ready();
            kshell_printf("bt: reset complete\n");
            rc = KSHELL_OK;
        }
    } else if (argc == 3 && strcmp(argv[1], "name") == 0) {
        k_size_t len = strlen(argv[2]);
        k_u8_t params[BT_HCI_LOCAL_NAME_LEN];

        if (len >= BT_HCI_LOCAL_NAME_LEN) {
            kshell_printf("bt: name must be at most %u bytes\n",
                          (unsigned)(BT_HCI_LOCAL_NAME_LEN - 1u));
            rc = KSHELL_USAGE;
            goto out;
        }

        memset(params, 0, sizeof(params));
        memcpy(params, argv[2], len);
        responded = bt_command(&g_bt_device, BT_HCI_OP_WRITE_LOCAL_NAME, params,
                               BT_HCI_LOCAL_NAME_LEN, &status);
        if (responded && status == 0u) {
            memcpy(g_bt_state.name, params, len);
            g_bt_state.name[len] = '\0';
            g_bt_state.name_known = true;
            kshell_printf("bt: local name set\n");
            rc = KSHELL_OK;
        }
    } else if (argc == 3 && strcmp(argv[1], "scan") == 0) {
        if (strcmp(argv[2], "on") == 0) {
            static const k_u8_t params[] = {
                0x33u,
                0x8Bu,
                0x9Eu, /* General Inquiry Access Code, LSB first */
                BT_HCI_INQUIRY_LENGTH,
                0x00u, /* duration and unlimited responses */
            };

            responded =
                bt_command(&g_bt_device, BT_HCI_OP_INQUIRY, params, sizeof(params), &status);
            if (responded && status == 0u) {
                g_bt_state.inquiry_requested = true;
                kshell_printf("bt: inquiry requested; results are not collected\n");
                rc = KSHELL_OK;
            }
        } else if (strcmp(argv[2], "off") == 0) {
            responded = bt_command(&g_bt_device, BT_HCI_OP_INQUIRY_CANCEL, NULL, 0u, &status);
            if (responded && status == 0u) {
                g_bt_state.inquiry_requested = false;
                kshell_printf("bt: inquiry cancel accepted\n");
                rc = KSHELL_OK;
            }
        } else {
            kshell_printf("bt: usage: bt scan on|off\n");
            rc = KSHELL_USAGE;
            goto out;
        }
    } else {
        kshell_printf("bt: usage: bt [info|reset|name <text>|scan on|off]\n");
        rc = KSHELL_USAGE;
        goto out;
    }

    if (rc != KSHELL_OK) {
        if (responded) {
            kshell_printf("bt: HCI command failed with status 0x%02x\n", (unsigned)status);
        } else {
            kshell_printf("bt: HCI command timed out or could not be sent\n");
        }
    }

out:
    (void)k_mutex_unlock(&g_bt_lock);
    return rc;
}

void port_x86_64_bt_init(void)
{
    struct kmod mod;

    memset(&g_bt_state, 0, sizeof(g_bt_state));
    memset(&g_bt_device, 0, sizeof(g_bt_device));
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

void port_x86_64_bt_register(void)
{
    static const struct kshell_help help = {
        .summary = "Bluetooth HCI status and radio controls",
        .usage = "bt [info|reset|name <text>|scan on|off]",
        .detail = "Without a subcommand, or with info, prints the controller identity,\n"
                  "the state of its USB channels and the ACL accounting. reset sends\n"
                  "HCI_Reset; name sends Write Local Name; scan sends Inquiry or Inquiry\n"
                  "Cancel. Scan results are not collected, and there are no connections,\n"
                  "L2CAP or GATT in this slice.\n",
    };

    if (g_bt_command_registered) {
        return;
    }

    if (kshell_register_help("bt", &help, cmd_bt) == KSHELL_OK) {
        g_bt_command_registered = true;
        kinfo("bt: command registered");
    } else {
        kwarn("bt: command could not be registered");
    }
}

static bool bt_attach_locked(struct port_x86_64_usb_device *device,
                             const struct port_x86_64_usb_config *config)
{
    struct bt_usb_if bt;
    struct bt_hci_version version;
    struct bt_hci_bdaddr bdaddr;
    k_u8_t status = 0;
    char addr_str[18];

    if (g_bt_state.present) {
        return false;
    }

    memset(&g_bt_state, 0, sizeof(g_bt_state));
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

    if (bt.bulk_in.address != 0u && port_x86_64_xhci_endpoint(device, bt.bulk_in.address, true, 0u,
                                                              bt.bulk_in.max_packet) == 0) {
        g_bt_state.bulk_in_open = true;
    } else if (bt.bulk_in.address != 0u) {
        kwarn("bt: %04x:%04x bulk IN endpoint configure failed", (unsigned)device->vendor,
              (unsigned)device->product);
    }
    if (bt.bulk_out.address != 0u && port_x86_64_xhci_endpoint(device, bt.bulk_out.address, true,
                                                               0u, bt.bulk_out.max_packet) == 0) {
        g_bt_state.bulk_out_open = true;
    } else if (bt.bulk_out.address != 0u) {
        kwarn("bt: %04x:%04x bulk OUT endpoint configure failed", (unsigned)device->vendor,
              (unsigned)device->product);
    }

    /* HCI Reset. */
    if (!bt_command(device, BT_HCI_OP_RESET, NULL, 0u, &status) || status != 0u) {
        kwarn("bt: %04x:%04x HCI_Reset failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "HCI_Reset failed");
        return false;
    }

    /* Read Local Version Information. */
    memset(&version, 0, sizeof(version));
    if (!bt_command(device, BT_HCI_OP_READ_LOCAL_VERSION, NULL, 0u, &status) || status != 0u ||
        !port_x86_64_bt_parse_local_version(g_hci_evt, (k_size_t)g_hci_evt[1] + 2u, &version)) {
        kwarn("bt: %04x:%04x Read Local Version failed", (unsigned)device->vendor,
              (unsigned)device->product);
        bt_set_kmod(KMOD_FAIL, "Read Local Version failed");
        return false;
    }

    /* Read BD_ADDR. */
    memset(&bdaddr, 0, sizeof(bdaddr));
    if (!bt_command(device, BT_HCI_OP_READ_BD_ADDR, NULL, 0u, &status) || status != 0u ||
        !port_x86_64_bt_parse_bdaddr(g_hci_evt, (k_size_t)g_hci_evt[1] + 2u, &bdaddr)) {
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
    g_bt_device = *device;
    bt_mark_ready();

    kinfo("bt: %04x:%04x HCI %u rev 0x%04x manufacturer 0x%04x BD_ADDR %s",
          (unsigned)device->vendor, (unsigned)device->product, (unsigned)version.hci_version,
          (unsigned)version.hci_revision, (unsigned)version.manufacturer, addr_str);
    kinfo("bt: ACL bulk IN %s, OUT %s; no ACL data was sent",
          g_bt_state.bulk_in_open ? "open" : "closed",
          g_bt_state.bulk_out_open ? "open" : "closed");

    return true;
}

bool port_x86_64_usb_bt_attach(struct port_x86_64_usb_device *device,
                               const struct port_x86_64_usb_config *config)
{
    bool attached;

    if (device == NULL || config == NULL || k_mutex_lock(&g_bt_lock) != 0) {
        return false;
    }

    attached = bt_attach_locked(device, config);
    (void)k_mutex_unlock(&g_bt_lock);
    return attached;
}

void port_x86_64_usb_bt_detach(struct port_x86_64_usb_device *device)
{
    if (device == NULL || k_mutex_lock(&g_bt_lock) != 0) {
        return;
    }

    if (g_bt_state.present && device->slot == g_bt_device.slot) {
        memset(&g_bt_state, 0, sizeof(g_bt_state));
        memset(&g_bt_device, 0, sizeof(g_bt_device));
        bt_set_kmod(KMOD_SKIP, "no Bluetooth controller found");
        kinfo("bt: controller removed");
    }

    (void)k_mutex_unlock(&g_bt_lock);
}
