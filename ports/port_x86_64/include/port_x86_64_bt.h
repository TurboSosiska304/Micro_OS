/*
 * Micro OS — the x86-64 port: a Bluetooth controller over the USB (HCI).
 *
 * The BT of the laptop is a USB device of the class 0xE0/0x01/0x01
 * (Wireless Controller / RF Controller / Bluetooth). The host speaks the
 * HCI over the USB transport (Bluetooth Core Spec, Vol 4 Part B): commands
 * go out as class control transfers on endpoint 0, events come back on an
 * interrupt IN endpoint, and ACL data uses the bulk endpoints. The handshake
 * and the small control surface do not load firmware and do not implement
 * connections, L2CAP or GATT.
 *
 * The parsing of the HCI answers lives in bt_hci.c as pure functions so the
 * host tests can feed them synthetic bytes without a controller.
 */
#ifndef MICRO_OS_PORT_X86_64_BT_H
#define MICRO_OS_PORT_X86_64_BT_H

#include <stddef.h>

#include "kernel/kmod.h"
#include "kernel/types.h"

/* HCI command opcodes (Bluetooth Core Spec, Vol 4 Part E). */
#define BT_HCI_OP_RESET 0x0C03u
#define BT_HCI_OP_WRITE_LOCAL_NAME 0x0C13u
#define BT_HCI_OP_INQUIRY 0x0401u
#define BT_HCI_OP_INQUIRY_CANCEL 0x0402u
#define BT_HCI_OP_READ_LOCAL_VERSION 0x1001u
#define BT_HCI_OP_READ_BD_ADDR 0x1009u

/* HCI event codes handled by this slice. */
#define BT_HCI_EVT_DISCONNECTION_COMPLETE 0x05u
#define BT_HCI_EVT_COMMAND_COMPLETE 0x0Eu
#define BT_HCI_EVT_COMMAND_STATUS 0x0Fu
#define BT_HCI_EVT_HARDWARE_ERROR 0x10u
#define BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS 0x13u

#define BT_HCI_BDADDR_LEN 6u
#define BT_HCI_LOCAL_NAME_LEN 248u
#define BT_HCI_EVENT_PARAM_MAX 255u
#define BT_HCI_COMPLETED_MAX ((BT_HCI_EVENT_PARAM_MAX - 1u) / 4u)

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

/* The event table is the list of packets this driver understands. Fixed
 * events have exactly min_params bytes; variable events may be longer. */
struct bt_hci_event_def {
    k_u8_t code;
    const char *name;
    k_size_t min_params;
    bool variable;
};

struct bt_hci_completed_packet {
    k_u16_t handle;
    k_u16_t completed;
};

/* A validated HCI event. Fields not used by its code remain zero. */
struct bt_hci_event {
    k_u8_t code;
    k_u8_t status;
    k_u8_t num_command_packets;
    k_u16_t opcode;
    k_u16_t handle;
    k_u8_t reason;
    k_u8_t hardware_code;
    k_u8_t completed_count;
    struct bt_hci_completed_packet completed[BT_HCI_COMPLETED_MAX];
};

const struct bt_hci_event_def *port_x86_64_bt_event_info(k_u8_t code);
k_size_t port_x86_64_bt_event_count(void);
const struct bt_hci_event_def *port_x86_64_bt_event_at(k_size_t index);

/* Parse and validate one event packet (event code, parameter length and the
 * parameters). Unknown events and malformed packets return false. */
bool port_x86_64_bt_parse_event(const k_u8_t *event, k_size_t length, struct bt_hci_event *out);

/* Parse a Command Complete event for HCI_Reset. Returns true on a well-formed
 * event whose opcode matches and whose status is 0. */
bool port_x86_64_bt_parse_reset(const k_u8_t *event, k_size_t length, k_u8_t *status);

/* Parse a Command Complete event for Read Local Version Information. */
bool port_x86_64_bt_parse_local_version(const k_u8_t *event, k_size_t length,
                                        struct bt_hci_version *out);

/* Parse a Command Complete event for Read BD_ADDR. */
bool port_x86_64_bt_parse_bdaddr(const k_u8_t *event, k_size_t length, struct bt_hci_bdaddr *out);

/* Account completed ACL packets without letting an unsolicited completion
 * underflow the outstanding counter. Returns the new outstanding value and,
 * when requested, the number accounted. */
k_u16_t port_x86_64_bt_acl_account(k_u16_t outstanding, k_u16_t completed, k_u32_t *accounted);

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
    bool bulk_in_open;
    bool bulk_out_open;
    bool name_known;
    char name[BT_HCI_LOCAL_NAME_LEN + 1u];
    bool inquiry_requested;
    k_u16_t acl_outstanding;
    k_u32_t acl_completed;
};

void port_x86_64_bt_init(void);
const struct port_x86_64_bt_state *port_x86_64_bt_state(void);
const char *port_x86_64_bt_kmod_state_name(void);

#endif /* MICRO_OS_PORT_X86_64_BT_H */
