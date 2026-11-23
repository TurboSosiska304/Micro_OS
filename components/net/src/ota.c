/*
 * Micro OS — the update over the network: the download, the check and the
 * slot (net/ota.h).
 *
 * The order is the promise of ADR 0022: nothing is written into the slot
 * until the signature of the image is the one of the machine. The image
 * arrives in a staging file of the volume, the digest of its payload is read
 * back from that file, and the trailer - the last hundred and twenty bytes -
 * is checked: the magic, the length, the digest and the Ed25519 signature of
 * the header. Only then does a byte go into the inactive slot.
 *
 * The staging file is removed whatever happens: a failed update leaves the
 * machine as it was, and a volume of a few megabytes does not keep a
 * megabyte of a half image.
 *
 * The clock and the key are the machine's: the key of the image of the build
 * when the build has one (the board, whose boot trusts the same key), or a
 * file of the machine in hexadecimal (x86-64 and the host tests, where the
 * key of a build is a development one).
 */
#include "net/ota.h"

#include <stdio.h>
#include <string.h>

#include "ed25519.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "net/net.h"
#include "sha256.h"

#if MICRO_OS_OTA_BUILTIN_KEY
#include "sign_public_key.h"
#endif

/* The two halves of the update do not run at once: one staging file and one
 * record of the state are the machine's. */
static bool s_busy;

/* The magic of the trailer and the algorithm of the digest (ADR 0020). */
static const char s_trailer_magic[8] = {'M', 'O', 'S', 'I', 'G', 'N', '0', '1'};

#define TRAILER_VERSION 1u
#define TRAILER_ALGORITHM_SHA256_ED25519 0u

/* The block of the copy from the staging file into the slot. */
#define OTA_BLOCK 4096u

static k_u32_t read_le32(const k_u8_t *p)
{
    return (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);
}

