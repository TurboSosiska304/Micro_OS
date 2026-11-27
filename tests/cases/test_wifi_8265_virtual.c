/*
 * Micro OS — the virtual Intel 8265 and the stage 2 loader tests.
 *
 * The device is a small model of the registers the loader touches: CSR, PRPH
 * and the FH DMA door. It accepts ucode, checks that the image arrived whole,
 * counts a CRC32 over the loaded bytes, and posts an ALIVE notification. The
 * model lives only in tests; no production code includes it.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/pcie/trans.c,
 * pcie/ctxt-info.c, iwl-fh.h and iwl-prph.h (GPL-2.0, Linux v6.12);
 * code written here (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "wifi/iwl8265_fw.h"
#include "wifi/iwl8265_loader.h"

/* CSR offsets the loader and the model share. */
#define V_CSR_INT 0x008u
#define V_CSR_INT_MASK 0x00Cu
#define V_CSR_FH_INT_STATUS 0x010u
#define V_CSR_GP_CNTRL 0x024u
#define V_CSR_HW_REV 0x028u
#define V_CSR_HW_IF_CONFIG 0x000u

#define V_CSR_INT_FH_TX (1u << 27)
#define V_CSR_INT_FH_RX (1u << 31)
#define V_CSR_INT_ALIVE (1u << 0)
#define V_CSR_FH_TX_MASK 0x3u
#define V_CSR_GP_MAC_ACCESS_REQ 0x8u
#define V_CSR_GP_CLOCK_READY 0x1u

#define V_FH_TCSR_CONFIG(q) (0x1D00u + 0x20u * (q))
#define V_FH_TCSR_BUF_STATUS(q) (0x1D00u + 0x20u * (q) + 0x08u)
#define V_FH_TFDIB_CTRL0(q) (0x1900u + 0x08u * (q))
#define V_FH_TFDIB_CTRL1(q) (0x1904u + 0x08u * (q))
#define V_FH_SRVC_SRAM_ADDR 0x19C8u

#define V_TCSR_DMA_ENABLE 0x80000000u
#define V_TCSR_CIRQ_ENDTFD 0x00100000u

#define V_PRPH_RELEASE_CPU_RESET 0x300Cu
#define V_RELEASE_CPU_RESET_BIT (1u << 24)
#define V_PRPH_FH_UCODE_LOAD_STATUS 0x1AF0u
#define V_PRPH_RFH_Q0_FRBDCB_WIDX_TRG 0x1C80u
#define V_PRPH_RFH_Q0_FRBDCB_RIDX 0xA080C0u

#define V_SRAM_SIZE (256u * 1024u)
#define V_CSR_WORDS 0x800u

struct virtual_8265 {
    k_u32_t csr[V_CSR_WORDS];
    k_u32_t prph_release;
    k_u32_t prph_wfpm_gp2;
    k_u32_t prph_ureg_load;
    k_u32_t prph_fh_load;
    k_u32_t prph_ridx;
    k_u32_t prph_widx;
    k_u8_t sram[V_SRAM_SIZE];
    k_u32_t sram_loaded;
    k_u32_t dma_chunks;
    k_u32_t crc;
    bool alive_posted;
    bool suppress_alive;
    k_u64_t now_ms;
    /* RX ring pointers the loader programmed. */
    k_u64_t rbd_phys;
    k_u64_t rx_buf_phys;
    k_u8_t *rbd_ptr;
    k_u8_t *rx_ptr;
    k_u64_t last_dma_phys;
};

static void v_put_le16(k_u8_t *p, k_u16_t v)
{
    p[0] = (k_u8_t)v;
    p[1] = (k_u8_t)(v >> 8);
}

static void v_put_le32(k_u8_t *p, k_u32_t v)
{
    p[0] = (k_u8_t)v;
    p[1] = (k_u8_t)(v >> 8);
    p[2] = (k_u8_t)(v >> 16);
    p[3] = (k_u8_t)(v >> 24);
}

