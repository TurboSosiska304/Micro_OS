/*
 * Micro OS — pure HCI event parsing for the x86-64 BT transport.
 *
 * These functions take the bytes of an HCI event as the controller sent
 * them on the interrupt IN endpoint and fill a small record. They have no
 * side effects and no dependency on the xHCI, so the host tests feed them
 * synthetic events (Bluetooth Core Spec, Vol 4 Part E).
 */
#include "port_x86_64_bt.h"

#include <stdio.h>
#include <string.h>

static const struct bt_hci_event_def g_bt_events[] = {
    {BT_HCI_EVT_DISCONNECTION_COMPLETE, "Disconnection Complete", 4u, false},
    {BT_HCI_EVT_COMMAND_COMPLETE, "Command Complete", 3u, true},
    {BT_HCI_EVT_COMMAND_STATUS, "Command Status", 4u, false},
    {BT_HCI_EVT_HARDWARE_ERROR, "Hardware Error", 1u, false},
    {BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS, "Number Of Completed Packets", 1u, true},
};

static k_u16_t le16(const k_u8_t *p)
{
    return (k_u16_t)p[0] | (k_u16_t)((k_u16_t)p[1] << 8);
}

const struct bt_hci_event_def *port_x86_64_bt_event_info(k_u8_t code)
{
    k_size_t i;

    for (i = 0; i < sizeof(g_bt_events) / sizeof(g_bt_events[0]); i++) {
        if (g_bt_events[i].code == code) {
            return &g_bt_events[i];
        }
    }

    return NULL;
}

k_size_t port_x86_64_bt_event_count(void)
{
    return sizeof(g_bt_events) / sizeof(g_bt_events[0]);
}

const struct bt_hci_event_def *port_x86_64_bt_event_at(k_size_t index)
{
    if (index >= port_x86_64_bt_event_count()) {
        return NULL;
    }

    return &g_bt_events[index];
}

static bool event_params(const k_u8_t *event, k_size_t length, const struct bt_hci_event_def *def,
                         const k_u8_t **params, k_size_t *param_len)
{
    k_size_t total;

    if (event == NULL || def == NULL || params == NULL || param_len == NULL || length < 2u) {
        return false;
    }

    if (event[0] != def->code) {
        return false;
    }

    total = (k_size_t)event[1];
    if (total + 2u > length || total < def->min_params) {
        return false;
    }
    if (!def->variable && total != def->min_params) {
        return false;
    }

    *params = event + 2;
    *param_len = total;
    return true;
}

static bool parse_command_complete(const k_u8_t *event, k_size_t length, k_u16_t opcode,
                                   const k_u8_t **params, k_size_t *param_len)
{
    const struct bt_hci_event_def *def = port_x86_64_bt_event_info(BT_HCI_EVT_COMMAND_COMPLETE);

    if (def == NULL || !event_params(event, length, def, params, param_len) || *param_len < 3u) {
        return false;
    }

    return le16(*params + 1u) == opcode;
}

bool port_x86_64_bt_parse_event(const k_u8_t *event, k_size_t length, struct bt_hci_event *out)
{
    const struct bt_hci_event_def *def;
    const k_u8_t *params = NULL;
    k_size_t param_len = 0;
    k_size_t i;

    if (event == NULL || out == NULL || length < 2u) {
        return false;
    }

    def = port_x86_64_bt_event_info(event[0]);
    if (def == NULL || !event_params(event, length, def, &params, &param_len)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->code = event[0];

    switch (out->code) {
    case BT_HCI_EVT_COMMAND_COMPLETE:
        out->num_command_packets = params[0];
        out->opcode = le16(params + 1u);
        break;
    case BT_HCI_EVT_COMMAND_STATUS:
        out->status = params[0];
        out->num_command_packets = params[1];
        out->opcode = le16(params + 2u);
        break;
    case BT_HCI_EVT_HARDWARE_ERROR:
        out->hardware_code = params[0];
        break;
    case BT_HCI_EVT_DISCONNECTION_COMPLETE:
        out->status = params[0];
        out->handle = le16(params + 1u);
        out->reason = params[3];
        break;
    case BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS:
        out->completed_count = params[0];
        if (out->completed_count == 0u || out->completed_count > BT_HCI_COMPLETED_MAX ||
            param_len != 1u + (k_size_t)out->completed_count * 4u) {
            return false;
        }
        for (i = 0; i < out->completed_count; i++) {
            const k_u8_t *item = params + 1u + i * 4u;
            out->completed[i].handle = le16(item);
            out->completed[i].completed = le16(item + 2u);
        }
        break;
    default:
        return false;
    }

    return true;
}

bool port_x86_64_bt_parse_reset(const k_u8_t *event, k_size_t length, k_u8_t *status)
{
    const k_u8_t *params = NULL;
    k_size_t param_len = 0;

    if (!parse_command_complete(event, length, BT_HCI_OP_RESET, &params, &param_len) ||
        param_len < 4u) {
        return false;
    }

    if (status != NULL) {
        *status = params[3];
    }

    return params[3] == 0u;
}

bool port_x86_64_bt_parse_local_version(const k_u8_t *event, k_size_t length,
                                        struct bt_hci_version *out)
{
    const k_u8_t *params = NULL;
    k_size_t param_len = 0;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!parse_command_complete(event, length, BT_HCI_OP_READ_LOCAL_VERSION, &params, &param_len) ||
        param_len < 12u) {
        return false;
    }

    out->status = params[3];
    out->hci_version = params[4];
    out->hci_revision = le16(params + 5);
    out->lmp_version = params[7];
    out->manufacturer = le16(params + 8);
    out->lmp_subversion = le16(params + 10);
    return out->status == 0u;
}

bool port_x86_64_bt_parse_bdaddr(const k_u8_t *event, k_size_t length, struct bt_hci_bdaddr *out)
{
    const k_u8_t *params = NULL;
    k_size_t param_len = 0;

    if (out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (!parse_command_complete(event, length, BT_HCI_OP_READ_BD_ADDR, &params, &param_len) ||
        param_len < 10u) {
        return false;
    }

    out->status = params[3];
    memcpy(out->addr, params + 4, BT_HCI_BDADDR_LEN);
    return out->status == 0u;
}

k_u16_t port_x86_64_bt_acl_account(k_u16_t outstanding, k_u16_t completed, k_u32_t *accounted)
{
    k_u16_t accepted = completed > outstanding ? outstanding : completed;

    if (accounted != NULL) {
        *accounted = accepted;
    }

    return (k_u16_t)(outstanding - accepted);
}

void port_x86_64_bt_format_bdaddr(const k_u8_t addr[BT_HCI_BDADDR_LEN], char *out, k_size_t out_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }

    if (addr == NULL) {
        out[0] = '\0';
        return;
    }

    /* BD_ADDR is transmitted LSB first; human-readable form prints the most
     * significant octet first. */
    (void)snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)addr[5],
                   (unsigned)addr[4], (unsigned)addr[3], (unsigned)addr[2], (unsigned)addr[1],
                   (unsigned)addr[0]);
}
