/*
 * Micro OS — firmware-file discovery and skip semantics for Intel 8265.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/cfg/8000.c
 * (GPL-2.0, Linux v6.12); code written here (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/vfs.h"
#include "mfs/mfs_vfs.h"
#include "ramfs/ramfs_vfs.h"
#include "wifi/iwl8265_fw_file.h"

#ifndef WIFI_TEST_ROOT
#define WIFI_TEST_ROOT ""
#endif

static void test_missing_firmware(void)
{
    struct wifi_iwl8265_fw_file file;

    ktest_begin("wifi: no firmware file is a skip, not a driver failure");
    memset(&file, 0, sizeof(file));

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_file_load(&file), WIFI_IWL8265_FW_FILE_NONE);
    KT_CHECK(file.data == NULL);
    KT_CHECK_INT_EQ(file.size, 0u);
    KT_CHECK(file.path[0] == '\0');

    KT_CHECK_INT_EQ(wifi_iwl8265_fw_file_load(NULL), WIFI_IWL8265_FW_FILE_INVALID);
}

static void test_reads_large_firmware(void)
{
    struct wifi_iwl8265_fw_file file;
    struct vfs_file writer;
    struct vfs_stat info;
    k_u8_t private_data[VFS_FILE_DATA_SIZE];
    k_size_t size = 70u * 1024u;
    k_u8_t *bytes = kmm_alloc(size, KMM_PRIORITY_KERNEL);
    k_size_t done = 0u;
    int rc;

    ktest_begin("wifi: a firmware larger than one syscall block is read whole");
    KT_CHECK(bytes != NULL);
    if (bytes == NULL) {
        return;
    }
    memset(bytes, 0xA5, size);

    rc = vfs_mkdir_p(WIFI_TEST_ROOT "/etc/firmware");
    KT_CHECK_INT_EQ(rc, 0);
    rc = vfs_open(WIFI_TEST_ROOT "/etc/firmware/iwlwifi-8265-36.ucode",
                  VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &writer, private_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        while (done < size) {
            rc = vfs_write(&writer, bytes + done, size - done);
            if (rc <= 0) {
                break;
            }
            done += (k_size_t)rc;
        }
        KT_CHECK_INT_EQ(done, size);
        (void)vfs_close(&writer);
    }

    memset(&file, 0, sizeof(file));
    KT_CHECK_INT_EQ(wifi_iwl8265_fw_file_load(&file), WIFI_IWL8265_FW_FILE_OK);
    KT_CHECK(file.data != NULL);
    KT_CHECK_INT_EQ(file.size, size);
    KT_CHECK_STR_EQ(file.path, WIFI_TEST_ROOT "/etc/firmware/iwlwifi-8265-36.ucode");
    if (file.data != NULL) {
        KT_CHECK_INT_EQ(file.data[0], 0xA5);
        KT_CHECK_INT_EQ(file.data[size - 1u], 0xA5);
        KT_CHECK_INT_EQ(vfs_stat(file.path, &info), 0);
        KT_CHECK_INT_EQ(info.size, size);
        kmm_free(file.data);
    }
    kmm_free(bytes);
}

void test_wifi_8265_fw_file(void)
{
    int rc;

    (void)vfs_umount("/");
    (void)ramfs_vfs_reset();
    rc = ramfs_vfs_register_type();
    if (rc != 0 && rc != KERR_EXIST) {
        KT_CHECK_INT_EQ(rc, 0);
    }
    rc = vfs_mount_type("ramfs", "/", "ram0");
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        (void)vfs_unregister_type("ramfs");
        return;
    }

    test_missing_firmware();
    test_reads_large_firmware();

    (void)vfs_umount("/");
    (void)vfs_unregister_type("ramfs");
}
