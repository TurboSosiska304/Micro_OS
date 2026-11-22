/*
 * Micro OS — the signature of the application at the boot of the board
 * (ADR 0020, "The policy: a setting, not a wall").
 *
 * The ROM and the ESP-IDF bootloader have already loaded this image when
 * app_main runs, so the check here cannot stop the first instructions of a
 * damaged image from having run. It can still tell a person, before the
 * filesystem and the shell, that the bytes of the application and the trailer
 * at the end of its partition do not agree, and it can refuse to continue or
 * continue with a warning according to CONFIG_MICRO_OS_REQUIRE_SIGNATURE.
 *
 * The trailer is not after the payload on the flash: the application is at
 * the start of the factory partition and the trailer is in the last 4 KiB of
 * that partition. The signature covers the bytes of the app image up to the
 * length in its own ESP header, so the check hashes the flash in chunks and
 * never copies a megabyte into RAM.
 */

#include "port_esp32.h"

#include <stdint.h>

#include "ed25519.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "kernel/log.h"
#include "kernel/version.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sha256.h"
#include "sign_image.h"
#include "sign_public_key.h"

/* The last 4 KiB of the factory partition are kept for the trailer; the app
   image, its header and its own hash have to end before them. */
#define IMAGE_TRAILER_RESERVE 0x1000u
#define IMAGE_DESC_OFFSET 0x20u
#define IMAGE_CHUNK 512u

/* The NVS namespace and key of the anti-rollback version. The namespace is
   separate from the configuration: this is a fact of the boot, not /etc. */
#define IMAGE_NVS_NAMESPACE "microos_boot"
#define IMAGE_NVS_MIN_VERSION "min_version"

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t align16(size_t value)
{
    return (value + 15u) & ~(size_t)15u;
}

/*
 * The length of the ESP image, from its own header: 24 bytes, the segment
 * headers and their data, the checksum padded to 16 bytes, and the SHA-256
 * that elf2image appends. It is the length of micro_os.bin, which is what
 * tools/bundle.sh signs.
 */
static int image_length(const esp_partition_t *part, size_t *length)
{
    uint8_t header[24];
    size_t offset = sizeof(header);
    unsigned segments;
    unsigned i;

    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK) {
        return -1;
    }
    if (header[0] != 0xE9u) {
        return -1;
    }

    segments = header[1];
    if (segments > 16u) {
        return -1;
    }

    for (i = 0; i < segments; i++) {
        uint8_t segment[8];
        uint32_t data_len;

        if (offset > part->size || part->size - offset < sizeof(segment) ||
            esp_partition_read(part, offset, segment, sizeof(segment)) != ESP_OK) {
            return -1;
        }

        data_len = read_le32(segment + 4);
        offset += sizeof(segment);
        if (data_len > part->size - offset) {
            return -1;
        }
        offset += data_len;
    }

    /* One byte of checksum, padded to the next 16-byte boundary, then the
       optional SHA-256 of the whole image (hash_appended at offset 23). */
    offset = align16(offset + 1u);
    if (header[23] != 0u) {
        if (offset > part->size || part->size - offset < 32u) {
            return -1;
        }
        offset += 32u;
    }

    *length = offset;

    return 0;
}

/* The version of the application, from the esp_app_desc_t at 0x20. */
static int image_version(const esp_partition_t *part, char version[32])
{
    esp_app_desc_t desc;
    unsigned i;

    if (esp_partition_read(part, IMAGE_DESC_OFFSET, &desc, sizeof(desc)) != ESP_OK) {
        return -1;
    }
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return -1;
    }

    for (i = 0; i < sizeof(desc.version) - 1u && desc.version[i] != '\0'; i++) {
        version[i] = desc.version[i];
    }
    version[i] = '\0';

    return 0;
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

/* The answer of a boot that could not verify the image. */
static int unverified(const char *why)
{
#if CONFIG_MICRO_OS_REQUIRE_SIGNATURE
    kerror("boot: the image is refused: %s", why);
    return -1;
#else
    kwarn("boot: unsigned or damaged image: not guaranteed (%s)", why);
    return 0;
#endif
}

/*
 * The anti-rollback of the NVS: the newest version a boot has seen stays
 * there, and a lower one is refused or warned about. A store that cannot be
 * read or written is named on the console; it does not take a working board
 * down, because the signature above is the boundary of the image and a
 * damaged NVS is not proof of a rollback.
 */
