/*
 * Micro OS — the trailer of a signed image (ADR 0020).
 *
 * An image is a payload followed by 120 bytes:
 *
 *   offset  size  field
 *   0       8     "MOSIGN01"
 *   8       4     version, little endian (1)
 *   12      4     algorithm, little endian (0 is SHA-256 + Ed25519)
 *   16      8     the size of the payload, little endian
 *   24      32    SHA-256 of the payload
 *   56      64    Ed25519 over bytes 0..55
 *
 * The signature covers the header and the hash, so a trailer cannot be moved
 * to another size or another algorithm, and the hash covers every byte before
 * the trailer. The same code parses the trailer in the tool and in the boot
 * loader; a second parser would be a second answer to "is this image good".
 *
 * This format is not MOX1 and not an ESP-IDF image: it is the envelope of a
 * whole binary as it travels from a build to a machine, and a program inside
 * a filesystem is covered by the signature of the volume's image, not by a
 * trailer of its own.
 */
#ifndef MICRO_OS_PORT_X86_64_LOADER_SIGN_IMAGE_H
#define MICRO_OS_PORT_X86_64_LOADER_SIGN_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define MICROOS_SIGN_MAGIC "MOSIGN01"
#define MICROOS_SIGN_MAGIC_SIZE 8
#define MICROOS_SIGN_VERSION 1
#define MICROOS_SIGN_ALGORITHM_SHA256_ED25519 0
#define MICROOS_SIGN_TRAILER_SIZE 120
#define MICROOS_SIGN_HEADER_SIZE 56
#define MICROOS_SIGN_DIGEST_SIZE 32
#define MICROOS_SIGN_KEY_SIZE 32
#define MICROOS_SIGN_SECRET_SIZE 64

/*
 * 0 when every byte of the image is the one the signer saw, -1 otherwise.
 * `why` points at a constant sentence and may be NULL; it is the word the
 * boot loader prints and the tool exits with.
 */
int microos_sign_image_verify(const void *image, size_t size,
                              const uint8_t public_key[MICROOS_SIGN_KEY_SIZE], const char **why);

/* The header of a trailer, without a signature. */
void microos_sign_trailer_make(uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE], uint64_t payload_size,
                               const uint8_t digest[MICROOS_SIGN_DIGEST_SIZE]);

/* Sign bytes 0..55 of a trailer finished by microos_sign_trailer_make(). */
int microos_sign_trailer_sign(uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE],
                              const uint8_t secret_key[MICROOS_SIGN_SECRET_SIZE]);

/* Read the header of a trailer: the size of the payload and its digest. */
int microos_sign_trailer_read(const uint8_t trailer[MICROOS_SIGN_TRAILER_SIZE],
                              uint64_t *payload_size, uint8_t digest[MICROOS_SIGN_DIGEST_SIZE]);

/*
 * The policy of the boot (ADR 0020, "The policy: a setting, not a wall").
 * A verified image always continues. An image that did not pass continues
 * only when the build does not require a signature; the caller prints the
 * warning, because the console of a boot loader and of the board are not the
 * same. The same two numbers are in the two machines and in the tests.
 */
#define MICROOS_SIGN_POLICY_ALLOW 0
#define MICROOS_SIGN_POLICY_REQUIRE 1

int microos_sign_policy_continue(int verified, int policy);

/* The sentence of a boot that continues without a verified image. */
const char *microos_sign_policy_warning(void);

#endif /* MICRO_OS_PORT_X86_64_LOADER_SIGN_IMAGE_H */
