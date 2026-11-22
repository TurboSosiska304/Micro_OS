/*
 * Micro OS — the configuration of the vendored mbedTLS.
 *
 * The library is built from the upstream sources of
 * `third_party/mbedtls` and this file replaces `mbedtls_config.h`: it is the
 * one file of the configuration, and every switch that is not written here
 * is off. The shape is a TLS *client* of a machine without a libc:
 *
 *   - TLS 1.2 and TLS 1.3, X.509, ECDHE with ECDSA or RSA certificates,
 *     RSA-PSS signatures (TLS 1.3 has no other kind of RSA signature);
 *   - AES-GCM, AES-CCM, ChaCha20-Poly1305, SHA-1/256/384/512;
 *   - PEM roots, parsed into memory; no file system, no sockets, no
 *     threads, no timing, no server, no certificate writing;
 *   - the platform layer of Micro OS: `kmm` behind `mbedtls_calloc`, the
 *     clock of the kernel behind `mbedtls_time`, a UTC conversion of our
 *     own, and entropy from the port (`mbedtls_hardware_poll` lives in
 *     components/net/src/tls.c).
 *
 * Two symbols of the platform layer are declared here because mbedTLS asks
 * for them as macros before `platform.h` can see a prototype:
 *
 *   net_tls_time()        the seconds of Unix, from the clock of the kernel
 *                         (components/net/src/tls.c)
 *   mbedtls_hardware_poll() the entropy of the machine, from the port
 *                         (`ports/port_x86_64/src/tls_platform.c` on x86-64,
 *                         `ports/port_host/src/tls_platform.c` on the host)
 *
 * The file is included by every translation unit of the library through
 * `MBEDTLS_CONFIG_FILE`; a definition added here is a decision, and a
 * definition missing here is a feature that is not in the image.
 */
#ifndef MICRO_OS_MBEDTLS_CONFIG_H
#define MICRO_OS_MBEDTLS_CONFIG_H

#include <stddef.h>

/* --- the platform: no libc under the library --- */

/*
 * The allocator, the formatted output and the clock come from the caller.
 * `MBEDTLS_PLATFORM_C` makes them function pointers that tls.c sets once,
 * with the exception of the clock: mbedTLS calls it as a macro, so its
 * prototype has to be visible from here.
 */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_SNPRINTF_ALT
#define MBEDTLS_PLATFORM_TIME_TYPE_MACRO long
long net_tls_time(long *t);
#define MBEDTLS_PLATFORM_TIME_MACRO net_tls_time

/*
 * The allocator is named here as a macro and not set at run time: a
 * function pointer initialised to `calloc` is a reference to a libc symbol
 * that does not exist under this kernel. The two functions are the kernel's
 * allocator (components/net/src/tls.c).
 */
void *net_tls_calloc(size_t count, size_t size);
void net_tls_free(void *block);
#define MBEDTLS_PLATFORM_CALLOC_MACRO net_tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO net_tls_free

/*
 * Three libc facilities the library would otherwise reach for, and the
 * implementations of the kernel (components/net/src/tls.c): a zeroize that
 * no optimizer may remove, a monotonic millisecond counter, and the UTC
 * calendar of a moment. mbedTLS knows a machine without any of them.
 */
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_GMTIME_R_ALT

/*
 * A certificate has dates, and a machine whose clock NTP has not set cannot
 * check them. The dates are checked, and `net_tls_clock_ok()` in tls.c is
 * the policy of the client: a clock before 2020 refuses the connection
 * instead of passing it silently (ADR 0024 §3).
 */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE

/*
 * There is no /dev/urandom and no `getrandom()` under the kernel: the
 * entropy comes from the port through `mbedtls_hardware_poll()`. A machine
 * without RDRAND falls back to the timer and says so in its source.
 */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* TLS 1.3 derives its keys through PSA; the client enables both versions. */
#define MBEDTLS_HKDF_C
#define MBEDTLS_PSA_CRYPTO_C

/* --- the primitives --- */

#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C

/*
 * The certificates: ASN.1, PEM, the public-key layer and X.509. The file
 * system is not enabled (`MBEDTLS_FS_IO` is off) because the roots are read
 * through the VFS of the kernel and handed over as a buffer.
 */
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
/*
 * The bignum code of the library divides a 128-bit word by a 64-bit one with
 * the help of the compiler's `__udivti3`, which a freestanding kernel does
 * not link. The slow path of the library is the answer: a handshake costs a
 * few milliseconds more and needs nothing below it.
 */
#define MBEDTLS_NO_UDBL_DIVISION

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/*
 * The certificate of a server may name an address in its SAN. The library
 * calls inet_pton() of the platform when the headers of the build define
 * AF_INET6, and this kernel has no inet_pton(); the switch selects the
 * library's own implementation, which is why it exists.
 */
#define MBEDTLS_TEST_SW_INET_PTON

/*
 * The curves of the ClientHello: X25519 for TLS 1.3, P-256 and P-384 for the
 * signatures of the certificates. P-521 is left out on purpose; a server
 * with a P-521 key is not one this machine has to talk to.
 */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* --- the protocol --- */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
/*
 * The middlebox compatibility of RFC 8446 appendix D.2: a TLS 1.3 server
 * (OpenSSL, and every CDN) sends the dummy ChangeCipherSpec of a 1.2
 * handshake, and a client without this mode stops on it.
 */
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* The strings of mbedtls_strerror(), for the message of a failed handshake. */
#define MBEDTLS_ERROR_C

#endif /* MICRO_OS_MBEDTLS_CONFIG_H */
