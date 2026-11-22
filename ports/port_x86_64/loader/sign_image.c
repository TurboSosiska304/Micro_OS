/*
 * Micro OS — the parser of a signed image (ADR 0020).
 *
 * The trailer is read byte by byte: the module of the boot is page aligned
 * and the trailer is at the end of it, but nothing promises that the end is
 * aligned to eight bytes, and a load of a uint64_t from an odd address is a
 * fault on a machine that has alignment rules. A little-endian read of eight
 * bytes is also the same code on the host and on the loader.
 */

#include "sign_image.h"

#include "ed25519.h"
#include "sha256.h"

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *p, uint64_t value)
{
    write_le32(p, (uint32_t)value);
    write_le32(p + 4, (uint32_t)(value >> 32));
}

static int magic_is_there(const uint8_t *trailer)
{
    unsigned i;

    for (i = 0; i < MICROOS_SIGN_MAGIC_SIZE; i++) {
        if (trailer[i] != (uint8_t)MICROOS_SIGN_MAGIC[i]) {
            return 0;
        }
    }

    return 1;
}

static int digest_is_there(const uint8_t *a, const uint8_t *b)
{
    uint8_t difference = 0;
    unsigned i;

    for (i = 0; i < MICROOS_SIGN_DIGEST_SIZE; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }

    return difference == 0;
}

int microos_sign_trailer_read(const uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE],
                              uint64_t *payload_size, uint8_t digest[MICROOS_SIGN_DIGEST_SIZE])
{
    unsigned i;

    if (!magic_is_there(trailer)) {
        return -1;
    }
    if (read_le32(trailer + 8) != MICROOS_SIGN_VERSION) {
        return -1;
    }
    if (read_le32(trailer + 12) != MICROOS_SIGN_ALGORITHM_SHA256_ED25519) {
        return -1;
    }

    if (payload_size != NULL) {
        *payload_size = read_le64(trailer + 16);
    }
    if (digest != NULL) {
        for (i = 0; i < MICROOS_SIGN_DIGEST_SIZE; i++) {
            digest[i] = trailer[24 + i];
        }
    }

    return 0;
}

void microos_sign_trailer_make(uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE], uint64_t payload_size,
                               const uint8_t digest[MICROOS_SIGN_DIGEST_SIZE])
{
    unsigned i;

    for (i = 0; i < MICROOS_SIGN_TRAILER_SIZE; i++) {
        trailer[i] = 0;
    }
    for (i = 0; i < MICROOS_SIGN_MAGIC_SIZE; i++) {
        trailer[i] = (uint8_t)MICROOS_SIGN_MAGIC[i];
    }
    write_le32(trailer + 8, MICROOS_SIGN_VERSION);
    write_le32(trailer + 12, MICROOS_SIGN_ALGORITHM_SHA256_ED25519);
    write_le64(trailer + 16, payload_size);
    for (i = 0; i < MICROOS_SIGN_DIGEST_SIZE; i++) {
        trailer[24 + i] = digest[i];
    }
}

int microos_sign_trailer_sign(uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE],
                              const uint8_t secret_key[MICROOS_SIGN_SECRET_SIZE])
{
    return microos_ed25519_sign(trailer + MICROOS_SIGN_HEADER_SIZE, trailer,
                                MICROOS_SIGN_HEADER_SIZE, secret_key);
}

int microos_sign_image_verify(const void *image, size_t size,
                              const uint8_t public_key[MICROOS_SIGN_KEY_SIZE], const char **why)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const uint8_t *trailer;
    uint64_t payload_size;
    uint8_t digest[MICROOS_SIGN_DIGEST_SIZE];

    if (why != NULL) {
        *why = "the image has no signature";
    }
    if (size < MICROOS_SIGN_TRAILER_SIZE) {
        if (why != NULL) {
            *why = "the image is too short for a signature";
        }
        return -1;
    }

    trailer = bytes + size - MICROOS_SIGN_TRAILER_SIZE;

    if (!magic_is_there(trailer)) {
        return -1;
    }
    if (read_le32(trailer + 8) != MICROOS_SIGN_VERSION) {
        if (why != NULL) {
            *why = "the signature has a version this boot does not know";
        }
        return -1;
    }
    if (read_le32(trailer + 12) != MICROOS_SIGN_ALGORITHM_SHA256_ED25519) {
        if (why != NULL) {
            *why = "the signature names another algorithm";
        }
        return -1;
    }

    payload_size = read_le64(trailer + 16);
    if (payload_size != (uint64_t)(size - MICROOS_SIGN_TRAILER_SIZE)) {
        if (why != NULL) {
            *why = "the signature belongs to another image";
        }
        return -1;
    }

    microos_sha256(bytes, (size_t)payload_size, digest);
    if (!digest_is_there(digest, trailer + 24)) {
        if (why != NULL) {
            *why = "the checksum does not match";
        }
        return -1;
    }

    if (why != NULL) {
        *why = "the signature does not match";
    }
    if (microos_ed25519_verify(trailer + MICROOS_SIGN_HEADER_SIZE, trailer,
                               MICROOS_SIGN_HEADER_SIZE, public_key) != 0) {
        return -1;
    }

    if (why != NULL) {
        *why = NULL;
    }

    return 0;
}

int microos_sign_policy_continue(int verified, int policy)
{
    if (verified != 0) {
        return 1;
    }

    return policy == MICROOS_SIGN_POLICY_ALLOW ? 1 : 0;
}

const char *microos_sign_policy_warning(void)
{
    return "Micro OS: unsigned or damaged image: not guaranteed";
}
