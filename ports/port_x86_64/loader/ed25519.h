/*
 * Micro OS — Ed25519, the signature of an image (ADR 0020).
 *
 * The arithmetic is the one of TweetNaCl, which is in the public domain: a
 * field of sixteen 16-bit limbs, the curve in extended coordinates and the
 * scalar reduced modulo the order of the base point. Three calls are public
 * and not one of them allocates: the loader verifies, the tool signs, and the
 * key pair of a test comes from a seed rather than from a random source, so
 * that a failing vector is reproducible.
 */
#ifndef MICRO_OS_PORT_X86_64_LOADER_ED25519_H
#define MICRO_OS_PORT_X86_64_LOADER_ED25519_H

#include <stddef.h>
#include <stdint.h>

#define MICROOS_ED25519_KEY_SIZE 32
#define MICROOS_ED25519_SECRET_SIZE 64
#define MICROOS_ED25519_SIGNATURE_SIZE 64

/*
 * The public key and the secret of a seed. The secret is the seed followed by
 * the public key, the layout of NaCl; a seed of 32 bytes is the whole secret.
 */
void microos_ed25519_keypair(uint8_t public_key[MICROOS_ED25519_KEY_SIZE],
                             uint8_t secret_key[MICROOS_ED25519_SECRET_SIZE],
                             const uint8_t seed[MICROOS_ED25519_KEY_SIZE]);

/* The signature of a message, or 0 when the secret is not usable. */
int microos_ed25519_sign(uint8_t signature[MICROOS_ED25519_SIGNATURE_SIZE], const uint8_t *message,
                         size_t len, const uint8_t secret_key[MICROOS_ED25519_SECRET_SIZE]);

/* 0 when the signature is of the message and the key, -1 otherwise. */
int microos_ed25519_verify(const uint8_t signature[MICROOS_ED25519_SIGNATURE_SIZE],
                           const uint8_t *message, size_t len,
                           const uint8_t public_key[MICROOS_ED25519_KEY_SIZE]);

#endif /* MICRO_OS_PORT_X86_64_LOADER_ED25519_H */
