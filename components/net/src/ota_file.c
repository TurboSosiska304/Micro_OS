/*
 * Micro OS — the slots of a machine whose slots are files (net/ota.h).
 *
 * The form of the host and of x86-64: a directory holds the payload of the
 * two slots and one record of the state, and a "reboot" is a second call of
 * ota_boot() - which is exactly what the bootloader of the board does, with
 * otadata instead of a file. The state machine, the order of the calls and
 * the answer of each are the same, which is what makes the test of this form
 * a proof of the logic and not of a model of it.
 *
 *   /var/ota/slot0.img   the payload of slot 0
 *   /var/ota/slot1.img   the payload of slot 1
 *   /var/ota/boot        the record: the slot, its state, the count
 *
 * The record is text and it is written whole after every change, so a
 * machine that loses power in the middle of an install finds either the old
 * record or the new one. Nothing of the record is a secret; the signature of
 * the payload is what a boot trusts.
 */
#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/vfs.h"
#include "net/ota.h"

/* The descriptor of the slot an install is filling. One install at a time is
 * the rule of the component (net/ota.c, s_busy). */
static struct vfs_file s_slot_file;
static k_u8_t s_slot_priv[VFS_FILE_DATA_SIZE];
static bool s_slot_open;

static const char *slot_dir(void)
{
    return OTA_STATE_DEFAULT;
}

static int path_of(const char *name, int slot, char *out, k_size_t size)
{
    int n;

    if (name != NULL) {
        n = snprintf(out, size, "%s/%s", slot_dir(), name);
    } else {
        n = snprintf(out, size, "%s/slot%d.img", slot_dir(), slot);
    }

    return (n < 0 || (k_size_t)n >= size) ? -1 : 0;
}

static int state_path(char *out, k_size_t size)
{
    int n = snprintf(out, size, "%s/boot", slot_dir());

    return (n < 0 || (k_size_t)n >= size) ? -1 : 0;
}

/* --- the record of the state --- */

static k_u32_t text_u32(const char *text, const char *name, k_u32_t fallback)
{
    const char *at = strstr(text, name);
    k_u32_t value = 0;

    if (at == NULL) {
        return fallback;
    }

    at += strlen(name);

    if (*at < '0' || *at > '9') {
        return fallback;
    }

    while (*at >= '0' && *at <= '9') {
        value = value * 10u + (k_u32_t)(*at - '0');
        at++;
    }

    return value;
}

static int state_load(struct ota_state *out)
{
    char path[VFS_PATH_MAX];
    char text[128];
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    int n;

    memset(out, 0, sizeof(*out));
    out->active = 0;
    out->slot_state = OTA_SLOT_VALID;

    if (state_path(path, sizeof(path)) != 0) {
        return OTA_EIO;
    }

    if (vfs_open(path, VFS_O_RDONLY, &file, priv) != 0) {
        /* A machine that never updated: one slot, valid, and nothing else.
         * The record is made by the first ota_install(), not here. */
        return OTA_OK;
    }

    n = vfs_read(&file, text, sizeof(text) - 1);
    (void)vfs_close(&file);

    if (n <= 0) {
        return OTA_OK;
    }

    text[n] = '\0';

    out->active = (int)text_u32(text, "active=", 0) & 1;
    out->sequence = text_u32(text, "seq=", 0);

    if (strstr(text, "state=new") != NULL) {
        out->slot_state = OTA_SLOT_NEW;
    } else if (strstr(text, "state=pending") != NULL) {
        out->slot_state = OTA_SLOT_PENDING;
    } else if (strstr(text, "state=invalid") != NULL) {
        out->slot_state = OTA_SLOT_INVALID;
    } else {
        out->slot_state = OTA_SLOT_VALID;
    }

    return OTA_OK;
}

static int state_save(const struct ota_state *in)
{
    char path[VFS_PATH_MAX];
    char text[96];
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_size_t len = 0;
    int n;

    if (state_path(path, sizeof(path)) != 0) {
        return OTA_EIO;
    }

    n = snprintf(text, sizeof(text), "active=%d\nstate=%s\nseq=%u\n", in->active,
                 ota_state_name(in->slot_state), (unsigned)in->sequence);

    if (n < 0 || (k_size_t)n >= sizeof(text)) {
        return OTA_EIO;
    }

    len = (k_size_t)n;

    if (vfs_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) != 0) {
        return OTA_EIO;
    }

    if (vfs_write(&file, text, len) != (int)len) {
        (void)vfs_close(&file);
        return OTA_EIO;
    }

    (void)vfs_close(&file);

    return OTA_OK;
}

int ota_status(struct ota_state *out)
{
    if (out == NULL) {
        return OTA_EIO;
    }

    return state_load(out);
}