static k_u64_t read_le64(const k_u8_t *p)
{
    return (k_u64_t)read_le32(p) | ((k_u64_t)read_le32(p + 4) << 32);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

const char *ota_strerror(int rc)
{
    switch (rc) {
    case OTA_OK:
        return "ok";
    case OTA_INSTALLED:
        return "installed, a reboot is needed";
    case OTA_EBUSY:
        return "another update is running";
    case OTA_EFETCH:
        return "the image did not arrive";
    case OTA_ESIZE:
        return "the image does not fit the slot";
    case OTA_EFORMAT:
        return "the trailer is not one of a signed image";
    case OTA_EDIGEST:
        return "the image is not the one the trailer hashes";
    case OTA_ESIGN:
        return "the signature is not the one of this machine";
    case OTA_EIO:
        return "the storage refused a call";
    case OTA_ENOKEY:
        return "there is no key to check an image with";
    case OTA_ENOMEM:
        return "no memory for the update";
    default:
        return "the update failed";
    }
}

const char *ota_state_name(int slot_state)
{
    switch (slot_state) {
    case OTA_SLOT_VALID:
        return "valid";
    case OTA_SLOT_NEW:
        return "new";
    case OTA_SLOT_PENDING:
        return "pending";
    case OTA_SLOT_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

/* --- the staging file --- */

static const char *state_dir(const struct ota_config *cfg)
{
    return (cfg != NULL && cfg->state != NULL && cfg->state[0] != '\0') ? cfg->state
                                                                        : OTA_STATE_DEFAULT;
}

static int staging_path(const struct ota_config *cfg, char *out, k_size_t size)
{
    int n = snprintf(out, size, "%s/download.tmp", state_dir(cfg));

    return (n < 0 || (k_size_t)n >= size) ? -1 : 0;
}

/* The directory of the state: /var may not be there on a machine whose init
 * has not made it, and an update of a machine is not the place to invent a
 * tree. The caller makes it. */
static int staging_make_dir(const struct ota_config *cfg)
{
    const char *dir = state_dir(cfg);

    if (vfs_mkdir_p(dir) != 0) {
        kwarn("ota: cannot make %s", dir);
        return -1;
    }

    return 0;
}

/* The digest of the payload of a file, and its length. The read is the one
 * of the file the slot will copy: a file that changed under us is a file
 * whose digest does not match, not a race of the memory. */
static int stage_digest(const char *path, k_u64_t payload, k_u8_t digest[MICROOS_SHA256_SIZE])
{
    struct microos_sha256_ctx hash;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_u8_t block[OTA_BLOCK];
    k_u64_t left = payload;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return OTA_EIO;
    }

    microos_sha256_init(&hash);

    while (left > 0) {
        k_size_t want = left > sizeof(block) ? sizeof(block) : (k_size_t)left;
        int n = vfs_read(&file, block, want);

        if (n <= 0) {
            (void)vfs_close(&file);
            return OTA_EIO;
        }

        microos_sha256_update(&hash, block, (size_t)n);
        left -= (k_u64_t)n;
    }

    (void)vfs_close(&file);

    microos_sha256_final(&hash, digest);

    return OTA_OK;
}

/* The trailer of the staged file: the last hundred and twenty bytes. */
static int stage_trailer(const char *path, k_u64_t size, k_u8_t trailer[OTA_TRAILER_SIZE])
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    int rc;

    if (size < OTA_TRAILER_SIZE) {
        return OTA_ESIZE;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return OTA_EIO;
    }

    file.offset = size - OTA_TRAILER_SIZE;

    rc = vfs_read(&file, trailer, OTA_TRAILER_SIZE);
    (void)vfs_close(&file);

    return rc == (int)OTA_TRAILER_SIZE ? OTA_OK : OTA_EIO;
}

/*
 * The trailer and the bytes: the digest of the payload from the trailer is
 * compared with the digest of the file. The signature of the trailer covers
 * the header and the digest, so a trailer moved to another payload, another
 * length or another algorithm is a signature that does not check.
 */
static int stage_check(const char *path, k_u64_t size, const k_u8_t *key)
{
    k_u8_t trailer[OTA_TRAILER_SIZE];
    k_u8_t digest[OTA_TRAILER_DIGEST];
    k_u64_t payload;
    int rc;

    rc = stage_trailer(path, size, trailer);
    if (rc != OTA_OK) {
        return rc;
    }

    payload = read_le64(trailer + 16);

    if (payload + OTA_TRAILER_SIZE != size) {
        return OTA_EFORMAT;
    }

    if (memcmp(trailer, s_trailer_magic, sizeof(s_trailer_magic)) != 0 ||
        read_le32(trailer + 8) != TRAILER_VERSION ||
        read_le32(trailer + 12) != TRAILER_ALGORITHM_SHA256_ED25519) {
        return OTA_EFORMAT;
    }

    rc = stage_digest(path, payload, digest);
    if (rc != OTA_OK) {
        return rc;
    }

    if (memcmp(digest, trailer + 24, sizeof(digest)) != 0) {
        return OTA_EDIGEST;
    }

    if (microos_ed25519_verify(trailer + OTA_TRAILER_HEADER, trailer, OTA_TRAILER_HEADER, key) !=
        0) {
        return OTA_ESIGN;
    }

    return OTA_OK;
}

/* --- the key of the machine --- */

static int key_from_file(const char *path, k_u8_t key[OTA_KEY_SIZE])
{
    char text[OTA_KEY_HEX + 8];
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_size_t i;
    int n;

    if (vfs_open(path, VFS_O_RDONLY, &file, priv) != 0) {
        return OTA_ENOKEY;
    }

    n = vfs_read(&file, text, sizeof(text) - 1);
    (void)vfs_close(&file);

    if (n < (int)OTA_KEY_HEX - 1) {
        return OTA_ENOKEY;
    }

    text[n] = '\0';

    for (i = 0; i < OTA_KEY_SIZE; i++) {
        int high = hex_digit(text[i * 2u]);
        int low = hex_digit(text[i * 2u + 1u]);

        if (high < 0 || low < 0) {
            return OTA_ENOKEY;
        }

        key[i] = (k_u8_t)((high << 4) | low);
    }

    return OTA_OK;
}

static int ota_key(const struct ota_config *cfg, k_u8_t key[OTA_KEY_SIZE])
{
    if (cfg != NULL && cfg->key != NULL) {
        memcpy(key, cfg->key, OTA_KEY_SIZE);
        return OTA_OK;
    }

    if (cfg != NULL && cfg->key_path != NULL && cfg->key_path[0] != '\0') {
        return key_from_file(cfg->key_path, key);
    }

#if MICRO_OS_OTA_BUILTIN_KEY
    memcpy(key, loader_public_key, OTA_KEY_SIZE);
    return OTA_OK;
#else
    return OTA_ENOKEY;
#endif
}

/* --- the install --- */

/* The payload of the staged file into the slot, one block at a time. */
static int stage_copy(const char *path, k_u64_t payload, int slot)
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_u8_t block[OTA_BLOCK];
    k_u64_t offset = 0;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return OTA_EIO;
    }

    while (offset < payload) {
        k_size_t want =
            payload - offset > sizeof(block) ? sizeof(block) : (k_size_t)(payload - offset);
        int n = vfs_read(&file, block, want);

        if (n <= 0) {
            (void)vfs_close(&file);
            return OTA_EIO;
        }

        if (ota_slot_write(slot, offset, block, (k_size_t)n) != 0) {
            (void)vfs_close(&file);
            return OTA_EIO;
        }

        offset += (k_u64_t)n;
    }

    (void)vfs_close(&file);

    return OTA_OK;
}

