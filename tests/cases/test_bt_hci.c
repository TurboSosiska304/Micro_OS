/*
 * Micro OS — tests of the pure HCI event parser of the BT transport.
 *
 * The bytes are synthetic HCI events as the USB transport delivers them on
 * the interrupt IN endpoint (Bluetooth Core Spec, Vol 4 Part E). No
 * controller and no xHCI is involved.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "port_x86_64_bt.h"

static void test_event_table(void)
{
    static const k_u8_t codes[] = {
        BT_HCI_EVT_DISCONNECTION_COMPLETE,
        BT_HCI_EVT_COMMAND_COMPLETE,
        BT_HCI_EVT_COMMAND_STATUS,
        BT_HCI_EVT_HARDWARE_ERROR,
        BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS,
    };
    k_size_t i;

    KT_CHECK_INT_EQ(port_x86_64_bt_event_count(), sizeof(codes) / sizeof(codes[0]));
    KT_CHECK(port_x86_64_bt_event_info(0xFFu) == NULL);
    KT_CHECK(port_x86_64_bt_event_at(port_x86_64_bt_event_count()) == NULL);

    for (i = 0; i < port_x86_64_bt_event_count(); i++) {
        const struct bt_hci_event_def *def = port_x86_64_bt_event_at(i);

        KT_CHECK(def != NULL);
        if (def == NULL) {
            continue;
        }
        KT_CHECK_INT_EQ(def->code, codes[i]);
        KT_CHECK(def->name != NULL);
        KT_CHECK(def->name[0] != '\0');
        KT_CHECK(port_x86_64_bt_event_info(def->code) == def);
        if (i != 0u) {
            KT_CHECK(port_x86_64_bt_event_at(i - 1u)->code < def->code);
        }
    }

    KT_CHECK_STR_EQ(port_x86_64_bt_event_info(BT_HCI_EVT_COMMAND_STATUS)->name, "Command Status");
    KT_CHECK_INT_EQ(port_x86_64_bt_event_info(BT_HCI_EVT_HARDWARE_ERROR)->min_params, 1);
    KT_CHECK(port_x86_64_bt_event_info(BT_HCI_EVT_COMMAND_COMPLETE)->variable);
}

static void test_reset_ok(void)
{
    /* Command Complete: Num_HCI_Command_Packets=1, opcode=0x0C03, status=0. */
    static const k_u8_t evt[] = {0x0Eu, 0x04u, 0x01u, 0x03u, 0x0Cu, 0x00u};
    k_u8_t status = 0xFFu;

    KT_CHECK(port_x86_64_bt_parse_reset(evt, sizeof(evt), &status));
    KT_CHECK_INT_EQ(status, 0);
}

static void test_reset_fail_status(void)
{
    static const k_u8_t evt[] = {0x0Eu, 0x04u, 0x01u, 0x03u, 0x0Cu, 0x01u};
    k_u8_t status = 0u;

    KT_CHECK(!port_x86_64_bt_parse_reset(evt, sizeof(evt), &status));
    KT_CHECK_INT_EQ(status, 1);
}

static void test_local_version_ok(void)
{
    /* Read Local Version Information (0x1001) return: status, hci_ver,
     * hci_rev(lo,hi), lmp_ver, manufacturer(lo,hi), lmp_subver(lo,hi). */
    static const k_u8_t evt[] = {
        0x0Eu, 0x0Cu, 0x01u, /* Command Complete, plen=12, num_pkts=1 */
        0x01u, 0x10u,        /* opcode 0x1001 */
        0x00u,               /* status */
        0x09u,               /* HCI version (5.0 = 9) */
        0x00u, 0x08u,        /* HCI revision 0x0800 */
        0x09u,               /* LMP/PAL version */
        0x2Du, 0x00u,        /* manufacturer 0x002D (Intel) */
        0x00u, 0x01u,        /* LMP subversion 0x0100 */
    };
    struct bt_hci_version v;

    KT_CHECK(port_x86_64_bt_parse_local_version(evt, sizeof(evt), &v));
    KT_CHECK_INT_EQ(v.status, 0);
    KT_CHECK_INT_EQ(v.hci_version, 9);
    KT_CHECK_INT_EQ(v.hci_revision, 0x0800);
    KT_CHECK_INT_EQ(v.lmp_version, 9);
    KT_CHECK_INT_EQ(v.manufacturer, 0x002D);
    KT_CHECK_INT_EQ(v.lmp_subversion, 0x0100);
}