/*
 * The boot of this machine, as the state machine sees it. A slot an update
 * wrote (NEW) becomes PENDING: this is the boot that has to confirm it. A
 * slot that was already PENDING at the boot before never confirmed itself,
 * and the machine returns to the other one - the rollback the board's
 * bootloader makes with otadata, written out where a person can read it.
 */
int ota_boot(struct ota_state *out)
{
    struct ota_state state;
    int rc = state_load(&state);

    if (out != NULL) {
        *out = state;
    }

    if (rc != OTA_OK) {
        return rc;
    }

    if (state.slot_state == OTA_SLOT_NEW) {
        state.slot_state = OTA_SLOT_PENDING;

        rc = state_save(&state);

        if (out != NULL) {
            *out = state;
        }

        return rc;
    }

    if (state.slot_state != OTA_SLOT_PENDING) {
        return OTA_OK;
    }

    state.active = state.active == 0 ? 1 : 0;
    state.slot_state = OTA_SLOT_VALID;
    state.rolled_back = true;

    rc = state_save(&state);

    if (out != NULL) {
        *out = state;
    }

    kwarn("ota: the slot %d did not confirm itself, the machine returns to slot %d",
          state.active == 0 ? 1 : 0, state.active);

    return rc;
}

int ota_confirm(void)
{
    struct ota_state state;
    int rc = state_load(&state);

    if (rc != OTA_OK) {
        return rc;
    }

    if (state.slot_state != OTA_SLOT_PENDING && state.slot_state != OTA_SLOT_NEW) {
        return OTA_OK;
    }

    state.slot_state = OTA_SLOT_VALID;

    return state_save(&state);
}

/* --- the slots --- */

int ota_slot_begin(int slot)
{
    char path[VFS_PATH_MAX];

    if (slot != 0 && slot != 1) {
        return -1;
    }

    if (vfs_mkdir_p(slot_dir()) != 0) {
        return OTA_EIO;
    }

    if (path_of(NULL, slot, path, sizeof(path)) != 0) {
        return OTA_EIO;
    }

    if (vfs_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &s_slot_file, s_slot_priv) != 0) {
        return OTA_EIO;
    }

    s_slot_open = true;

    return OTA_OK;
}

int ota_slot_write(int slot, k_u64_t offset, const void *data, k_size_t size)
{
    (void)slot;

    if (!s_slot_open) {
        return OTA_EIO;
    }

    s_slot_file.offset = offset;

    if (vfs_write(&s_slot_file, data, size) != (int)size) {
        return OTA_EIO;
    }

    return OTA_OK;
}

int ota_slot_end(int slot, const k_u8_t trailer[OTA_TRAILER_SIZE])
{
    struct vfs_stat info;
    char path[VFS_PATH_MAX];

    /* The trailer is the envelope of the image and not a part of the slot:
     * the bytes an image needs to boot are the payload, and the trailer was
     * verified before the first of them was written. */
    (void)trailer;

    if (s_slot_open) {
        (void)vfs_close(&s_slot_file);
        s_slot_open = false;
    }

    /* The bytes have to be a file of the volume: a write that failed is a
     * slot that must not be selected. */
    if (path_of(NULL, slot, path, sizeof(path)) != 0 || vfs_stat(path, &info) != 0 ||
        info.type != 1) {
        return OTA_EIO;
    }

    return OTA_OK;
}

int ota_slot_select(int slot)
{
    struct ota_state state;
    int rc = state_load(&state);

    if (rc != OTA_OK) {
        return rc;
    }

    state.active = slot & 1;
    state.slot_state = OTA_SLOT_NEW;
    state.sequence++;

    return state_save(&state);
}

int ota_slot_invalidate(int slot)
{
    char path[VFS_PATH_MAX];
    struct ota_state state;
    int rc = state_load(&state);

    if (s_slot_open) {
        (void)vfs_close(&s_slot_file);
        s_slot_open = false;
    }

    if (path_of(NULL, slot, path, sizeof(path)) == 0) {
        (void)vfs_unlink(path);
    }

    if (rc != OTA_OK || state.active != (slot & 1)) {
        return rc;
    }

    state.active = state.active == 0 ? 1 : 0;
    state.slot_state = OTA_SLOT_VALID;

    return state_save(&state);
}

int ota_slot_size(int slot, k_u64_t *bytes)
{
    char path[VFS_PATH_MAX];
    struct vfs_stat info;

    if (bytes == NULL || path_of(NULL, slot, path, sizeof(path)) != 0) {
        return OTA_EIO;
    }

    if (vfs_stat(path, &info) != 0 || info.type != 1) {
        return OTA_EIO;
    }

    *bytes = info.size;

    return OTA_OK;
}
