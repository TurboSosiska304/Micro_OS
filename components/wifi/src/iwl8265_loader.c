/*
 * Micro OS — original Intel 8265 PCIe firmware loader.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/pcie/trans.c,
 * pcie/ctxt-info.c, pcie/rx.c, pcie/tx.c, iwl-context-info.h,
 * iwl-fh.h and iwl-prph.h (GPL-2.0, Linux v6.12);
 * code written here (MIT).
 */
#include "wifi/iwl8265_loader.h"

#include <string.h>

#include "kernel/log.h"

/* CSR/HBUS offsets in BAR0. */
#define IWL_CSR_INT 0x008u
#define IWL_CSR_INT_MASK 0x00Cu
#define IWL_CSR_FH_INT_STATUS 0x010u
#define IWL_CSR_RESET 0x020u
#define IWL_CSR_GP_CNTRL 0x024u
#define IWL_CSR_HW_REV 0x028u
#define IWL_CSR_CTXT_INFO_BA 0x040u
#define IWL_CSR_MAC_SHADOW_REG_CTRL 0x0A8u

#define IWL_CSR_INT_FH_TX (1u << 27)
#define IWL_CSR_INT_FH_RX (1u << 31)
#define IWL_CSR_INT_ALIVE (1u << 0)
#define IWL_CSR_INT_HW_ERR (1u << 29)
#define IWL_CSR_INT_SW_ERR (1u << 25)
#define IWL_CSR_FH_TX_MASK 0x3u
#define IWL_CSR_GP_MAC_ACCESS_REQ 0x8u
#define IWL_CSR_GP_CLOCK_READY 0x1u
#define IWL_CSR_GP_GOING_TO_SLEEP 0x10u
#define IWL_CSR_HW_NIC_READY 0x00400000u

/* Flow-handler registers are direct BAR0 offsets on this family. */
#define IWL_FH_TCSR_LOWER 0x1D00u
#define IWL_FH_TCSR_CONFIG(q) (IWL_FH_TCSR_LOWER + 0x20u * (q))
#define IWL_FH_TCSR_BUF_STATUS(q) (IWL_FH_TCSR_LOWER + 0x20u * (q) + 0x08u)
#define IWL_FH_TFDIB_CTRL0(q) (0x1900u + 0x08u * (q))
#define IWL_FH_TFDIB_CTRL1(q) (0x1904u + 0x08u * (q))
#define IWL_FH_SRVC_SRAM_ADDR 0x19C8u

#define IWL_TCSR_DMA_ENABLE 0x80000000u
#define IWL_TCSR_CIRQ_ENDTFD 0x00100000u
#define IWL_BUF_TB_NUM (1u << 20)
#define IWL_BUF_TB_IDX (1u << 12)
#define IWL_BUF_VALID 0x3u

/* Internal periphery accessed through the HBUS window. */
#define IWL_PRPH_RELEASE_CPU_RESET 0x300Cu
#define IWL_RELEASE_CPU_RESET_BIT (1u << 24)
#define IWL_PRPH_WFPM_GP2 0xA030B4u
#define IWL_PRPH_UREG_UCODE_LOAD_STATUS 0xA05C40u
#define IWL_PRPH_FH_UCODE_LOAD_STATUS 0x1AF0u
#define IWL_PRPH_RFH_Q0_FRBDCB_WIDX_TRG 0x1C80u
#define IWL_PRPH_RFH_Q0_FRBDCB_RIDX 0xA080C0u
#define IWL_PRPH_RFH_RXF_DMA_CFG 0xA09820u
#define IWL_PRPH_RFH_GEN_CFG 0xA09800u
#define IWL_PRPH_RFH_RXF_RXQ_ACTIVE 0xA0980Cu

#define IWL_DMA_CHUNK_MAX 0x20000u
#define IWL_IMAGE_MAX_ENTRIES 16u

