/*
 * Micro OS — Intel 8265 ucode: the portable reader of the TLV file.
 *
 * The file starts with a fixed header and continues with TLVs. Each TLV has
 * a four byte type, a four byte length and a payload padded to a multiple of
 * four bytes. The simple image TLVs (INST, DATA, INIT, INIT_DATA) are
 * sections of their own at fixed addresses; BOOT does not belong to this
 * family and is refused. The compressed image TLVs (SEC_RT, SEC_INIT) and
 * the paging TLV carry an inline stream of records: a LE32 destination
 * offset, a LE32 size and size-4 bytes of data. The two separator offsets
 * have no data; they keep the CPU boundary and the start of the paging part
 * visible to the loader. There are no offset/length descriptor tables.
 *
 * Every length is checked against the bytes that remain in the buffer, so a
 * truncated or hostile file is answered with WIFI_IWL8265_FW_INVALID and no
 * pointer ever leaves the buffer. The reader has no state of its own.
 */
#include "wifi/iwl8265_fw.h"

#define WIFI_IWL8265_HEADER_SIZE 88u
#define WIFI_IWL8265_TLV_HEADER_SIZE 8u
#define WIFI_IWL8265_RECORD_HEADER_SIZE 8u
#define WIFI_IWL8265_MAGIC 0x0a4c5749u
#define WIFI_IWL8265_HUMAN_SIZE 64u

typedef enum wifi_iwl8265_fw_result fw_result_t;

/* Read little endian bytes explicitly: the file may be unaligned in memory. */
static k_u32_t read_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static void clear_bytes(void *p, k_size_t n)
{
    k_u8_t *b = (k_u8_t *)p;

    while (n-- != 0) {
        *b++ = 0;
    }
}

static void fw_clear(struct wifi_iwl8265_fw *out)
{
    clear_bytes(out, sizeof(*out));
}

static fw_result_t add_section(struct wifi_iwl8265_fw *out, k_u32_t offset, k_u32_t length,
                               const k_u8_t *data, k_u8_t kind)
{
    struct wifi_iwl8265_fw_section *section;

    if (out->count >= WIFI_IWL8265_FW_MAX_SECTIONS) {
        return WIFI_IWL8265_FW_NOSPACE;
    }

    section = &out->sections[out->count];
    section->offset = offset;
    section->length = length;
    section->data = data;
    section->kind = kind;
    out->count++;
    return WIFI_IWL8265_FW_OK;
}

/*
 * The version word is major/minor/API/serial from its most significant byte
 * down. Only bits 15:8 name the API: the low byte is a serial number and may
 * hold any value, including one that looks like a supported API.
 */
static fw_result_t choose_api(k_u32_t ver, k_u32_t *api)
{
    k_u32_t candidate = (ver >> WIFI_IWL8265_VER_API_SHIFT) & WIFI_IWL8265_VER_API_MASK;

    if (candidate >= WIFI_IWL8265_API_MIN && candidate <= WIFI_IWL8265_API_MAX) {
        *api = candidate;
        return WIFI_IWL8265_FW_OK;
    }

    return WIFI_IWL8265_FW_UNSUPPORTED;
}

static void copy_human(char *dst, const k_u8_t *src)
{
    k_size_t i = 0;

    while (i + 1 < WIFI_IWL8265_HUMAN_SIZE && src[i] != 0) {
        dst[i] = (char)src[i];
        i++;
    }
    dst[i] = '\0';
}

/*
 * Walk the inline records of a compressed image or paging TLV. A record is
 * an offset, a size that counts the offset word, and size-4 bytes that
 * follow immediately; the next record starts after them. The separators
 * carry no data and stay in the output as empty sections, so the loader can
 * see where the second CPU and the paging image begin.
 */
static fw_result_t expand_inline(struct wifi_iwl8265_fw *out, const k_u8_t *payload,
                                 k_size_t length, k_u8_t kind, bool *added, bool paging)
{
    k_size_t pos = 0u;

    while (pos < length) {
        k_u32_t offset;
        k_u32_t size;
        k_u32_t data_length;
        fw_result_t result;

        if (length - pos < WIFI_IWL8265_RECORD_HEADER_SIZE) {
            return WIFI_IWL8265_FW_INVALID;
        }
        offset = read_le32(payload + pos);
        size = read_le32(payload + pos + 4u);
        pos += WIFI_IWL8265_RECORD_HEADER_SIZE;

        /* Even a separator has a four byte size: the offset word itself. */
        if (size < 4u) {
            return WIFI_IWL8265_FW_INVALID;
        }
        data_length = size - 4u;
        if (data_length > length - pos) {
            return WIFI_IWL8265_FW_INVALID;
        }

        if (offset == WIFI_IWL8265_CPU2_SEPARATOR) {
            if (data_length != 0u) {
                return WIFI_IWL8265_FW_INVALID;
            }
            result = add_section(out, offset, 0u, NULL, kind);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            out->dual_cpu = true;
            continue;
        }
        if (offset == WIFI_IWL8265_PAGING_SEPARATOR) {
            if (data_length != 0u) {
                return WIFI_IWL8265_FW_INVALID;
            }
            result = add_section(out, offset, 0u, NULL, kind);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            break;
        }
        if (data_length == 0u) {
            continue;
        }

        result = add_section(out, offset, data_length, payload + pos, kind);
        if (result != WIFI_IWL8265_FW_OK) {
            return result;
        }
        *added = true;
        if (paging) {
            if (data_length > 0xffffffffu - out->paging_size) {
                return WIFI_IWL8265_FW_INVALID;
            }
            out->paging_size += data_length;
        }
        pos += data_length;
    }

    return WIFI_IWL8265_FW_OK;
}