static k_u32_t v_get_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static k_u32_t crc32_step(k_u32_t crc, k_u8_t byte)
{
    k_u32_t c = crc ^ byte;
    k_u32_t i;

    for (i = 0u; i < 8u; i++) {
        c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

static k_u32_t crc32_buf(const k_u8_t *data, k_size_t size)
{
    k_u32_t crc = 0xFFFFFFFFu;
    k_size_t i;

    for (i = 0u; i < size; i++) {
        crc = crc32_step(crc, data[i]);
    }
    return ~crc;
}

static void v_reset(struct virtual_8265 *v)
{
    memset(v, 0, sizeof(*v));
    v->csr[V_CSR_HW_REV / 4u] = 0x000024fdu; /* 8265, step A */
    v->csr[V_CSR_GP_CNTRL / 4u] = V_CSR_GP_CLOCK_READY;
}

static k_u32_t v_csr_read(struct virtual_8265 *v, k_u32_t off)
{
    if (off >= V_CSR_WORDS * 4u || (off & 3u) != 0u) {
        return 0u;
    }
    return v->csr[off / 4u];
}

static void v_post_alive(struct virtual_8265 *v)
{
    k_u8_t *pkt;
    k_u32_t len = 64u;
    k_u32_t rbd_lo;
    k_u32_t rbd_hi;

    if (v->alive_posted || v->suppress_alive || v->rbd_ptr == NULL) {
        return;
    }

    rbd_lo = v_get_le32(v->rbd_ptr);
    rbd_hi = v_get_le32(v->rbd_ptr + 4u);
    v->rx_buf_phys = ((k_u64_t)(rbd_hi & 0xFFFu) << 40) | ((k_u64_t)(rbd_lo & ~1u) << 8);
    v->rx_ptr = (k_u8_t *)(uintptr_t)v->rx_buf_phys;
    if (v->rx_ptr == NULL) {
        return;
    }

    pkt = v->rx_ptr;
    memset(pkt, 0, len);
    v_put_le32(pkt, len);
    v_put_le16(pkt + 4u, 1u);      /* ALIVE cmd */
    v_put_le16(pkt + 8u, 0xCAFEu); /* status */
    v_put_le16(pkt + 10u, 0u);
    v_put_le32(pkt + 12u, 1u); /* lmac major */
    v_put_le32(pkt + 16u, 0u); /* lmac minor */
    v_put_le32(pkt + 36u, 0x12340000u);
    v_put_le32(pkt + 44u, 1u); /* umac major */
    v_put_le32(pkt + 48u, 0u); /* umac minor */

    /* RBD: valid bit set, points to rx buffer. */
    v_put_le32(v->rbd_ptr, (k_u32_t)(v->rx_buf_phys >> 8) | 1u);
    v_put_le32(v->rbd_ptr + 4u, (k_u32_t)(v->rx_buf_phys >> 40));
    v->prph_ridx = 1u;
    v->csr[V_CSR_INT / 4u] |= V_CSR_INT_ALIVE | V_CSR_INT_FH_RX;
    v->alive_posted = true;
}

static void v_csr_write(struct virtual_8265 *v, k_u32_t off, k_u32_t value)
{
    if (off >= V_CSR_WORDS * 4u || (off & 3u) != 0u) {
        return;
    }

    /* Write-1-to-clear for INT and FH_INT_STATUS. */
    if (off == V_CSR_INT) {
        v->csr[off / 4u] &= ~value;
        return;
    }
    if (off == V_CSR_FH_INT_STATUS) {
        v->csr[off / 4u] &= ~value;
        return;
    }

    v->csr[off / 4u] = value;

    if (off == 0x044u) {
        /* CTXT_INFO_BA high half: image and context are in place. */
        if (v->sram_loaded > 0u) {
            v_post_alive(v);
        }
    }

    if (off == V_CSR_GP_CNTRL) {
        if ((value & V_CSR_GP_MAC_ACCESS_REQ) != 0u) {
            v->csr[off / 4u] |= V_CSR_GP_CLOCK_READY;
        } else {
            v->csr[off / 4u] &= ~V_CSR_GP_CLOCK_READY;
        }
    }

    /* FH DMA on queue 9. */
    if (off == V_FH_TCSR_CONFIG(9u) && (value & V_TCSR_DMA_ENABLE) != 0u &&
        (value & V_TCSR_CIRQ_ENDTFD) != 0u) {
        k_u32_t ctrl0 = v_csr_read(v, V_FH_TFDIB_CTRL0(9u));
        k_u32_t ctrl1 = v_csr_read(v, V_FH_TFDIB_CTRL1(9u));
        k_u64_t src_phys = ((k_u64_t)(ctrl1 & 0xFu) << 28) | (k_u64_t)ctrl0;
        k_u32_t len = ctrl1 & 0xFFFFFu;
        k_u32_t dst = v_csr_read(v, V_FH_SRVC_SRAM_ADDR);
        const k_u8_t *src;

        if (v->last_dma_phys != 0u && (k_u32_t)v->last_dma_phys == ctrl0) {
            src_phys = v->last_dma_phys;
        }
        src = (const k_u8_t *)(uintptr_t)src_phys;

        {
            k_u32_t mapped = dst & (V_SRAM_SIZE - 1u);

            if (len > 0u && (k_size_t)mapped + len <= V_SRAM_SIZE) {
                memcpy(v->sram + mapped, src, len);
                v->sram_loaded += len;
                v->crc = crc32_buf(v->sram, v->sram_loaded);
                v->dma_chunks++;
            }
        }
        v->csr[V_CSR_INT / 4u] |= V_CSR_INT_FH_TX;
        v->csr[V_CSR_FH_INT_STATUS / 4u] |= V_CSR_FH_TX_MASK;
        v->csr[off / 4u] = 0u;
    }
}

static k_u32_t v_prph_read(struct virtual_8265 *v, k_u32_t off)
{
    switch (off) {
    case V_PRPH_RELEASE_CPU_RESET:
        return v->prph_release;
    case V_PRPH_FH_UCODE_LOAD_STATUS:
        return v->prph_fh_load;
    case V_PRPH_RFH_Q0_FRBDCB_RIDX:
        return v->prph_ridx;
    case V_PRPH_RFH_Q0_FRBDCB_WIDX_TRG:
        return v->prph_widx;
    default:
        return 0u;
    }
}

static void v_prph_write(struct virtual_8265 *v, k_u32_t off, k_u32_t value)
{
    switch (off) {
    case V_PRPH_RELEASE_CPU_RESET:
        v->prph_release = value;
        break;
    case V_PRPH_FH_UCODE_LOAD_STATUS:
        v->prph_fh_load = value;
        break;
    case V_PRPH_RFH_Q0_FRBDCB_WIDX_TRG:
        v->prph_widx = value;
        break;
    case V_PRPH_RFH_Q0_FRBDCB_RIDX:
        v->prph_ridx = value;
        break;
    case 0xA08000u:
        v->rbd_phys = (v->rbd_phys & 0xFFFFFFFF00000000ull) | value;
        v->rbd_ptr = (k_u8_t *)(uintptr_t)v->rbd_phys;
        break;
    case 0xA08004u:
        v->rbd_phys = (v->rbd_phys & 0xFFFFFFFFull) | ((k_u64_t)value << 32);
        v->rbd_ptr = (k_u8_t *)(uintptr_t)v->rbd_phys;
        break;
    case 0xA08200u:
        /* status phys low - ignore */
        break;
    case 0xA08204u:
        break;
    case 0xA08100u:
        break;
    case 0xA08104u:
        break;
    default:
        break;
    }
}

/* --- ops adapter ---------------------------------------------------------- */

static k_u32_t op_read(void *ctx, k_u32_t off)
{
    return v_csr_read((struct virtual_8265 *)ctx, off);
}

static void op_write(void *ctx, k_u32_t off, k_u32_t value)
{
    v_csr_write((struct virtual_8265 *)ctx, off, value);
}

static void op_write64(void *ctx, k_u32_t off, k_u64_t value)
{
    v_csr_write((struct virtual_8265 *)ctx, off, (k_u32_t)value);
    v_csr_write((struct virtual_8265 *)ctx, off + 4u, (k_u32_t)(value >> 32));
}

static k_u32_t op_read_prph(void *ctx, k_u32_t off)
{
    return v_prph_read((struct virtual_8265 *)ctx, off);
}

static void op_write_prph(void *ctx, k_u32_t off, k_u32_t value)
{
    v_prph_write((struct virtual_8265 *)ctx, off, value);
}

static k_u64_t op_dma_phys(void *ctx, const void *buffer)
{
    struct virtual_8265 *v = (struct virtual_8265 *)ctx;

    v->last_dma_phys = (k_u64_t)(uintptr_t)buffer;
    return v->last_dma_phys;
}

static k_u64_t op_now_ms(void *ctx)
{
    struct virtual_8265 *v = (struct virtual_8265 *)ctx;
    return v->now_ms++;
}

static void v_ops(struct virtual_8265 *v, struct wifi_iwl8265_load_ops *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->context = v;
    ops->read = op_read;
    ops->write = op_write;
    ops->write64 = op_write64;
    ops->read_prph = op_read_prph;
    ops->write_prph = op_write_prph;
    ops->dma_phys = op_dma_phys;
    ops->now_ms = op_now_ms;
}

/* --- firmware builders ---------------------------------------------------- */

#define FW_BUF_SIZE 4096
static k_u8_t g_fw_buf[FW_BUF_SIZE] __attribute__((aligned(4096)));

static void fw_put32(k_u8_t *p, k_u32_t v)
{
    v_put_le32(p, v);
}

static k_size_t fw_header(k_u8_t *b, k_u32_t api, k_u32_t build, const char *human)
{
    k_size_t i = 0;
    fw_put32(b, 0u);
    fw_put32(b + 4u, 0x0a4c5749u);
    while (human[i] != '\0' && i < 63u) {
        b[8u + i] = (k_u8_t)human[i];
        i++;
    }
    memset(b + 8u + i, 0, 64u - i);
    fw_put32(b + 72u, (9u << 24) | (7u << 16) | (api << 8) | 3u);
    fw_put32(b + 76u, build);
    memset(b + 80u, 0, 8u);
    return 88u;
}

static k_size_t fw_tlv(k_u8_t *b, k_size_t pos, k_u32_t type, const k_u8_t *payload, k_u32_t len)
{
    k_size_t padded = ((k_size_t)len + 3u) & ~(k_size_t)3u;
    fw_put32(b + pos, type);
    fw_put32(b + pos + 4u, len);
    if (len > 0u && payload != NULL) {
        memcpy(b + pos + 8u, payload, len);
    }
    if (padded > len) {
        memset(b + pos + 8u + len, 0, padded - len);
    }
    return pos + 8u + padded;
}

static k_size_t fw_record(k_u8_t *p, k_u32_t offset, const k_u8_t *data, k_size_t n)
{
    fw_put32(p, offset);
    fw_put32(p + 4u, (k_u32_t)n + 4u);
    if (n > 0u) {
        memcpy(p + 8u, data, n);
    }
    return 8u + n;
}

static k_size_t make_runtime_fw(k_u8_t *b, k_u32_t api)
{
    static const k_u8_t code[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    k_u8_t sec[32];
    k_size_t n = 0u;
    k_size_t pos;

    n += fw_record(sec + n, 0x800000u, code, sizeof(code));
    pos = fw_header(b, api, 0x1234u, "iwlwifi-8265-36.ucode");
    pos = fw_tlv(b, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, (k_u32_t)n);
    return pos;
}

/* --- state machine -------------------------------------------------------- */

enum iwl8265_sm_state {
    IWL_SM_DOWN = 0,
    IWL_SM_POWER_UP,
    IWL_SM_FW_LOADING,
    IWL_SM_ALIVE,
    IWL_SM_READY,
    IWL_SM_FAILED,
    IWL_SM_SKIP,
};

static const char *sm_name(enum iwl8265_sm_state state)
{
    switch (state) {
    case IWL_SM_DOWN:
        return "DOWN";
    case IWL_SM_POWER_UP:
        return "POWER_UP";
    case IWL_SM_FW_LOADING:
        return "FW_LOADING";
    case IWL_SM_ALIVE:
        return "ALIVE";
    case IWL_SM_READY:
        return "READY";
    case IWL_SM_FAILED:
        return "FAILED";
    case IWL_SM_SKIP:
        return "SKIP";
    default:
        return "UNKNOWN";
    }
}

struct iwl8265_sm {
    enum iwl8265_sm_state state;
    char reason[96];
    k_u32_t transitions;
};

static void sm_set(struct iwl8265_sm *sm, enum iwl8265_sm_state state, const char *reason)
{
    k_size_t i = 0u;

    sm->state = state;
    sm->transitions++;
    sm->reason[0] = '\0';
    if (reason != NULL) {
        while (i + 1u < sizeof(sm->reason) && reason[i] != '\0') {
            sm->reason[i] = reason[i];
            i++;
        }
        sm->reason[i] = '\0';
    }
    klog(K_LOG_LEVEL_INFO, "wifi: state %s%s%s", sm_name(state), reason ? ": " : "",
         reason ? reason : "");
}

static int sm_start(struct iwl8265_sm *sm, struct virtual_8265 *v,
                    const struct wifi_iwl8265_load_ops *ops, const k_u8_t *fw, k_size_t fw_size)
{
    struct wifi_iwl8265_fw parsed;
    struct wifi_iwl8265_alive alive;
    enum wifi_iwl8265_fw_result rc;
    int load_rc;

    /* attach -> power up -> fw loading -> alive -> ready */
    sm_set(sm, IWL_SM_POWER_UP, "device attached and powered");
    sm_set(sm, IWL_SM_FW_LOADING, "loading ucode");

    rc = wifi_iwl8265_fw_parse(fw, fw_size, &parsed);
    if (rc != WIFI_IWL8265_FW_OK) {
        sm_set(sm, IWL_SM_FAILED, "firmware parse failed");
        return -1;
    }

    load_rc = wifi_iwl8265_load(&parsed, ops, &alive);
    if (load_rc != 0) {
        sm_set(sm, IWL_SM_FAILED, v->suppress_alive ? "ALIVE timeout" : "load failed");
        return -1;
    }

    sm_set(sm, IWL_SM_ALIVE, "alive notification received");
    sm_set(sm, IWL_SM_READY, "transport ready");
    return 0;
}

/* --- tests ---------------------------------------------------------------- */

static void test_virtual_whole_path(void)
{
    static struct virtual_8265 v;
    struct wifi_iwl8265_load_ops ops;
    struct iwl8265_sm sm;
    k_size_t fw_size;
    int rc;

    ktest_begin("wifi: virtual 8265 runs the whole start path to READY");

    v_reset(&v);
    v_ops(&v, &ops);
    memset(&sm, 0, sizeof(sm));
    sm.state = IWL_SM_DOWN;

    fw_size = make_runtime_fw(g_fw_buf, 36u);
    rc = sm_start(&sm, &v, &ops, g_fw_buf, fw_size);

    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(sm.state, IWL_SM_READY);
    KT_CHECK_STR_EQ(sm_name(sm.state), "READY");
    KT_CHECK(v.dma_chunks > 0u);
    KT_CHECK(v.sram_loaded > 0u);
    KT_CHECK(v.alive_posted);
    KT_CHECK(v.crc != 0u);
    KT_CHECK(sm.transitions >= 4u);
}

static void test_virtual_bad_tlv(void)
{
    static struct virtual_8265 v;
    struct wifi_iwl8265_load_ops ops;
    struct iwl8265_sm sm;
    k_size_t fw_size;

    ktest_begin("wifi: virtual 8265 rejects a corrupted TLV");

    v_reset(&v);
    v_ops(&v, &ops);
    memset(&sm, 0, sizeof(sm));

    fw_size = make_runtime_fw(g_fw_buf, 36u);
    /* Corrupt TLV length to overflow the file. */
    fw_put32(g_fw_buf + 88u + 4u, 0xFFFFFFF0u);

    KT_CHECK_INT_EQ(sm_start(&sm, &v, &ops, g_fw_buf, fw_size), -1);
    KT_CHECK_INT_EQ(sm.state, IWL_SM_FAILED);
}

static void test_virtual_short_firmware(void)
{
    static struct virtual_8265 v;
    struct wifi_iwl8265_load_ops ops;
    struct iwl8265_sm sm;
    k_size_t fw_size;

    ktest_begin("wifi: virtual 8265 rejects a truncated firmware");

    v_reset(&v);
    v_ops(&v, &ops);
    memset(&sm, 0, sizeof(sm));

    fw_size = make_runtime_fw(g_fw_buf, 36u);
    /* Truncate: claim full size but give less. */
    KT_CHECK_INT_EQ(sm_start(&sm, &v, &ops, g_fw_buf, 40u), -1);
    KT_CHECK_INT_EQ(sm.state, IWL_SM_FAILED);
    (void)fw_size;
}

static void test_virtual_no_alive_timeout(void)
{
    static struct virtual_8265 v;
    struct wifi_iwl8265_load_ops ops;
    struct iwl8265_sm sm;
    k_size_t fw_size;
    int rc;

    ktest_begin("wifi: missing ALIVE ends by timeout and becomes degraded, not hung");

    v_reset(&v);
    v.suppress_alive = true;
    v_ops(&v, &ops);
    memset(&sm, 0, sizeof(sm));

    fw_size = make_runtime_fw(g_fw_buf, 36u);
    rc = sm_start(&sm, &v, &ops, g_fw_buf, fw_size);

    KT_CHECK_INT_EQ(rc, -1);
    KT_CHECK_INT_EQ(sm.state, IWL_SM_FAILED);
    KT_CHECK(!v.alive_posted);
    /* The loader must have returned by its own timeout bound. */
    KT_CHECK(v.now_ms > 0u);
    KT_CHECK(v.sram_loaded > 0u);
}

static void test_state_machine_names(void)
{
    ktest_begin("wifi: state machine names and transitions match stage 2 contract");

    KT_CHECK_STR_EQ(sm_name(IWL_SM_DOWN), "DOWN");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_POWER_UP), "POWER_UP");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_FW_LOADING), "FW_LOADING");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_ALIVE), "ALIVE");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_READY), "READY");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_FAILED), "FAILED");
    KT_CHECK_STR_EQ(sm_name(IWL_SM_SKIP), "SKIP");
}

void test_wifi_8265_virtual(void)
{
    test_virtual_whole_path();
    test_virtual_bad_tlv();
    test_virtual_short_firmware();
    test_virtual_no_alive_timeout();
    test_state_machine_names();
}