struct image_blocks {
    k_size_t count;
    k_size_t lmac_count;
    struct {
        const k_u8_t *data;
        k_u32_t length;
    } block[IWL_IMAGE_MAX_ENTRIES];
    k_size_t paging_count;
    struct {
        const k_u8_t *data;
        k_u32_t length;
    } paging[IWL_IMAGE_MAX_ENTRIES];
};

static k_u32_t get_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static k_u16_t get_le16(const k_u8_t *p)
{
    return (k_u16_t)((k_u16_t)p[0] | ((k_u16_t)p[1] << 8));
}

static void put_le16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)value;
    p[1] = (k_u8_t)(value >> 8);
}

static void put_le32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)value;
    p[1] = (k_u8_t)(value >> 8);
    p[2] = (k_u8_t)(value >> 16);
    p[3] = (k_u8_t)(value >> 24);
}

static void put_le64(k_u8_t *p, k_u64_t value)
{
    put_le32(p, (k_u32_t)value);
    put_le32(p + 4u, (k_u32_t)(value >> 32));
}

static int read32(const struct wifi_iwl8265_load_ops *ops, k_u32_t offset, k_u32_t *value)
{
    if (ops->read == NULL) {
        return -1;
    }
    *value = ops->read(ops->context, offset);
    return *value == 0xFFFFFFFFu ? -1 : 0;
}

static int read_prph(const struct wifi_iwl8265_load_ops *ops, k_u32_t offset, k_u32_t *value)
{
    if (ops->read_prph == NULL) {
        return -1;
    }
    *value = ops->read_prph(ops->context, offset);
    return *value == 0x5A5A5A5Au || *value == 0xFFFFFFFFu ? -1 : 0;
}

static int wait_register(const struct wifi_iwl8265_load_ops *ops, bool prph, k_u32_t offset,
                         k_u32_t mask, k_u32_t wanted, k_u32_t timeout_ms, const char *what)
{
    k_u64_t start = ops->now_ms(ops->context);
    k_u32_t value = 0u;

    for (;;) {
        int rc = prph ? read_prph(ops, offset, &value) : read32(ops, offset, &value);
        if (rc == 0 && (value & mask) == wanted) {
            return 0;
        }
        if ((k_u32_t)(ops->now_ms(ops->context) - start) >= timeout_ms) {
            kerror("wifi: timeout waiting for %s at 0x%x (value 0x%08x, %u ms)", what,
                   (unsigned)offset, (unsigned)value, (unsigned)timeout_ms);
            return -1;
        }
    }
}

static int grab_mac(const struct wifi_iwl8265_load_ops *ops)
{
    k_u32_t value;

    if (read32(ops, IWL_CSR_GP_CNTRL, &value) != 0) {
        return -1;
    }
    ops->write(ops->context, IWL_CSR_GP_CNTRL, value | IWL_CSR_GP_MAC_ACCESS_REQ);
    return wait_register(ops, false, IWL_CSR_GP_CNTRL,
                         IWL_CSR_GP_CLOCK_READY | IWL_CSR_GP_GOING_TO_SLEEP, IWL_CSR_GP_CLOCK_READY,
                         15u, "MAC clock");
}

static void release_mac(const struct wifi_iwl8265_load_ops *ops)
{
    k_u32_t value;

    if (read32(ops, IWL_CSR_GP_CNTRL, &value) == 0) {
        ops->write(ops->context, IWL_CSR_GP_CNTRL, value & ~IWL_CSR_GP_MAC_ACCESS_REQ);
    }
}

static int clear_pending(const struct wifi_iwl8265_load_ops *ops)
{
    k_u32_t value;

    if (read32(ops, IWL_CSR_INT, &value) != 0) {
        return -1;
    }
    ops->write(ops->context, IWL_CSR_INT, value);
    if (read32(ops, IWL_CSR_FH_INT_STATUS, &value) != 0) {
        return -1;
    }
    ops->write(ops->context, IWL_CSR_FH_INT_STATUS, value);
    return 0;
}

