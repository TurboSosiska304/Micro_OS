/*
 * Micro OS — the tests of the Intel 8265 ucode reader.
 *
 * No firmware blob is needed: every case is built from bytes here, the way
 * a hostile or truncated file would arrive from a disk. The buffer lives in
 * static storage because the output of the reader points back into it.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "wifi/iwl8265_fw.h"

#define FW_CAPACITY 1024

static k_u8_t g_fw[FW_CAPACITY];

static void put32(k_u8_t *p, k_u32_t v)
{
    p[0] = (k_u8_t)v;
    p[1] = (k_u8_t)(v >> 8);
    p[2] = (k_u8_t)(v >> 16);
    p[3] = (k_u8_t)(v >> 24);
}

static void set_bytes(k_u8_t *p, k_u8_t value, k_size_t n)
{
    while (n-- != 0) {
        *p++ = value;
    }
}

/* major/minor/API/serial: the API lives above the serial, not in it. */
static k_u32_t ver_word(k_u32_t api, k_u32_t serial)
{
    return (9u << 24) | (7u << 16) | (api << 8) | serial;
}

static k_size_t make_header(k_u8_t *b, k_u32_t ver, k_u32_t build, const char *human)
{
    k_size_t i = 0;

    put32(b, 0);
    put32(b + 4, 0x0a4c5749u);

    while (human[i] != '\0' && i < 63) {
        b[8 + i] = (k_u8_t)human[i];
        i++;
    }
    set_bytes(b + 8 + i, 0, 64 - i);

    put32(b + 72, ver);
    put32(b + 76, build);
    set_bytes(b + 80, 0, 8);
    return 88;
}

static k_size_t image_header(k_u8_t *b, k_u32_t api, k_u32_t build, const char *human)
{
    return make_header(b, ver_word(api, 3u), build, human);
}

static k_size_t add_tlv(k_u8_t *b, k_size_t pos, k_u32_t type, const k_u8_t *payload,
                        k_u32_t length)
{
    k_size_t padded = ((k_size_t)length + 3u) & ~(k_size_t)3u;
    k_size_t i;

    put32(b + pos, type);
    put32(b + pos + 4, length);
    for (i = 0; i < length; i++) {
        b[pos + 8 + i] = payload[i];
    }
    set_bytes(b + pos + 8 + length, 0, padded - length);
    return pos + 8 + padded;
}

/* One inline record: destination offset, size of offset plus data, data. */
static k_size_t put_record(k_u8_t *p, k_u32_t offset, const k_u8_t *data, k_size_t n)
{
    k_size_t i;

    put32(p, offset);
    put32(p + 4, (k_u32_t)n + 4u);
    for (i = 0; i < n; i++) {
        p[8 + i] = data[i];
    }
    return 8u + n;
}

static k_size_t put_separator(k_u8_t *p, k_u32_t offset)
{
    put32(p, offset);
    put32(p + 4, 4u);
    return 8u;
}

static k_size_t minimal(k_u8_t *b, k_u32_t api)
{
    const k_u8_t code[4] = {0x11, 0x22, 0x33, 0x44};
    k_size_t pos = image_header(b, api, 0xc8e8e144u, "iwlwifi-8265-36.ucode");

    return add_tlv(b, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
}

static void test_the_header(void)
{
    struct wifi_iwl8265_fw fw;
    const k_u8_t code[4] = {0xde, 0xad, 0xbe, 0xef};
    const k_u8_t data[4] = {1, 2, 3, 4};
    k_size_t pos;

    ktest_begin("wifi: the header of a 8265 firmware gives api and build");

    pos = make_header(g_fw, (0x12u << 24) | (0x34u << 16) | (22u << 8) | 0x56u, 0xc8e8e144u,
                      "iwlwifi-8265-22.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_DATA, data, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.api, 22);
    KT_CHECK_INT_EQ(fw.build, 0xc8e8e144u);
    KT_CHECK_STR_EQ(fw.version, "iwlwifi-8265-22.ucode");
    KT_CHECK_INT_EQ(fw.count, 2);
    KT_CHECK_INT_EQ(fw.sections[0].kind, WIFI_IWL8265_SECTION_RUNTIME);
    KT_CHECK_INT_EQ(fw.sections[0].offset, WIFI_IWL8265_RTC_INST_LOWER_BOUND);
    KT_CHECK_INT_EQ(fw.sections[1].kind, WIFI_IWL8265_SECTION_RUNTIME);
    KT_CHECK_INT_EQ(fw.sections[1].offset, WIFI_IWL8265_RTC_DATA_LOWER_BOUND);
    KT_CHECK_INT_EQ(fw.sections[0].data[0], 0xde);
    KT_CHECK_INT_EQ(fw.sections[1].data[3], 4);
    KT_CHECK(!fw.dual_cpu);
    KT_CHECK_INT_EQ(fw.paging_size, 0);

    /* 64 human bytes without a NUL are bounded to 63 plus a terminator. */
    pos = image_header(g_fw, 36u, 7u, "");
    set_bytes(g_fw + 8, 'A', 64);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.api, 36);
    KT_CHECK_INT_EQ(fw.version[63], '\0');
    KT_CHECK_INT_EQ(fw.version[0], 'A');
    KT_CHECK_INT_EQ(fw.version[62], 'A');

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(NULL, 88, &fw), WIFI_IWL8265_FW_INVALID);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, NULL), WIFI_IWL8265_FW_INVALID);
}

