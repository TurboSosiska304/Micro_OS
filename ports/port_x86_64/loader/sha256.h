/*
 * Micro OS — SHA-256, the hash of a signed image (ADR 0020).
 *
 * The same file is compiled into the host tool, the 32-bit loader and the
 * board: it has no libc in it and no allocation, so the verifier of the boot
 * and the signer of the build agree by sharing code rather than by sharing a
 * test vector.
 *
 * The one-shot call covers a buffer that is already in memory (the loader and
 * the tool). The context covers an image that is only on the flash, one chunk
 * at a time (the board): the digest is the same and the two paths meet in the
 * tests.
 */
#ifndef MICRO_OS_PORT_X86_64_LOADER_SHA256_H
#define MICRO_OS_PORT_X86_64_LOADER_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define MICROOS_SHA256_SIZE 32

/* The state of a digest that grows one buffer at a time. */
struct microos_sha256_ctx {
    uint32_t state[8];
    uint64_t total;
    uint8_t block[64];
    size_t used;
};

/* The digest of a whole buffer. A NULL buffer is allowed when len is 0. */
void microos_sha256(const uint8_t *data, size_t len, uint8_t out[MICROOS_SHA256_SIZE]);

/* The same digest over one or more buffers. */
void microos_sha256_init(struct microos_sha256_ctx *ctx);
void microos_sha256_update(struct microos_sha256_ctx *ctx, const uint8_t *data, size_t len);
void microos_sha256_final(struct microos_sha256_ctx *ctx, uint8_t out[MICROOS_SHA256_SIZE]);

#endif /* MICRO_OS_PORT_X86_64_LOADER_SHA256_H */