static int wait_dma_done(const struct wifi_iwl8265_load_ops *ops, const char *what)
{
    k_u64_t start = ops->now_ms(ops->context);

    for (;;) {
        k_u32_t intr;

        if (read32(ops, IWL_CSR_INT, &intr) != 0) {
            return -1;
        }
        if ((intr & IWL_CSR_INT_HW_ERR) != 0u || (intr & IWL_CSR_INT_SW_ERR) != 0u) {
            kerror("wifi: device error 0x%08x while %s", (unsigned)intr, what);
            return -1;
        }
        if ((intr & IWL_CSR_INT_FH_TX) != 0u) {
            ops->write(ops->context, IWL_CSR_INT, IWL_CSR_INT_FH_TX);
            ops->write(ops->context, IWL_CSR_FH_INT_STATUS, IWL_CSR_FH_TX_MASK);
            return 0;
        }
        if ((k_u32_t)(ops->now_ms(ops->context) - start) >= WIFI_IWL8265_LOAD_WAIT_MS) {
            kerror("wifi: timeout waiting for DMA completion of %s", what);
            return -1;
        }
    }
}

static int load_chunk(const struct wifi_iwl8265_load_ops *ops, k_u32_t destination,
                      const k_u8_t *data, k_u32_t length)
{
    k_u64_t physical = ops->dma_phys(ops->context, data);
    k_u32_t high;

    if (physical == 0u || (physical & 0xFFFu) != 0u || length == 0u || length > IWL_DMA_CHUNK_MAX) {
        return -1;
    }
    high = (k_u32_t)(physical >> 28) & 0xFu;

    if (grab_mac(ops) != 0) {
        return -1;
    }

    ops->write(ops->context, IWL_FH_TCSR_CONFIG(9u), 0u);
    ops->write(ops->context, IWL_FH_SRVC_SRAM_ADDR, destination);
    ops->write(ops->context, IWL_FH_TFDIB_CTRL0(9u), (k_u32_t)physical);
    ops->write(ops->context, IWL_FH_TFDIB_CTRL1(9u), (high << 28) | length);
    ops->write(ops->context, IWL_FH_TCSR_BUF_STATUS(9u),
               IWL_BUF_TB_NUM | IWL_BUF_TB_IDX | IWL_BUF_VALID);
    ops->write(ops->context, IWL_FH_TCSR_CONFIG(9u), IWL_TCSR_DMA_ENABLE | IWL_TCSR_CIRQ_ENDTFD);

    release_mac(ops);
    return wait_dma_done(ops, "firmware chunk");
}

static int load_section(const struct wifi_iwl8265_load_ops *ops,
                        const struct wifi_iwl8265_fw_section *section)
{
    k_u32_t done = 0u;

    while (done < section->length) {
        k_u32_t amount = section->length - done;
        k_u32_t destination = section->offset + done;

        if (amount > IWL_DMA_CHUNK_MAX) {
            amount = IWL_DMA_CHUNK_MAX;
        }
        if (load_chunk(ops, destination, section->data + done, amount) != 0) {
            return -1;
        }
        done += amount;
    }
    return 0;
}

static int append_block(struct image_blocks *image, const k_u8_t *data, k_u32_t length, bool paging)
{
    k_size_t *count = paging ? &image->paging_count : &image->count;
    k_size_t index = *count;

    if (index >= IWL_IMAGE_MAX_ENTRIES) {
        return -1;
    }
    if (paging) {
        image->paging[index].data = data;
        image->paging[index].length = length;
    } else {
        image->block[index].data = data;
        image->block[index].length = length;
    }
    (*count)++;
    return 0;
}

