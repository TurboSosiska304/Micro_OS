/*
 * Micro OS — tests of the pure HCI response parser of the BT handshake.
 *
 * The bytes are synthetic Command Complete events as the USB HCI transport
 * delivers them on the interrupt IN endpoint (Bluetooth Core Spec, Vol 4
 * Part E, 7.7.14 Command Complete). No controller and no xHCI is involved.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "port_x86_64_bt.h"

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

static void test_rejects_bad_events(void)
{
    static const k_u8_t bad_code[] = {0x0Fu, 0x04u, 0x01u, 0x03u, 0x0Cu, 0x00u};
    static const k_u8_t bad_opcode[] = {0x0Eu, 0x04u, 0x01u, 0x04u, 0x0Cu, 0x00u};
    static const k_u8_t short_evt[] = {0x0Eu, 0x04u, 0x01u, 0x03u};
    struct bt_hci_version v;
    struct bt_hci_bdaddr a;
    k_u8_t status = 0;

    KT_CHECK(!port_x86_64_bt_parse_reset(bad_code, sizeof(bad_code), &status));
    KT_CHECK(!port_x86_64_bt_parse_reset(bad_opcode, sizeof(bad_opcode), &status));
    KT_CHECK(!port_x86_64_bt_parse_reset(short_evt, sizeof(short_evt), &status));
    KT_CHECK(!port_x86_64_bt_parse_reset(NULL, 0u, &status));

    KT_CHECK(!port_x86_64_bt_parse_local_version(short_evt, sizeof(short_evt), &v));
    KT_CHECK(!port_x86_64_bt_parse_bdaddr(short_evt, sizeof(short_evt), &a));
}

void test_bt_hci(void)
{
    test_reset_ok();
    test_reset_fail_status();
    test_local_version_ok();
    test_bdaddr_ok();
    test_rejects_bad_events();
}
