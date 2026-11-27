/*
 * Micro OS — locate and read an Intel 8265 firmware file.
 *
 * Firmware is a redistributable data file under Intel's own licence. It is
 * not source, is not embedded in the kernel, and is never modified here.
 * This layer only enumerates bounded paths and reads the file through VFS.
 */
#ifndef MICRO_OS_WIFI_IWL8265_FW_FILE_H
#define MICRO_OS_WIFI_IWL8265_FW_FILE_H

#include "kernel/types.h"

#define WIFI_IWL8265_FW_PATH_MAX 96u
#define WIFI_IWL8265_FW_MAX_BYTES (4u * 1024u * 1024u)

struct wifi_iwl8265_fw_file {
    k_u8_t *data;
    k_size_t size;
    char path[WIFI_IWL8265_FW_PATH_MAX];
};

enum wifi_iwl8265_fw_file_result {
    WIFI_IWL8265_FW_FILE_OK = 0,
    WIFI_IWL8265_FW_FILE_NONE,
    WIFI_IWL8265_FW_FILE_INVALID,
    WIFI_IWL8265_FW_FILE_IO,
    WIFI_IWL8265_FW_FILE_NOMEM,
    WIFI_IWL8265_FW_FILE_TOO_BIG,
};

/*
 * Find the highest supported API (36 down to 22) in /etc/firmware,
 * /lib/firmware and /firmware. A missing file is NONE, not a driver failure.
 * On OK the caller owns `data` and releases it with kmm_free().
 */
enum wifi_iwl8265_fw_file_result wifi_iwl8265_fw_file_load(struct wifi_iwl8265_fw_file *out);

#endif /* MICRO_OS_WIFI_IWL8265_FW_FILE_H */