static int split_image(const struct wifi_iwl8265_fw *firmware, struct image_blocks *image)
{
    k_size_t i;
    k_size_t paging_start = firmware->count;
    k_size_t lmac_count = 0u;
    k_size_t umac_count = 0u;
    bool lmac_separator = false;

    memset(image, 0, sizeof(*image));
    for (i = 0u; i < firmware->count; i++) {
        const struct wifi_iwl8265_fw_section *section = &firmware->sections[i];

        if (section->offset == WIFI_IWL8265_CPU2_SEPARATOR) {
            lmac_separator = true;
            continue;
        }
        if (section->offset == WIFI_IWL8265_PAGING_SEPARATOR) {
            continue;
        }
        if (section->kind == WIFI_IWL8265_SECTION_PAGING) {
            paging_start = i;
            break;
        }
        if (section->kind != WIFI_IWL8265_SECTION_RUNTIME || section->length == 0u) {
            continue;
        }
        if (!lmac_separator) {
            if (append_block(image, section->data, section->length, false) != 0) {
                return -1;
            }
            lmac_count++;
        } else {
            if (append_block(image, section->data, section->length, false) != 0) {
                return -1;
            }
            umac_count++;
        }
    }

    if (image->count != lmac_count + umac_count || image->count == 0u) {
        return -1;
    }
    image->lmac_count = lmac_count;
    for (i = paging_start; i < firmware->count; i++) {
        const struct wifi_iwl8265_fw_section *section = &firmware->sections[i];
        if (section->kind != WIFI_IWL8265_SECTION_PAGING) {
            return -1;
        }
        if (append_block(image, section->data, section->length, true) != 0) {
            return -1;
        }
    }
    return 0;
}

static int allocate_dma(const struct wifi_iwl8265_load_ops *ops, k_size_t size, k_size_t alignment,
                        void **buffer, k_u64_t *physical)
{
    if (size == 0u || alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        ops->alloc == NULL || ops->free == NULL || ops->dma_phys == NULL) {
        return -1;
    }
    *buffer = ops->alloc(ops->context, size, alignment, physical);
    if (*buffer == NULL || *physical == 0u || (*physical & (alignment - 1u)) != 0u ||
        ops->dma_phys(ops->context, *buffer) != *physical) {
        if (*buffer != NULL) {
            ops->free(ops->context, *buffer, size);
        }
        *buffer = NULL;
        *physical = 0u;
        return -1;
    }
    return 0;
}

struct context_layout {
    k_size_t offset_rbd;
    k_size_t offset_hcmd;
    k_size_t offset_dram;
};

static void context_layout(struct context_layout *layout)
{
    /* Original packed on-wire layout from iwl-context-info.h. */
    layout->offset_rbd = 24u;
    layout->offset_hcmd = 48u;
    layout->offset_dram = 208u;
}

static int build_context(const struct wifi_iwl8265_load_ops *ops, const struct image_blocks *image,
                         k_u64_t *context_phys, k_u8_t **context_out)
{
    struct context_layout layout;
    k_u8_t *context = NULL;
    k_u64_t physical = 0u;
    k_size_t size = WIFI_IWL8265_CONTEXT_BYTES;
    k_size_t i;
    k_u32_t hw_rev = 0u;

    if (read32(ops, IWL_CSR_HW_REV, &hw_rev) != 0 ||
        allocate_dma(ops, size, 256u, (void **)&context, &physical) != 0) {
        return -1;
    }
    memset(context, 0, size);
    context_layout(&layout);

    put_le16(context + 0u, (k_u16_t)hw_rev);
    put_le16(context + 2u, 0u);
    put_le16(context + 4u, (k_u16_t)(size / 4u));
    /* Long TFD, 256 RBD ring (exponent 8), 4 KiB receive buffers. */
    put_le32(context + 8u, 0x0100u | (8u << 4) | (4u << 9));

    /* Map LMAC1, UMAC and paging in their separate descriptor arrays. */
    for (i = 0u; i < image->lmac_count; i++) {
        put_le64(context + layout.offset_dram + 512u + i * 8u,
                 ops->dma_phys(ops->context, image->block[i].data));
    }
    for (i = image->lmac_count; i < image->count; i++) {
        k_size_t index = image->count - i - 1u;

        put_le64(context + layout.offset_dram + index * 8u,
                 ops->dma_phys(ops->context, image->block[i].data));
    }

    /* The compact loader uses runtime sections only; UMAC/paging maps stay
     * zero unless explicitly represented by the parsed image. */
    for (i = 0u; i < image->paging_count; i++) {
        if (i >= 64u) {
            ops->free(ops->context, context, size);
            return -1;
        }
        put_le64(context + layout.offset_dram + 1024u + i * 8u,
                 ops->dma_phys(ops->context, image->paging[i].data));
    }

    *context_phys = physical;
    *context_out = context;
    return 0;
}

