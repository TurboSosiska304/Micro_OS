/*
 * Micro OS — locate and read an Intel 8265 firmware file.
 *
 * The ucode remains a data file under LICENCE.iwlwifi_firmware. This layer
 * only reads it through VFS; it never creates, changes or embeds the blob.
 */
#include "wifi/iwl8265_fw_file.h"

#include "wifi/iwl8265_fw.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/kfile.h"
#include "kernel/kmm.h"
#include "kernel/task.h"
#include "kernel/vfs.h"

static void copy_path(char *out, k_size_t size, const char *text)
{
    k_size_t length = strlen(text);

    if (length >= size) {
        length = size - 1u;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static enum wifi_iwl8265_fw_file_result read_candidate(const char *directory_path, const char *name,
                                                       k_u64_t size,
                                                       struct wifi_iwl8265_fw_file *out)
{
    struct kfile *file;
    struct task *owner = task_self();
    k_size_t done = 0u;
    char path[WIFI_IWL8265_FW_PATH_MAX];
    int fd;
    int rc;
    int length = snprintf(path, sizeof(path), "%s/%s", directory_path, name);

    if (length < 0 || (k_size_t)length >= sizeof(path)) {
        return WIFI_IWL8265_FW_FILE_INVALID;
    }
    if (size == 0u || size > WIFI_IWL8265_FW_MAX_BYTES) {
        return WIFI_IWL8265_FW_FILE_TOO_BIG;
    }

    out->data = kmm_alloc((k_size_t)size, KMM_PRIORITY_KERNEL);
    if (out->data == NULL) {
        return WIFI_IWL8265_FW_FILE_NOMEM;
    }

    /* A kernel task owns the descriptor for the duration of the read. The
     * firmware is larger than KSYS_MAX_IO, so kfile_read is used directly. */
    fd = kfile_alloc(owner, &file);
    if (fd < 0) {
        kmm_free(out->data);
        out->data = NULL;
        return WIFI_IWL8265_FW_FILE_NOMEM;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file->file, file->priv);
    if (rc != 0) {
        (void)kfile_close(owner, fd);
        kmm_free(out->data);
        out->data = NULL;
        return WIFI_IWL8265_FW_FILE_IO;
    }
    file->flags = VFS_O_RDONLY;
    copy_path(file->path, sizeof(file->path), path);

    while (done < (k_size_t)size) {
        k_ssize_t got = kfile_read(file, out->data + done, (k_size_t)size - done);

        if (got <= 0) {
            break;
        }
        done += (k_size_t)got;
    }

    (void)kfile_close(owner, fd);
    if (done != (k_size_t)size) {
        kmm_free(out->data);
        out->data = NULL;
        return WIFI_IWL8265_FW_FILE_IO;
    }

    out->size = done;
    copy_path(out->path, sizeof(out->path), path);
    return WIFI_IWL8265_FW_FILE_OK;
}

static enum wifi_iwl8265_fw_file_result scan_directory(const char *path, k_u32_t api,
                                                       struct wifi_iwl8265_fw_file *out)
{
    struct kfile *directory;
    struct task *owner = task_self();
    struct vfs_dirent entries[8];
    k_size_t count = 0u;
    k_size_t i;
    int directory_fd;
    int rc;

    directory_fd = kfile_alloc(owner, &directory);
    if (directory_fd < 0) {
        return WIFI_IWL8265_FW_FILE_NOMEM;
    }
    rc = vfs_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, &directory->file, directory->priv);
    if (rc != 0) {
        (void)kfile_close(owner, directory_fd);
        return rc == KERR_NOENT ? WIFI_IWL8265_FW_FILE_NONE : WIFI_IWL8265_FW_FILE_IO;
    }
    directory->flags = VFS_O_RDONLY | VFS_O_DIRECTORY;
    copy_path(directory->path, sizeof(directory->path), path);

    for (;;) {
        char expected[40];
        int length = snprintf(expected, sizeof(expected), "iwlwifi-8265-%u.ucode", (unsigned)api);

        if (length < 0 || (k_size_t)length >= sizeof(expected)) {
            (void)kfile_close(owner, directory_fd);
            return WIFI_IWL8265_FW_FILE_INVALID;
        }

        count = 0u;
        rc = vfs_readdir(&directory->file, entries, 8u, &count);
        if (rc != 0) {
            (void)kfile_close(owner, directory_fd);
            return WIFI_IWL8265_FW_FILE_IO;
        }
        for (i = 0u; i < count; i++) {
            enum wifi_iwl8265_fw_file_result result;

            if (strcmp(entries[i].name, expected) != 0) {
                continue;
            }
            (void)kfile_close(owner, directory_fd);
            result = read_candidate(path, entries[i].name, entries[i].size, out);
            return result == WIFI_IWL8265_FW_FILE_NONE ? WIFI_IWL8265_FW_FILE_IO : result;
        }
        if (count < 8u) {
            break;
        }
    }

    (void)kfile_close(owner, directory_fd);
    return WIFI_IWL8265_FW_FILE_NONE;
}

enum wifi_iwl8265_fw_file_result wifi_iwl8265_fw_file_load(struct wifi_iwl8265_fw_file *out)
{
    static const char *const directories[] = {"/etc/firmware", "/lib/firmware", "/firmware"};
    k_u32_t api;

    if (out == NULL) {
        return WIFI_IWL8265_FW_FILE_INVALID;
    }

    memset(out, 0, sizeof(*out));

    /* The newest API first is the order used by the reference driver. */
    for (api = WIFI_IWL8265_API_MAX; api >= WIFI_IWL8265_API_MIN; api--) {
        k_size_t d;

        for (d = 0u; d < sizeof(directories) / sizeof(directories[0]); d++) {
            enum wifi_iwl8265_fw_file_result result = scan_directory(directories[d], api, out);

            if (result != WIFI_IWL8265_FW_FILE_NONE) {
                return result;
            }
        }
    }

    return WIFI_IWL8265_FW_FILE_NONE;
}