static void test_the_sections(void)
{
    struct wifi_iwl8265_fw fw;
    k_u8_t sec[64];
    const k_u8_t data[4] = {9, 8, 7, 6};
    const k_u8_t odd[3] = {5, 5, 5};
    const k_u8_t first[4] = {0x11, 0x22, 0x33, 0x44};
    const k_u8_t second[3] = {5, 6, 7};
    k_size_t pos;
    k_size_t n = 0u;

    ktest_begin("wifi: inline SEC_RT records and their separators expand");

    n += put_record(sec + n, 0x800000u, first, 4);
    n += put_separator(sec + n, WIFI_IWL8265_CPU2_SEPARATOR);
    n += put_record(sec + n, 0xa08000u, second, 3);
    n += put_separator(sec + n, WIFI_IWL8265_PAGING_SEPARATOR);

    pos = image_header(g_fw, 36u, 42u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, (k_u32_t)n);
    pos = add_tlv(g_fw, pos, 99u, odd, 3); /* padded to four bytes */
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_DATA, data, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.count, 5);
    KT_CHECK(fw.dual_cpu);
    KT_CHECK_INT_EQ(fw.sections[0].kind, WIFI_IWL8265_SECTION_RUNTIME);
    KT_CHECK_INT_EQ(fw.sections[0].offset, 0x800000u);
    KT_CHECK_INT_EQ(fw.sections[0].length, 4);
    KT_CHECK_INT_EQ(fw.sections[0].data[0], 0x11);
    KT_CHECK_INT_EQ(fw.sections[1].offset, WIFI_IWL8265_CPU2_SEPARATOR);
    KT_CHECK_INT_EQ(fw.sections[1].length, 0);
    KT_CHECK(fw.sections[1].data == NULL);
    KT_CHECK_INT_EQ(fw.sections[2].kind, WIFI_IWL8265_SECTION_RUNTIME);
    KT_CHECK_INT_EQ(fw.sections[2].offset, 0xa08000u);
    KT_CHECK_INT_EQ(fw.sections[2].length, 3);
    KT_CHECK_INT_EQ(fw.sections[2].data[2], 7);
    KT_CHECK_INT_EQ(fw.sections[3].offset, WIFI_IWL8265_PAGING_SEPARATOR);
    KT_CHECK_INT_EQ(fw.sections[3].length, 0);
    KT_CHECK(fw.sections[3].data == NULL);
    KT_CHECK_INT_EQ(fw.sections[4].kind, WIFI_IWL8265_SECTION_RUNTIME);
    KT_CHECK_INT_EQ(fw.sections[4].data[0], 9);
}

static void test_the_init_sections(void)
{
    struct wifi_iwl8265_fw fw;
    k_u8_t sec[32];
    const k_u8_t code[4] = {0xa1, 0xa2, 0xa3, 0xa4};
    k_size_t pos;
    k_size_t n = 0u;

    ktest_begin("wifi: SEC_INIT and INIT TLVs are init sections, not runtime");

    n += put_record(sec + n, 0x800000u, code, 4);
    pos = image_header(g_fw, 36u, 9u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_INIT, sec, (k_u32_t)n);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INIT, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INIT_DATA, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.count, 4);
    KT_CHECK_INT_EQ(fw.sections[0].kind, WIFI_IWL8265_SECTION_INIT);
    KT_CHECK_INT_EQ(fw.sections[1].kind, WIFI_IWL8265_SECTION_INIT);
    KT_CHECK_INT_EQ(fw.sections[1].offset, WIFI_IWL8265_RTC_INST_LOWER_BOUND);
    KT_CHECK_INT_EQ(fw.sections[2].kind, WIFI_IWL8265_SECTION_INIT);
    KT_CHECK_INT_EQ(fw.sections[2].offset, WIFI_IWL8265_RTC_DATA_LOWER_BOUND);
    KT_CHECK_INT_EQ(fw.sections[3].kind, WIFI_IWL8265_SECTION_RUNTIME);
}