static void test_bdaddr_ok(void)
{
    /* Read BD_ADDR (0x1009) return: status + 6-byte BD_ADDR (LSB first). */
    static const k_u8_t evt[] = {
        0x0Eu, 0x0Au, 0x01u,                      /* Command Complete, plen=10 */
        0x09u, 0x10u,                             /* opcode 0x1009 */
        0x00u,                                    /* status */
        0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, /* BD_ADDR LSB first */
    };
    struct bt_hci_bdaddr a;
    char s[18];

    KT_CHECK(port_x86_64_bt_parse_bdaddr(evt, sizeof(evt), &a));
    KT_CHECK_INT_EQ(a.status, 0);
    KT_CHECK_INT_EQ(a.addr[0], 0x11);
    KT_CHECK_INT_EQ(a.addr[5], 0x66);

    port_x86_64_bt_format_bdaddr(a.addr, s, sizeof(s));
    KT_CHECK_STR_EQ(s, "66:55:44:33:22:11");
}

static void test_command_status(void)
{
    static const k_u8_t accepted[] = {0x0Fu, 0x04u, 0x00u, 0x01u, 0x01u, 0x04u};
    static const k_u8_t rejected[] = {0x0Fu, 0x04u, 0x0Cu, 0x01u, 0x02u, 0x04u};
    struct bt_hci_event evt;

    KT_CHECK(port_x86_64_bt_parse_event(accepted, sizeof(accepted), &evt));
    KT_CHECK_INT_EQ(evt.code, BT_HCI_EVT_COMMAND_STATUS);
    KT_CHECK_INT_EQ(evt.status, 0);
    KT_CHECK_INT_EQ(evt.num_command_packets, 1);
    KT_CHECK_INT_EQ(evt.opcode, BT_HCI_OP_INQUIRY);

    KT_CHECK(port_x86_64_bt_parse_event(rejected, sizeof(rejected), &evt));
    KT_CHECK_INT_EQ(evt.status, 0x0C);
    KT_CHECK_INT_EQ(evt.opcode, BT_HCI_OP_INQUIRY_CANCEL);
}

static void test_completed_packets(void)
{
    static const k_u8_t evt_bytes[] = {
        0x13u, 0x09u, 0x02u,        /* two handles */
        0x01u, 0x00u, 0x03u, 0x00u, /* handle 1, three packets */
        0x02u, 0x00u, 0x01u, 0x00u, /* handle 2, one packet */
    };
    struct bt_hci_event evt;
    k_u32_t accounted = 0;

    KT_CHECK(port_x86_64_bt_parse_event(evt_bytes, sizeof(evt_bytes), &evt));
    KT_CHECK_INT_EQ(evt.code, BT_HCI_EVT_NUMBER_OF_COMPLETED_PACKETS);
    KT_CHECK_INT_EQ(evt.completed_count, 2);
    KT_CHECK_INT_EQ(evt.completed[0].handle, 1);
    KT_CHECK_INT_EQ(evt.completed[0].completed, 3);
    KT_CHECK_INT_EQ(evt.completed[1].handle, 2);
    KT_CHECK_INT_EQ(evt.completed[1].completed, 1);

    KT_CHECK_INT_EQ(port_x86_64_bt_acl_account(5u, 3u, &accounted), 2);
    KT_CHECK_INT_EQ(accounted, 3);
    KT_CHECK_INT_EQ(port_x86_64_bt_acl_account(2u, 7u, &accounted), 0);
    KT_CHECK_INT_EQ(accounted, 2);
    KT_CHECK_INT_EQ(port_x86_64_bt_acl_account(0u, 1u, NULL), 0);
}

static void test_error_and_disconnection(void)
{
    static const k_u8_t hardware[] = {0x10u, 0x01u, 0x42u};
    static const k_u8_t disconnect[] = {0x05u, 0x04u, 0x00u, 0x34u, 0x12u, 0x13u};
    struct bt_hci_event evt;

    KT_CHECK(port_x86_64_bt_parse_event(hardware, sizeof(hardware), &evt));
    KT_CHECK_INT_EQ(evt.code, BT_HCI_EVT_HARDWARE_ERROR);
    KT_CHECK_INT_EQ(evt.hardware_code, 0x42);

    KT_CHECK(port_x86_64_bt_parse_event(disconnect, sizeof(disconnect), &evt));
    KT_CHECK_INT_EQ(evt.code, BT_HCI_EVT_DISCONNECTION_COMPLETE);
    KT_CHECK_INT_EQ(evt.status, 0);
    KT_CHECK_INT_EQ(evt.handle, 0x1234);
    KT_CHECK_INT_EQ(evt.reason, 0x13);
}

