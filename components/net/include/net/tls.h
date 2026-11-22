/*
 * Micro OS — TLS, as the layer under the HTTP client.
 *
 * The client of ADR 0024 §3 is mbedTLS, vendored for the machine that has
 * the room for it (third_party/mbedtls): TLS 1.2 and 1.3, a certificate
 * chain checked against the roots of `/etc/ssl/certs.pem`, and a server
 * name that is the name of the URL. The board keeps the copy of ESP-IDF and
 * does not compile this file.
 *
 * The shape of the connection is the one of the HTTP client: a transport of
 * bytes. `net_tls_transport()` answers a template whose `open` secures the
 * transport below it, which the caller names in `user`:
 *
 *     struct http_transport secure = *net_tls_transport();
 *     secure.user = plain;
 *     http_stream(&(struct http_stream){ .transport = &secure, ... });
 *
 * The template is a constant: each stream that is one connection at a time
 * copies it and points it at its own transport, so two tasks with two
 * exchanges do not share a word.
 */
#ifndef MICRO_OS_NET_TLS_H
#define MICRO_OS_NET_TLS_H

#include "kernel/types.h"
#include "net/http.h"

/*
 * Where the roots of the machine live: one PEM file, the certificates of the
 * authorities the machine trusts, concatenated in any number. The path is a
 * file of the VFS, so a person replaces it with `wget https://... FILE` and
 * no reflash.
 */
#define TLS_ROOTS_PATH "/etc/ssl/certs.pem"

/* The largest file of roots this client reads. A store of CAs is tens of
 * kilobytes; the ceiling keeps a hostile file from asking for the memory of
 * the machine. */
#define TLS_ROOTS_MAX (256u * 1024u)

/*
 * The first moment a certificate can be checked at. A machine of 1970 has
 * not been given the time by NTP (or by a person), and a certificate has
 * dates: the client refuses the connection and says why instead of passing
 * it without a check (ADR 0024 §3). The moment is 2020-01-01, well before
 * any certificate of the era and well after a clock that was never set.
 */
#define TLS_TIME_MIN_UNIX 1577836800ull

/* The answers of the calls of this file. */
#define TLS_OK 0
#define TLS_ERR_CLOCK (-1)     /* the clock is not set; the dates cannot be checked */
#define TLS_ERR_ROOTS (-2)     /* /etc/ssl/certs.pem is not there or is not PEM */
#define TLS_ERR_MEMORY (-3)    /* the allocator of the kernel said no */
#define TLS_ERR_CONNECT (-4)   /* the TCP session below did not come up */
#define TLS_ERR_CERT (-5)      /* the chain or the name of the server is not trusted */
#define TLS_ERR_HANDSHAKE (-6) /* the handshake failed for another reason */

/*
 * The transport that secures the one in `user`, or NULL when this image was
 * built without TLS. The template has its `user` empty; the caller sets it
 * to the transport of the bytes.
 */
const struct http_transport *net_tls_transport(void);

/*
 * The reason of the last failed `open` of a TLS transport, as one line for
 * the console. The network of this system is one exchange at a time, so the
 * text lives in the module and not in the connection (the same shape as the
 * buffer of the HTTP client).
 */
const char *net_tls_last_error(void);

/* Whether a moment is one the dates of a certificate can be checked at. */
bool net_tls_clock_ok(k_u64_t unix_seconds, bool clock_set);

/*
 * Parse a buffer of PEM certificates the way the roots of the machine are
 * parsed: zero certificates is not a store, and a broken certificate is an
 * error and not a skipped part. `certificates` may be NULL. It exists for
 * the tests of this file; the client reads the file itself.
 */
int net_tls_ca_count(const void *pem, k_size_t size, k_size_t *certificates);

#endif /* MICRO_OS_NET_TLS_H */