static int anti_rollback(const char *version)
{
    nvs_handle_t handle;
    char stored[32];
    size_t length = sizeof(stored);
    esp_err_t rc;
    int found = 0;

    rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        kwarn("boot: anti-rollback: the NVS needs a format, the version is not stored");
        return 0;
    }
    if (rc != ESP_OK) {
        kwarn("boot: anti-rollback: the NVS is not available (rc=%d)", (int)rc);
        return 0;
    }

    rc = nvs_open(IMAGE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        kwarn("boot: anti-rollback: the store is not open (rc=%d)", (int)rc);
        return 0;
    }

    rc = nvs_get_str(handle, IMAGE_NVS_MIN_VERSION, stored, &length);
    if (rc == ESP_OK) {
        found = 1;
    } else if (rc != ESP_ERR_NVS_NOT_FOUND) {
        kwarn("boot: anti-rollback: the stored version is not readable (rc=%d)", (int)rc);
    }

    if (found) {
        int order = kernel_version_compare(version, stored);

        if (order < 0) {
#if CONFIG_MICRO_OS_ANTIROLLBACK
            kerror("boot: the image is refused: version %s is older than the stored %s", version,
                   stored);
            nvs_close(handle);
            return -1;
#else
            kwarn("boot: rollback from %s to %s is allowed: not guaranteed", stored, version);
#endif
        } else if (order > 0) {
            rc = nvs_set_str(handle, IMAGE_NVS_MIN_VERSION, version);
            if (rc == ESP_OK) {
                rc = nvs_commit(handle);
            }
            if (rc == ESP_OK) {
                kinfo("boot: anti-rollback: version %s recorded (was %s)", version, stored);
            } else {
                kwarn("boot: anti-rollback: version %s is not stored (rc=%d)", version, (int)rc);
            }
        }
    } else {
        rc = nvs_set_str(handle, IMAGE_NVS_MIN_VERSION, version);
        if (rc == ESP_OK) {
            rc = nvs_commit(handle);
        }
        if (rc == ESP_OK) {
            kinfo("boot: anti-rollback: version %s recorded", version);
        } else {
            kwarn("boot: anti-rollback: version %s is not stored (rc=%d)", version, (int)rc);
        }
    }

    nvs_close(handle);

    return 0;
}

int port_esp32_image_check(void)
{
    const esp_partition_t *part;
    struct microos_sha256_ctx ctx;
    uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE];
    uint8_t expected[MICROOS_SIGN_DIGEST_SIZE];
    uint8_t digest[MICROOS_SIGN_DIGEST_SIZE];
    uint64_t payload_size = 0;
    char version[32];
    size_t image_size = 0;
    size_t offset;
    int has_version;
    static uint8_t chunk[IMAGE_CHUNK];

    part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (part == NULL) {
        return unverified("there is no factory partition");
    }
    if (part->size <= IMAGE_TRAILER_RESERVE) {
        return unverified("the factory partition is too small for its trailer");
    }
    if (image_length(part, &image_size) != 0) {
        return unverified("the application header is not readable");
    }

    offset = part->size - IMAGE_TRAILER_RESERVE;
    if (esp_partition_read(part, offset, trailer, sizeof(trailer)) != ESP_OK) {
        return unverified("the trailer of the partition is not readable");
    }
    if (microos_sign_trailer_read(trailer, &payload_size, expected) != 0) {
        return unverified("the image has no signature");
    }
    if (payload_size > (uint64_t)offset) {
        return unverified("the signature names a size outside the partition");
    }
    if ((size_t)payload_size != image_size) {
        return unverified("the signature does not cover the application image");
    }

    microos_sha256_init(&ctx);
    for (offset = 0; offset < (size_t)payload_size;) {
        size_t want = (size_t)payload_size - offset;

        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        if (esp_partition_read(part, offset, chunk, want) != ESP_OK) {
            return unverified("the application is not readable");
        }
        microos_sha256_update(&ctx, chunk, want);
        offset += want;
    }
    microos_sha256_final(&ctx, digest);

    if (!digest_is_there(digest, expected)) {
        return unverified("the checksum does not match");
    }
    if (microos_ed25519_verify(trailer + MICROOS_SIGN_HEADER_SIZE, trailer,
                               MICROOS_SIGN_HEADER_SIZE, loader_public_key) != 0) {
        return unverified("the signature does not match");
    }

    kinfo("boot: image signature ok (sha-256, ed25519)");

    has_version = image_version(part, version) == 0;
    if (!has_version) {
        kwarn("boot: anti-rollback: the image has no version");
        return 0;
    }

    return anti_rollback(version);
}