static void test_the_paging(void)
{
    struct wifi_iwl8265_fw fw;
    k_u8_t paging[64];
    const k_u8_t one[4] = {1, 2, 3, 4};
    const k_u8_t two[8] = {2, 0, 0, 0, 0, 0, 0, 0};
    const k_u8_t cpus[4] = {2, 0, 0, 0};
    k_size_t pos;
    k_size_t n = 0u;

    ktest_begin("wifi: paging records are sections with their bytes intact");

    n += put_record(paging + n, 0x1000u, one, 4);
    n += put_record(paging + n, 0x2000u, two, 8);
    n += put_separator(paging + n, WIFI_IWL8265_PAGING_SEPARATOR);
    /* Bytes after the paging separator end the stream and must be ignored. */
    n += put_record(paging + n, 0x3000u, one, 4);

    pos = image_header(g_fw, 36u, 5u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_PAGING, paging, (k_u32_t)n);
    pos = add_tlv(g_fw, pos, 27u, cpus, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, one, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.paging_size, 12);
    KT_CHECK_INT_EQ(fw.count, 4);
    KT_CHECK(fw.dual_cpu);
    KT_CHECK_INT_EQ(fw.sections[0].kind, WIFI_IWL8265_SECTION_PAGING);
    KT_CHECK_INT_EQ(fw.sections[0].offset, 0x1000u);
    KT_CHECK_INT_EQ(fw.sections[0].length, 4);
    KT_CHECK_INT_EQ(fw.sections[0].data[3], 4);
    KT_CHECK_INT_EQ(fw.sections[1].kind, WIFI_IWL8265_SECTION_PAGING);
    KT_CHECK_INT_EQ(fw.sections[1].offset, 0x2000u);
    KT_CHECK_INT_EQ(fw.sections[1].length, 8);
    KT_CHECK_INT_EQ(fw.sections[1].data[0], 2);
    KT_CHECK_INT_EQ(fw.sections[2].offset, WIFI_IWL8265_PAGING_SEPARATOR);
    KT_CHECK_INT_EQ(fw.sections[2].length, 0);
    KT_CHECK(fw.sections[2].data == NULL);
    KT_CHECK_INT_EQ(fw.sections[3].kind, WIFI_IWL8265_SECTION_RUNTIME);

    /* A paging record shorter than its offset word is invalid. */
    set_bytes(paging, 0, sizeof(paging));
    put32(paging, 0x1000);
    put32(paging + 4, 3);
    pos = image_header(g_fw, 36u, 5u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_PAGING, paging, 8);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, one, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    /* Paging bytes alone are not a CPU image. */
    pos = image_header(g_fw, 36u, 5u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_PAGING, paging + 8, 0);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);
}

