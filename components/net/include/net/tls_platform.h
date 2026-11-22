/*
 * Micro OS — the entropy of the machine, for the TLS client.
 *
 * mbedTLS does not read /dev/urandom here (MBEDTLS_NO_PLATFORM_ENTROPY):
 * it calls mbedtls_hardware_poll(), which components/net/src/tls.c turns
 * into this call. The port owns the answer because only the port knows what
 * its processor has:
 *
 *   x86-64  RDRAND when CPUID says the processor implements it, and the
 *           timer (TSC jitter) otherwise (ports/port_x86_64/src/tls_platform.c);
 *   host    getrandom(), the entropy of the operating system under the
 *           simulator and the tests (ports/port_host/src/tls_platform.c).
 *
 * Returns 0 and fills `out` on success, a negative code when the machine has
 * no entropy at all; a caller that cannot get any must not continue to a
 * handshake.
 */
#ifndef MICRO_OS_NET_TLS_PLATFORM_H
#define MICRO_OS_NET_TLS_PLATFORM_H

#include "kernel/types.h"

int net_tls_platform_entropy(unsigned char *out, k_size_t len);

#endif /* MICRO_OS_NET_TLS_PLATFORM_H */
