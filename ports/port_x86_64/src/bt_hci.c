/*
 * Micro OS — pure HCI response parsing for the x86-64 BT handshake.
 *
 * These functions take the bytes of an HCI event as the controller sent
 * them on the interrupt IN endpoint and fill a small record. They have no
 * side effects and no dependency on the xHCI, so the host tests feed them
 * synthetic Command Complete events (Bluetooth Core Spec, Vol 4 Part E).
 */
#include "port_x86_64_bt.h"

#include <stdio.h>
#include <string.h>

static k_u16_t le16(const k_u8_t *p)
{
    return (k_u16_t)p[0] | (k_u16_t)((k_u16_t)p[1] << 8);
}

static bool parse_command_complete(const k_u8_t *event, k_size_t length, k_u16_t opcode,
                                   const k_u8_t **params, k_size_t *param_len)
{
    k_size_t total;

    if (event == NULL || params == NULL || param_len == NULL) {
        return false;
    }

    /* Event header: event code (1) + parameter total length (1). Command
     * Complete parameter: Num_HCI_Command_Packets (1) + Command_Opcode (2)
     * + return parameters. */
    if (length < 6u) {
        return false;
    }

    if (event[0] != BT_HCI_EVT_COMMAND_COMPLETE) {
        return false;
    }

    total = (k_size_t)event[1];

    if (total + 2u > length || total < 3u) {
        return false;
    }

    if (le16(event + 3) != opcode) {
        return false;
    }

    *params = event + 5;
    *param_len = total - 3u;
    return true;
}

bool port_x86_64_bt_parse_reset(const k_u8_t *event, k_size_t length, k_u8_t *status)
{
    const k_u8_t *params = NULL;
    k_size_t param_len = 0;

    if (!parse_command_complete(event, length, BT_HCI_OP_RESET, &params, &param_len) ||
        param_len < 1u) {
        return false;
    }

    if (status != NULL) {
        *status = params[0];
    }

    return params[0] == 0u;
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
        param_len < 9u) {
        return false;
    }

    out->status = params[0];
    out->hci_version = params[1];
    out->hci_revision = le16(params + 2);
    out->lmp_version = params[4];
    out->manufacturer = le16(params + 5);
    out->lmp_subversion = le16(params + 7);
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
        param_len < 1u + BT_HCI_BDADDR_LEN) {
        return false;
    }

    out->status = params[0];
    memcpy(out->addr, params + 1, BT_HCI_BDADDR_LEN);
    return out->status == 0u;
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