static int wait_alive(const struct wifi_iwl8265_load_ops *ops, const k_u8_t *rx, k_size_t rx_size,
                      k_u32_t *read_index, struct wifi_iwl8265_alive *alive)
{
    k_u64_t start = ops->now_ms(ops->context);

    while ((k_u32_t)(ops->now_ms(ops->context) - start) < WIFI_IWL8265_ALIVE_WAIT_MS) {
        k_u32_t value;
        k_u32_t index;

        if (read_prph(ops, IWL_PRPH_RFH_Q0_FRBDCB_RIDX, &index) != 0) {
            return -1;
        }
        if (index != *read_index) {
            k_u32_t rbd = get_le32(rx + ((k_size_t)(*read_index & 255u) * 8u));
            k_u32_t buffer_offset = rbd >> 8u;
            const k_u8_t *packet;
            k_u32_t length;

            if ((rbd & 1u) == 0u || buffer_offset > rx_size - 4u) {
                kerror("wifi: invalid RX descriptor 0x%08x", (unsigned)rbd);
                return -1;
            }
            packet = rx + buffer_offset;
            length = get_le32(packet) & 0x3FFFu;
            if (length > rx_size - buffer_offset || length < 12u) {
                kerror("wifi: invalid RX packet length %u", (unsigned)length);
                return -1;
            }

            if (get_le16(packet + 4u) == 1u && get_le16(packet + 8u) == 0xCAFEu && length >= 52u) {
                alive->status = 0xCAFEu;
                alive->flags = get_le16(packet + 10u);
                alive->lmac_major = get_le32(packet + 12u);
                alive->lmac_minor = get_le32(packet + 16u);
                alive->umac_major = get_le32(packet + 44u);
                alive->umac_minor = get_le32(packet + 48u);
                alive->scd_base = get_le32(packet + 36u);
                alive->valid = true;
            }
            *read_index = (*read_index + 1u) & 255u;
            ops->write_prph(ops->context, IWL_PRPH_RFH_Q0_FRBDCB_WIDX_TRG, *read_index);
        }
        if (alive->valid) {
            return 0;
        }

        value = 0u;
        if (read32(ops, IWL_CSR_INT, &value) == 0) {
            ops->write(ops->context, IWL_CSR_INT, value);
        }
        if (read32(ops, IWL_CSR_FH_INT_STATUS, &value) == 0) {
            ops->write(ops->context, IWL_CSR_FH_INT_STATUS, value);
        }
    }

    kerror("wifi: timeout waiting for ALIVE notification");
    return -1;
}