static void check_truncated(const k_u8_t *evt, k_size_t length)
{
    struct bt_hci_event parsed;
    k_size_t n;

    for (n = 0; n < length; n++) {
        KT_CHECK(!port_x86_64_bt_parse_event(evt, n, &parsed));
    }
}

static void test_rejects_bad_events(void)
{
    static const k_u8_t complete[] = {0x0Eu, 0x04u, 0x01u, 0x03u, 0x0Cu, 0x00u};
    static const k_u8_t status[] = {0x0Fu, 0x04u, 0x00u, 0x01u, 0x01u, 0x04u};
    static const k_u8_t completed[] = {0x13u, 0x09u, 0x02u, 0x01u, 0x00u, 0x03u,
                                       0x00u, 0x02u, 0x00u, 0x01u, 0x00u};
    static const k_u8_t hardware[] = {0x10u, 0x01u, 0x42u};
    static const k_u8_t disconnect[] = {0x05u, 0x04u, 0x00u, 0x34u, 0x12u, 0x13u};
    static const k_u8_t bad_code[] = {0xFFu, 0x04u, 0x01u, 0x03u, 0x0Cu, 0x00u};
    static const k_u8_t bad_opcode[] = {0x0Eu, 0x04u, 0x01u, 0x04u, 0x0Cu, 0x00u};
    static const k_u8_t short_evt[] = {0x0Eu, 0x04u, 0x01u, 0x03u};
    static const k_u8_t status_short[] = {0x0Fu, 0x03u, 0x00u, 0x01u, 0x04u};
    static const k_u8_t status_long[] = {0x0Fu, 0x05u, 0x00u, 0x01u, 0x01u, 0x04u, 0x00u};
    static const k_u8_t completed_zero[] = {0x13u, 0x01u, 0x00u};
    static const k_u8_t completed_mismatch[] = {0x13u, 0x05u, 0x02u, 0x01u, 0x00u, 0x03u, 0x00u};
    static const k_u8_t hardware_short[] = {0x10u, 0x00u};
    static const k_u8_t hardware_long[] = {0x10u, 0x02u, 0x42u, 0x00u};
    static const k_u8_t disconnect_short[] = {0x05u, 0x03u, 0x00u, 0x34u, 0x12u};
    k_u8_t oversized[] = {0x0Eu, 0xFFu, 0x01u, 0x03u, 0x0Cu, 0x00u};
    struct bt_hci_version v;
    struct bt_hci_bdaddr a;
    struct bt_hci_event evt;
    k_u8_t status_value = 0;

    KT_CHECK(!port_x86_64_bt_parse_reset(bad_code, sizeof(bad_code), &status_value));
    KT_CHECK(!port_x86_64_bt_parse_reset(bad_opcode, sizeof(bad_opcode), &status_value));
    KT_CHECK(!port_x86_64_bt_parse_reset(short_evt, sizeof(short_evt), &status_value));
    KT_CHECK(!port_x86_64_bt_parse_reset(NULL, 0u, &status_value));
    KT_CHECK(!port_x86_64_bt_parse_local_version(short_evt, sizeof(short_evt), &v));
    KT_CHECK(!port_x86_64_bt_parse_bdaddr(short_evt, sizeof(short_evt), &a));

    KT_CHECK(!port_x86_64_bt_parse_event(NULL, 0u, &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(complete, sizeof(complete), NULL));
    KT_CHECK(!port_x86_64_bt_parse_event(bad_code, sizeof(bad_code), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(oversized, sizeof(oversized), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(status_short, sizeof(status_short), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(status_long, sizeof(status_long), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(completed_zero, sizeof(completed_zero), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(completed_mismatch, sizeof(completed_mismatch), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(hardware_short, sizeof(hardware_short), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(hardware_long, sizeof(hardware_long), &evt));
    KT_CHECK(!port_x86_64_bt_parse_event(disconnect_short, sizeof(disconnect_short), &evt));

    check_truncated(complete, sizeof(complete));
    check_truncated(status, sizeof(status));
    check_truncated(completed, sizeof(completed));
    check_truncated(hardware, sizeof(hardware));
    check_truncated(disconnect, sizeof(disconnect));
}

void test_bt_hci(void)
{
    test_event_table();
    test_reset_ok();
    test_reset_fail_status();
    test_local_version_ok();
    test_bdaddr_ok();
    test_command_status();
    test_completed_packets();
    test_error_and_disconnection();
    test_rejects_bad_events();
}