enum wifi_iwl8265_fw_result wifi_iwl8265_fw_parse(const k_u8_t *data, k_size_t size,
                                                  struct wifi_iwl8265_fw *out)
{
    bool seen[21] = {false};
    bool have_image = false;
    k_size_t pos;
    fw_result_t result;

    if (out == NULL) {
        return WIFI_IWL8265_FW_INVALID;
    }
    fw_clear(out);

    if (data == NULL || size < WIFI_IWL8265_HEADER_SIZE) {
        return WIFI_IWL8265_FW_INVALID;
    }
    if (read_le32(data) != 0 || read_le32(data + 4) != WIFI_IWL8265_MAGIC) {
        return WIFI_IWL8265_FW_INVALID;
    }

    copy_human(out->version, data + 8);
    result = choose_api(read_le32(data + 72), &out->api);
    if (result != WIFI_IWL8265_FW_OK) {
        return result;
    }
    out->build = read_le32(data + 76);

    pos = WIFI_IWL8265_HEADER_SIZE;
    while (size - pos >= WIFI_IWL8265_TLV_HEADER_SIZE) {
        k_u32_t type = read_le32(data + pos);
        k_u32_t length = read_le32(data + pos + 4);
        const k_u8_t *payload = data + pos + WIFI_IWL8265_TLV_HEADER_SIZE;
        k_size_t remain = size - pos - WIFI_IWL8265_TLV_HEADER_SIZE;
        k_size_t padded;
        k_size_t step;

        if (length > remain) {
            return WIFI_IWL8265_FW_INVALID;
        }

        padded = ((k_size_t)length + 3u) & ~(k_size_t)3u;
        step = WIFI_IWL8265_TLV_HEADER_SIZE + padded;
        if (step > size - pos) {
            /* The last TLV may end exactly at the buffer, without padding. */
            if ((k_size_t)length != remain) {
                return WIFI_IWL8265_FW_INVALID;
            }
            step = size - pos;
        }

        if (type == WIFI_IWL8265_FW_TLV_BOOT) {
            return WIFI_IWL8265_FW_INVALID;
        }
        if ((type >= WIFI_IWL8265_FW_TLV_INST && type <= WIFI_IWL8265_FW_TLV_BOOT) ||
            type == WIFI_IWL8265_FW_TLV_SEC_RT || type == WIFI_IWL8265_FW_TLV_SEC_INIT) {
            if (seen[type]) {
                return WIFI_IWL8265_FW_INVALID;
            }
            seen[type] = true;
        }

        switch (type) {
        case WIFI_IWL8265_FW_TLV_INST:
            result = add_section(out, WIFI_IWL8265_RTC_INST_LOWER_BOUND, length, payload,
                                 WIFI_IWL8265_SECTION_RUNTIME);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            have_image = true;
            break;
        case WIFI_IWL8265_FW_TLV_DATA:
            result = add_section(out, WIFI_IWL8265_RTC_DATA_LOWER_BOUND, length, payload,
                                 WIFI_IWL8265_SECTION_RUNTIME);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            have_image = true;
            break;
        case WIFI_IWL8265_FW_TLV_INIT:
            result = add_section(out, WIFI_IWL8265_RTC_INST_LOWER_BOUND, length, payload,
                                 WIFI_IWL8265_SECTION_INIT);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            have_image = true;
            break;
        case WIFI_IWL8265_FW_TLV_INIT_DATA:
            result = add_section(out, WIFI_IWL8265_RTC_DATA_LOWER_BOUND, length, payload,
                                 WIFI_IWL8265_SECTION_INIT);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            have_image = true;
            break;
        case WIFI_IWL8265_FW_TLV_SEC_RT:
            result = expand_inline(out, payload, length, WIFI_IWL8265_SECTION_RUNTIME, &have_image,
                                   false);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            break;
        case WIFI_IWL8265_FW_TLV_SEC_INIT:
            result =
                expand_inline(out, payload, length, WIFI_IWL8265_SECTION_INIT, &have_image, false);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            break;
        case WIFI_IWL8265_FW_TLV_PAGING: {
            bool paging_added = false;

            result = expand_inline(out, payload, length, WIFI_IWL8265_SECTION_PAGING, &paging_added,
                                   true);
            if (result != WIFI_IWL8265_FW_OK) {
                return result;
            }
            break;
        }
        case 27: /* NUM_OF_CPU: two CPUs share the image. */
            if (length != 4u) {
                return WIFI_IWL8265_FW_INVALID;
            }
            if (read_le32(payload) == 2u) {
                out->dual_cpu = true;
            }
            break;
        default:
            /* FW_VERSION, HW_TYPE, capabilities and unknown TLVs are safe. */
            break;
        }

        pos += step;
    }

    /* One to seven bytes left over are a truncated TLV, not a padding. */
    if (pos != size) {
        return WIFI_IWL8265_FW_INVALID;
    }
    if (!have_image) {
        return WIFI_IWL8265_FW_INVALID;
    }
    return WIFI_IWL8265_FW_OK;
}