int wifi_iwl8265_load(const struct wifi_iwl8265_fw *firmware,
                      const struct wifi_iwl8265_load_ops *ops, struct wifi_iwl8265_alive *alive)
{
    struct image_blocks image;
    k_u8_t *rbd = NULL;
    k_u8_t *used = NULL;
    k_u8_t *status = NULL;
    k_u8_t *chunk = NULL;
    k_u8_t *context = NULL;
    k_u64_t rbd_phys = 0u;
    k_u64_t used_phys = 0u;
    k_u64_t status_phys = 0u;
    k_u64_t chunk_phys = 0u;
    k_u64_t context_phys = 0u;
    k_size_t i;
    k_u32_t value;
    k_u32_t load_status;
    k_u32_t read_index = 0u;
    int result = -1;

    if (firmware == NULL || ops == NULL || alive == NULL || ops->now_ms == NULL ||
        ops->write == NULL || ops->write64 == NULL || ops->write_prph == NULL) {
        return -1;
    }
    memset(alive, 0, sizeof(*alive));
    if (split_image(firmware, &image) != 0) {
        kerror("wifi: firmware image has no usable runtime sections");
        return -1;
    }

    if (allocate_dma(ops, 256u * 8u, 256u, (void **)&rbd, &rbd_phys) != 0 ||
        allocate_dma(ops, 256u * 8u, 256u, (void **)&used, &used_phys) != 0 ||
        allocate_dma(ops, 32u, 16u, (void **)&status, &status_phys) != 0 ||
        allocate_dma(ops, IWL_DMA_CHUNK_MAX, 4096u, (void **)&chunk, &chunk_phys) != 0) {
        goto out;
    }

    /* Direct 8000-family RX ring initialization. */
    ops->write(ops->context, 0x1C00u, 0u);
    ops->write(ops->context, 0x1BC0u, (k_u32_t)(rbd_phys >> 8));
    ops->write(ops->context, 0x1BC4u, (k_u32_t)(rbd_phys >> 36));
    ops->write(ops->context, 0x1BC8u, 0u);
    ops->write(ops->context, 0x1BCCu, (k_u32_t)(status_phys >> 4));
    ops->write(ops->context, 0x1BD0u, (k_u32_t)(status_phys >> 32));
    ops->write(ops->context, 0x1C00u, 0xC0708010u);
    ops->write_prph(ops->context, IWL_PRPH_RFH_RXF_DMA_CFG, 0u);
    ops->write_prph(ops->context, IWL_PRPH_RFH_RXF_RXQ_ACTIVE, 0u);
    ops->write_prph(ops->context, 0xA08000u, (k_u32_t)rbd_phys);
    ops->write_prph(ops->context, 0xA08004u, (k_u32_t)(rbd_phys >> 32));
    ops->write_prph(ops->context, 0xA08100u, (k_u32_t)used_phys);
    ops->write_prph(ops->context, 0xA08104u, (k_u32_t)(used_phys >> 32));
    ops->write_prph(ops->context, 0xA08200u, (k_u32_t)status_phys);
    ops->write_prph(ops->context, 0xA08204u, (k_u32_t)(status_phys >> 32));
    ops->write_prph(ops->context, 0xA08080u, 0u);
    ops->write_prph(ops->context, 0xA080C0u, 0u);
    ops->write_prph(ops->context, 0xA08180u, 0u);
    ops->write_prph(ops->context, IWL_PRPH_RFH_RXF_DMA_CFG, 0x8B039000u);
    ops->write_prph(ops->context, IWL_PRPH_RFH_GEN_CFG, 0x13u);
    ops->write_prph(ops->context, IWL_PRPH_RFH_RXF_RXQ_ACTIVE, 0x00010001u);

    /* The single RX buffer is large enough for ALIVE and stays DMA-coherent. */
    memset(rbd, 0, 256u * 8u);
    put_le32(rbd, (k_u32_t)(chunk_phys >> 8));
    put_le32(rbd + 4u, (k_u32_t)(chunk_phys >> 40));
    ops->write_prph(ops->context, IWL_PRPH_RFH_Q0_FRBDCB_WIDX_TRG, 0u);

    if (clear_pending(ops) != 0) {
        goto out;
    }
    ops->write(ops->context, IWL_CSR_INT_MASK, IWL_CSR_INT_FH_TX);
    ops->write(ops->context, IWL_CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    ops->write(ops->context, IWL_CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFFu);

    /* Copy paging before the CPU is released; the context map must name the
     * final bytes, not an allocation filled later. */
    for (i = 0u; i < firmware->count; i++) {
        const struct wifi_iwl8265_fw_section *section = &firmware->sections[i];

        if (section->kind != WIFI_IWL8265_SECTION_PAGING) {
            continue;
        }
        {
            k_u32_t done = 0u;

            while (done < section->length) {
                k_u32_t amount = section->length - done;
                k_size_t offset = (k_size_t)(section->offset + done);

                if (amount > IWL_DMA_CHUNK_MAX) {
                    amount = IWL_DMA_CHUNK_MAX;
                }
                if (offset > IWL_DMA_CHUNK_MAX - amount) {
                    goto out;
                }
                memcpy(chunk + offset, section->data + done, amount);
                done += amount;
            }
        }
    }

    /* Build the DRAM map while the command queue and RX ring exist. */
    if (build_context(ops, &image, &context_phys, &context) != 0) {
        goto out;
    }

    if (grab_mac(ops) != 0) {
        goto out;
    }
    ops->write_prph(ops->context, IWL_PRPH_WFPM_GP2, 0x01010101u);
    ops->write_prph(ops->context, IWL_PRPH_RELEASE_CPU_RESET, IWL_RELEASE_CPU_RESET_BIT);
    release_mac(ops);

    load_status = 0u;
    for (i = 0u; i < firmware->count; i++) {
        const struct wifi_iwl8265_fw_section *section = &firmware->sections[i];
        k_u32_t bit;

        if (section->kind == WIFI_IWL8265_SECTION_PAGING) {
            continue;
        }
        if (section->offset == WIFI_IWL8265_CPU2_SEPARATOR) {
            load_status = 0u;
            continue;
        }
        if (section->offset == WIFI_IWL8265_PAGING_SEPARATOR ||
            section->kind == WIFI_IWL8265_SECTION_INIT || section->length == 0u) {
            continue;
        }
        if (load_section(ops, section) != 0) {
            goto out;
        }
        bit = (i & 15u) < 15u ? (1u << (i & 15u)) : 0u;
        load_status |= bit;
        if (grab_mac(ops) == 0) {
            value = 0u;
            (void)read_prph(ops, IWL_PRPH_FH_UCODE_LOAD_STATUS, &value);
            ops->write_prph(ops->context, IWL_PRPH_FH_UCODE_LOAD_STATUS, value | bit);
            release_mac(ops);
        }
    }

    /* ALIVE and its RX notification are enabled after the image is loaded. */
    ops->write(ops->context, IWL_CSR_INT_MASK, IWL_CSR_INT_ALIVE | IWL_CSR_INT_FH_RX);
    ops->write_prph(ops->context, IWL_PRPH_UREG_UCODE_LOAD_STATUS, load_status);
    if (grab_mac(ops) == 0) {
        ops->write_prph(ops->context, IWL_PRPH_FH_UCODE_LOAD_STATUS, 0xFFFFFFFFu);
        release_mac(ops);
    }

    /* The context descriptor names the image and paging DRAM allocations. */
    ops->write64(ops->context, IWL_CSR_CTXT_INFO_BA, context_phys);
    ops->write(ops->context, IWL_CSR_INT_MASK, IWL_CSR_INT_FH_TX);

    result = wait_alive(ops, chunk, IWL_DMA_CHUNK_MAX, &read_index, alive);

out:
    ops->write(ops->context, IWL_CSR_INT_MASK, 0u);
    if (context != NULL && context_phys != 0u && ops->free != NULL) {
        ops->free(ops->context, context, WIFI_IWL8265_CONTEXT_BYTES);
    }
    if (chunk != NULL) {
        ops->free(ops->context, chunk, IWL_DMA_CHUNK_MAX);
    }
    if (status != NULL) {
        ops->free(ops->context, status, 32u);
    }
    if (used != NULL) {
        ops->free(ops->context, used, 256u * 8u);
    }
    if (rbd != NULL) {
        ops->free(ops->context, rbd, 256u * 8u);
    }
    return result;
}