int ota_install(const struct ota_config *cfg, struct ota_state *out)
{
    struct ota_state state;
    char staging[VFS_PATH_MAX];
    struct http_stream stream;
    struct vfs_stat info;
    k_u8_t key[OTA_KEY_SIZE];
    k_u64_t payload;
    k_u8_t trailer[OTA_TRAILER_SIZE];
    k_size_t limit;
    int target;
    int rc;

    if (cfg == NULL || cfg->url == NULL) {
        return OTA_EFETCH;
    }

    if (__atomic_exchange_n(&s_busy, true, __ATOMIC_SEQ_CST)) {
        return OTA_EBUSY;
    }

    if (staging_make_dir(cfg) != 0) {
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_EIO;
    }

    if (staging_path(cfg, staging, sizeof(staging)) != 0) {
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_EIO;
    }

    rc = ota_key(cfg, key);
    if (rc != OTA_OK) {
        kwarn("ota: %s", ota_strerror(rc));
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return rc;
    }

    limit = cfg->max_image != 0 ? cfg->max_image : OTA_IMAGE_MAX;

    /* A staging file that an earlier run left is not a file this download
     * wrote: it is removed before the first byte, so its length is the
     * length of the body and not the longer one of a dead attempt. */
    (void)vfs_unlink(staging);

    /* The image arrives in one file. The budget of the exchange is the
     * client's, and a server that promises more than the slot is refused
     * before a byte of the slot is touched. */
    rc = net_http_download_to(cfg->transport, cfg->url, staging, false, &stream);

    if (rc != HTTP_OK) {
        kwarn("ota: %s: %s", cfg->url, "the download failed");
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_EFETCH;
    }

    if (stream.total > limit) {
        kwarn("ota: the image is larger than the slot (%llu byte(s))",
              (unsigned long long)stream.total);
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_ESIZE;
    }

    if (vfs_stat(staging, &info) != 0 || info.type != 1) {
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_EIO;
    }

    if (info.size > limit) {
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return OTA_ESIZE;
    }

    /* The check before the slot: the signature, the length and the digest of
     * the bytes of the file the slot will copy. */
    rc = stage_check(staging, info.size, key);

    if (rc != OTA_OK) {
        kwarn("ota: the image of %s is refused: %s", cfg->url, ota_strerror(rc));
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return rc;
    }

    rc = ota_status(&state);
    if (rc != OTA_OK) {
        (void)vfs_unlink(staging);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return rc;
    }

    target = state.active == 0 ? 1 : 0;

    rc = stage_trailer(staging, info.size, trailer);
    payload = rc == OTA_OK ? read_le64(trailer + 16) : 0;

    if (rc == OTA_OK) {
        rc = ota_slot_begin(target);
    }
    if (rc == OTA_OK) {
        rc = stage_copy(staging, payload, target);
    }
    if (rc == OTA_OK) {
        rc = ota_slot_end(target, trailer);
    }
    if (rc == OTA_OK) {
        rc = ota_slot_select(target);
    }

    (void)vfs_unlink(staging);

    if (rc != OTA_OK) {
        kwarn("ota: the slot %d did not take the image: %s", target, ota_strerror(rc));
        (void)ota_slot_invalidate(target);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        return rc;
    }

    if (out != NULL) {
        if (ota_status(out) != OTA_OK) {
            memset(out, 0, sizeof(*out));
        }
    }

    __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);

    return OTA_INSTALLED;
}
