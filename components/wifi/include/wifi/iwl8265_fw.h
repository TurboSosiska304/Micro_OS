/*
 * Micro OS — Intel 8265 ucode: the portable reader of the TLV file.
 *
 * The card is fed by a binary that Intel publishes under a redistributable
 * licence (ADR 0039). Before the driver can send anything to the device it
 * has to know which pieces of that file are the code of the two CPUs and
 * which are the data the card copies into its own memory. This component
 * only reads bytes: no allocation, no platform calls, no clock. The caller
 * owns the buffer and the output structure.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/fw/file.h (GPL-2.0,
 * Linux v6.12); code written here (MIT).
 */
#ifndef MICRO_OS_WIFI_IWL8265_FW_H
#define MICRO_OS_WIFI_IWL8265_FW_H

#include "kernel/types.h"

/* The API numbers the 8265 of the laptop accepts (cfg/8000.c). */
#define WIFI_IWL8265_API_MIN 22
#define WIFI_IWL8265_API_MAX 36

/* The ver word of the header is major/minor/API/serial, in that order. */
#define WIFI_IWL8265_VER_API_SHIFT 8u
#define WIFI_IWL8265_VER_API_MASK 0xffu

/* Where the non-compressed image TLVs are loaded on this family. */
#define WIFI_IWL8265_RTC_INST_LOWER_BOUND 0x800000u
#define WIFI_IWL8265_RTC_DATA_LOWER_BOUND 0xa08000u

/* Separators of the inline stream mark the second CPU and the paging part. */
#define WIFI_IWL8265_CPU2_SEPARATOR 0xffffccccu
#define WIFI_IWL8265_PAGING_SEPARATOR 0xaaaabbbbu

/*
 * The type field of a TLV selects the parser; the section that comes out of
 * it is a runtime, init or paging piece of the image. The caller of the
 * loader uses the kind, not the TLV number, to decide where a piece goes.
 */
enum wifi_iwl8265_fw_tlv {
    WIFI_IWL8265_FW_TLV_INST = 1,
    WIFI_IWL8265_FW_TLV_DATA = 2,
    WIFI_IWL8265_FW_TLV_INIT = 3,
    WIFI_IWL8265_FW_TLV_INIT_DATA = 4,
    WIFI_IWL8265_FW_TLV_BOOT = 5,
    WIFI_IWL8265_FW_TLV_SEC_RT = 19,
    WIFI_IWL8265_FW_TLV_SEC_INIT = 20,
    WIFI_IWL8265_FW_TLV_PAGING = 32,
    WIFI_IWL8265_FW_TLV_FW_VERSION = 36,
    WIFI_IWL8265_FW_TLV_HW_TYPE = 58,
};

enum wifi_iwl8265_fw_section_kind {
    WIFI_IWL8265_SECTION_RUNTIME = 0,
    WIFI_IWL8265_SECTION_INIT = 1,
    WIFI_IWL8265_SECTION_PAGING = 2,
};

enum wifi_iwl8265_fw_result {
    WIFI_IWL8265_FW_OK = 0,
    WIFI_IWL8265_FW_INVALID,
    WIFI_IWL8265_FW_UNSUPPORTED,
    WIFI_IWL8265_FW_NOSPACE,
};

/* The output has room for this many sections; more is a NOSPACE failure. */
#define WIFI_IWL8265_FW_MAX_SECTIONS 16

/* One piece of the image: a window into the caller's firmware buffer. */
struct wifi_iwl8265_fw_section {
    k_u32_t offset;     /* the address the card loads the piece at */
    k_u32_t length;     /* the bytes in data; zero for a separator */
    const k_u8_t *data; /* points into the buffer, NULL for a separator */
    k_u8_t kind;        /* one of enum wifi_iwl8265_fw_section_kind */
};

/* Everything the driver needs before it starts the upload. */
struct wifi_iwl8265_fw {
    k_u32_t api;
    k_u32_t build;
    char version[64];
    k_size_t count;
    struct wifi_iwl8265_fw_section sections[WIFI_IWL8265_FW_MAX_SECTIONS];
    bool dual_cpu;       /* the CPU1/CPU2 separator was present */
    k_u32_t paging_size; /* the sum of the paging segments, not a section */
};

/*
 * Read the TLV file. The sections of the result point into data, which must
 * stay alive for as long as the result is used. Nothing is copied out of the
 * firmware buffer except the human readable version string.
 */
enum wifi_iwl8265_fw_result wifi_iwl8265_fw_parse(const k_u8_t *data, k_size_t size,
                                                  struct wifi_iwl8265_fw *out);

#endif /* MICRO_OS_WIFI_IWL8265_FW_H */