static void test_the_api_bounds(void)
{
    struct wifi_iwl8265_fw fw;
    k_size_t api;
    k_size_t pos;

    ktest_begin("wifi: only bits 15:8 of the header are the api");

    for (api = 20; api <= 38; api++) {
        pos = minimal(g_fw, (k_u32_t)api);
        KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw),
                        api >= 22 && api <= 36 ? WIFI_IWL8265_FW_OK : WIFI_IWL8265_FW_UNSUPPORTED);
        if (api >= 22 && api <= 36) {
            KT_CHECK_INT_EQ(fw.api, api);
        }
    }

    /* A serial in the supported range must not be read as the api. */
    pos = make_header(g_fw, ver_word(30u, 36u), 1u, "iwlwifi-8265-30.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, g_fw + 700, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.api, 30);

    /* The old version layout put a real api in the low byte; it is a serial now. */
    pos = make_header(g_fw, ver_word(0u, 22u), 1u, "iwlwifi-8265-22.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, g_fw + 700, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_UNSUPPORTED);

    pos = make_header(g_fw, ver_word(9u, 30u), 1u, "iwlwifi-8265-30.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, g_fw + 700, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_UNSUPPORTED);
}

static void test_the_refusals(void)
{
    struct wifi_iwl8265_fw fw;
    const k_u8_t code[4] = {1, 2, 3, 4};
    k_u8_t sec[32];
    k_size_t pos;

    ktest_begin("wifi: a hostile or a short file is refused");

    /* No room for the whole header. */
    pos = minimal(g_fw, 36u);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, 87, &fw), WIFI_IWL8265_FW_INVALID);

    /* The first word and the magic are part of the contract. */
    g_fw[0] = 1;
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);
    g_fw[0] = 0;
    g_fw[4] = 0;
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);
    g_fw[4] = 0x49;

    /* A TLV whose payload is cut short. */
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    put32(g_fw + pos, WIFI_IWL8265_FW_TLV_INST);
    put32(g_fw + pos + 4, 8);
    set_bytes(g_fw + pos + 8, 0xaa, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 12, &fw), WIFI_IWL8265_FW_INVALID);

    /* Padding must be present unless the payload ends exactly at the buffer. */
    put32(g_fw + pos + 4, 5);
    set_bytes(g_fw + pos + 8, 0xaa, 6);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 14, &fw), WIFI_IWL8265_FW_INVALID);

    /* One to three leftover bytes are a truncated TLV, not padding. */
    pos = minimal(g_fw, 36u);
    set_bytes(g_fw + pos, 0xbb, 3);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 2, &fw), WIFI_IWL8265_FW_INVALID);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 3, &fw), WIFI_IWL8265_FW_INVALID);

    /* A section whose inline data leaves the TLV. */
    put32(sec, 0x1000);
    put32(sec + 4, 8); /* offset word plus four bytes of data */
    sec[8] = 1;
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, 9);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    /* A record size smaller than the offset word, and a separator with data. */
    put32(sec + 4, 3);
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, 8);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    put32(sec, WIFI_IWL8265_CPU2_SEPARATOR);
    put32(sec + 4, 8);
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, 12);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    /* An absurd TLV length and a truncated TLV header. */
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    put32(g_fw + pos, WIFI_IWL8265_FW_TLV_INST);
    put32(g_fw + pos + 4, 0xffffffffu);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 8, &fw), WIFI_IWL8265_FW_INVALID);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos + 4, &fw),
                    WIFI_IWL8265_FW_INVALID); /* no whole TLV and no image */

    /* BOOT images do not exist on this family. */
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_BOOT, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    /* NUM_OF_CPU is exactly one word; a value other than two means one CPU. */
    put32(sec, 1);
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, 27u, sec, 2);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, 27u, sec, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK(!fw.dual_cpu);

    /* Duplicate image basics and an image with no basic TLV at all. */
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);

    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_FW_VERSION, code, 4);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_INVALID);
}

static void test_the_limits(void)
{
    struct wifi_iwl8265_fw fw;
    k_u8_t sec[256];
    const k_u8_t one[1] = {0};
    k_size_t count;
    k_size_t pos;
    k_size_t n = 0u;

    ktest_begin("wifi: the sixteen sections of the result are a hard bound");

    for (count = 0; count < 16; count++) {
        n += put_record(sec + n, 0x1000u + (k_u32_t)count, one, 1);
    }
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, (k_u32_t)n);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.count, 16);
    KT_CHECK_INT_EQ(fw.sections[15].offset, 0x100fu);

    n += put_record(sec + n, 0x2000u, one, 1);
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, (k_u32_t)n);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_NOSPACE);

    /* The empty separator entries take a slot just like the real sections. */
    n = 0u;
    for (count = 0; count < 15; count++) {
        n += put_record(sec + n, 0x1000u + (k_u32_t)count, one, 1);
    }
    n += put_separator(sec + n, WIFI_IWL8265_CPU2_SEPARATOR);
    n += put_separator(sec + n, WIFI_IWL8265_PAGING_SEPARATOR);
    pos = image_header(g_fw, 36u, 1u, "iwlwifi-8265-36.ucode");
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_SEC_RT, sec, (k_u32_t)n);
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_NOSPACE);
}

static void test_the_skips(void)
{
    struct wifi_iwl8265_fw fw;
    const k_u8_t code[4] = {1, 2, 3, 4};
    const k_u8_t empty[1] = {0};
    k_size_t pos;

    ktest_begin("wifi: unknown and empty TLVs are skipped safely");

    pos = image_header(g_fw, 22u, 9u, "iwlwifi-8265-22.ucode");
    pos = add_tlv(g_fw, pos, 0u, code, 4);    /* reserved type */
    pos = add_tlv(g_fw, pos, 1234u, code, 4); /* not known to this reader */
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_HW_TYPE, empty, 0);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_FW_VERSION, code, 4);
    pos = add_tlv(g_fw, pos, WIFI_IWL8265_FW_TLV_INST, code, 4);

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_parse(g_fw, pos, &fw), WIFI_IWL8265_FW_OK);
    KT_CHECK_INT_EQ(fw.count, 1);
    KT_CHECK_INT_EQ(fw.sections[0].kind, WIFI_IWL8265_SECTION_RUNTIME);
}

void test_wifi_8265_fw(void)
{
    test_the_header();
    test_the_sections();
    test_the_init_sections();
    test_the_paging();
    test_the_api_bounds();
    test_the_refusals();
    test_the_limits();
    test_the_skips();
}
