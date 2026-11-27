/*
 * Micro OS — the x86-64 port: a Bluetooth controller over the USB (HCI).
 *
 * The BT of the laptop is a USB device of the class 0xE0/0x01/0x01
 * (Wireless Controller / RF Controller / Bluetooth). The host speaks the
 * HCI over the USB transport (Bluetooth Core Spec, Vol 4 Part B): commands
 * go out as class control transfers on endpoint 0, events come back on an
 * interrupt IN endpoint, and ACL data would use the bulk endpoints. This
 * release does the handshake only - Reset and the two read commands - and
 * it never loads firmware and never changes the state of the radio.
 *
 * The parsing of the HCI answers lives in bt_hci.c as pure functions so the
 * host tests can feed them synthetic bytes without a controller.
 */
#ifndef MICRO_OS_PORT_X86_64_BT_H
#define MICRO_OS_PORT_X86_64_BT_H

#include <stddef.h>

#include "kernel/kmod.h"
#include "kernel/types.h"

/* HCI command opcodes (Bluetooth Core Spec, Vol 4 Part E / Host Controller
 * Baseband commands and Informational Parameters). */
#define BT_HCI_OP_RESET 0x0C03u
#define BT_HCI_OP_READ_LOCAL_VERSION 0x1001u
#define BT_HCI_OP_READ_BD_ADDR 0x1009u

/* HCI event codes. */
#define BT_HCI_EVT_COMMAND_COMPLETE 0x0Eu

#define BT_HCI_BDADDR_LEN 6u

struct bt_hci_version {
    k_u8_t status;
    k_u8_t hci_version;
    k_u16_t hci_revision;
    k_u8_t lmp_version;
    k_u16_t manufacturer;
    k_u16_t lmp_subversion;
};

struct bt_hci_bdaddr {
    k_u8_t status;
    k_u8_t addr[BT_HCI_BDADDR_LEN];
};

/* Parse a Command Complete event for HCI_Reset. Returns true on a well-formed
 * event whose opcode matches and whose status is 0. */
bool port_x86_64_bt_parse_reset(const k_u8_t *event, k_size_t length, k_u8_t *status);

/* Parse a Command Complete event for Read Local Version Information. */
bool port_x86_64_bt_parse_local_version(const k_u8_t *event, k_size_t length,
                                        struct bt_hci_version *out);

/* Parse a Command Complete event for Read BD_ADDR. */
bool port_x86_64_bt_parse_bdaddr(const k_u8_t *event, k_size_t length, struct bt_hci_bdaddr *out);

/* Format a BD_ADDR as "AA:BB:CC:DD:EE:FF" (big-endian human order). The
 * buffer must hold at least 18 bytes. */
void port_x86_64_bt_format_bdaddr(const k_u8_t addr[BT_HCI_BDADDR_LEN], char *out,
                                  k_size_t out_len);

/* The state the shell and the doctor read. */
struct port_x86_64_bt_state {
    bool present;
    k_u16_t vendor;
    k_u16_t product;
    k_u8_t hci_version;
    k_u16_t hci_revision;
    k_u16_t manufacturer;
    k_u8_t bdaddr[BT_HCI_BDADDR_LEN];
    char name[48];
};

void port_x86_64_bt_init(void);
const struct port_x86_64_bt_state *port_x86_64_bt_state(void);
const char *port_x86_64_bt_kmod_state_name(void);

#endif /* MICRO_OS_PORT_X86_64_BT_H */
