/*
 * Micro OS — SHA-256, the hash of a signed image (ADR 0020).
 *
 * The same file is compiled into the host tool and into the 32-bit loader:
 * it has no libc in it, no allocation and no state outside the call, so the
 * verifier of the boot and the signer of the build agree by sharing code
 * rather than by sharing a test vector.
 */
#ifndef MICRO_OS_PORT_X86_64_LOADER_SHA256_H
#define MICRO_OS_PORT_X86_64_LOADER_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define MICROOS_SHA256_SIZE 32

/* The digest of a whole buffer. A NULL buffer is allowed when len is 0. */
void microos_sha256(const uint8_t *data, size_t len, uint8_t out[MICROOS_SHA256_SIZE]);

#endif /* MICRO_OS_PORT_X86_64_LOADER_SHA256_H */
