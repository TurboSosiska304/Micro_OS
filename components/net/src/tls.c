/*
 * Micro OS — the TLS client: mbedTLS over the transport of the port.
 *
 * This file is the whole of the platform layer of the library and the whole
 * of the client (ADR 0024 §3). The library itself is in third_party/mbedtls
 * with its own configuration; what is here is the half mbedTLS cannot write
 * for a machine without a libc:
 *
 *   the allocator  kmm, at the priority of a system service;
 *   the formatter  the snprintf of the kernel's C library;
 *   the clock      k_clock_now_unix(), and a UTC conversion of our own, so
 *                  that the dates of a certificate are checked against the
 *                  moment NTP gave the machine - and a machine of 1970 is a
 *                  refusal with a sentence, never a silent success;
 *   the entropy    net_tls_platform_entropy(), which is RDRAND or the timer
 *                  on x86-64 and the operating system on the host;
 *   the roots      /etc/ssl/certs.pem read through the VFS, parsed in memory.
 *
 * The transport is layered. `net_tls_transport()` answers a template whose
 * `open` takes the plain transport from `self->user` and wraps it, so the
 * same client runs over the sockets of the port and over the socket of a
 * test, and the HTTP client above knows only "a stream of bytes".
 *
 * One exchange at a time: the reason of the last failure is a buffer of the
 * module, like the buffer of the page of the state. Two tasks that speak
 * TLS at the same moment would share it - the network of this system is one
 * session, and the shell is one task.
 */
#include "net/tls.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "kernel/clock.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/sysinfo.h"
#include "kernel/vfs.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "net/tls_platform.h"
#include "psa/crypto.h"

/* The sentence of the last failed connection, for the console. */
static char s_error[256];

/* The personalisation of the DRBG: the library mixes it into the seed. */
static const char s_personalization[] = "microos-tls";

/* The connection: the transport below it and the state of the library. */
struct tls_session {
    const struct http_transport *base;
    void *base_ctx;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool handshaken;
};

long net_tls_time(long *t);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf);

/* --- the platform layer of mbedTLS --- */

/*
 * The clock of the kernel, in the seconds of Unix. mbedTLS calls it through
 * MBEDTLS_PLATFORM_TIME_MACRO, which is why the prototype also stands in
 * the configuration: a macro is expanded where it is used, and the library
 * does not see this file.
 */
long net_tls_time(long *t)
{
    long now = (long)k_clock_now_unix();

    if (t != NULL) {
        *t = now;
    }

    return now;
}

/*
 * The UTC calendar of a moment, the algorithm of Hinnant (the same one the
 * NTP client uses for the console). It exists because mbedTLS asks for a
 * gmtime_r() that a freestanding machine does not have.
 */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    k_i64_t seconds;
    k_i64_t days;
    k_i64_t rem;
    k_u64_t z;
    k_u64_t era;
    k_u32_t doe;
    k_u32_t yoe;
    k_u32_t y;
    k_u32_t doy;
    k_u32_t mp;
    k_u32_t d;
    k_u32_t m;

    if (tt == NULL || tm_buf == NULL) {
        return NULL;
    }

    seconds = (k_i64_t)*tt;
    days = seconds / 86400;
    rem = seconds % 86400;

    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    /* The count of the days starts at 0000-03-01; 719468 is 1970-01-01. */
    z = (k_u64_t)(days + 719468);
    era = z / 146097u;
    doe = (k_u32_t)(z - era * 146097u);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    y = yoe + (k_u32_t)era * 400u;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = (mp < 10u) ? mp + 3u : mp - 9u;

    memset(tm_buf, 0, sizeof(*tm_buf));
    tm_buf->tm_year = (int)(y + (m <= 2u)) - 1900;
    tm_buf->tm_mon = (int)m - 1;
    tm_buf->tm_mday = (int)d;
    tm_buf->tm_hour = (int)(rem / 3600);
    tm_buf->tm_min = (int)((rem % 3600) / 60);
    tm_buf->tm_sec = (int)(rem % 60);

    return tm_buf;
}

/* The allocator of the kernel, at the priority of a system service: a
 * handshake must not be refused because an application filled the memory.
 * The configuration names these two functions, so mbedTLS uses them and
 * never a `calloc` of a libc that is not there. */
void *net_tls_calloc(size_t count, size_t size)
{
    void *block;

    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }

    block = kmm_alloc((k_size_t)(count * size), KMM_PRIORITY_SYSTEM);
    if (block != NULL) {
        memset(block, 0, count * size);
    }

    return block;
}

void net_tls_free(void *block)
{
    kmm_free(block);
}

/* The zeroize of the library, without the explicit_bzero() of a libc: the
 * writes are volatile, so a compiler may not remove a wipe of a secret. */
void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buf;

    while (len > 0) {
        *bytes++ = 0;
        len--;
    }
}

/* The monotonic counter of the library, from the uptime of the kernel. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)k_sysinfo_uptime_ms();
}

/* The entropy of the machine, through the port. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    int rc;

    (void)data;

    if (output == NULL || olen == NULL) {
        return -1;
    }

    *olen = 0;

    rc = net_tls_platform_entropy(output, (k_size_t)len);
    if (rc != 0) {
        return rc;
    }

    *olen = len;

    return 0;
}

/*
 * The library is told about the platform once, before its first use. The
 * calls are idempotent, and the network of this system is one exchange at a
 * time, so no lock is needed around them.
 */
static int tls_platform_init(void)
{
    if (mbedtls_platform_set_snprintf(snprintf) != 0) {
        return -1;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        return -1;
    }

    return 0;
}

/* --- the roots of the machine --- */

/*
 * Parse a buffer of PEM certificates into a chain. `length` counts the zero
 * that ends the buffer, because that is what mbedtls_x509_crt_parse() wants
 * for PEM: a file of the VFS has no zero in it, so its reader adds one and
 * says so here.
 */
static int tls_roots_parse(mbedtls_x509_crt *chain, const void *pem, k_size_t length)
{
    return mbedtls_x509_crt_parse(chain, (const unsigned char *)pem, (size_t)length);
}

/* The roots of `/etc/ssl/certs.pem`, read through the VFS. */
static int tls_roots_load(mbedtls_x509_crt *chain)
{
    struct vfs_stat info;
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char *buffer;
    k_size_t got = 0;
    int rc;

    rc = vfs_stat(TLS_ROOTS_PATH, &info);
    if (rc != 0) {
        snprintf(s_error, sizeof(s_error),
                 "there is no %s: the machine trusts no certificate authority", TLS_ROOTS_PATH);
        return TLS_ERR_ROOTS;
    }

    if (info.type != 1 || info.size == 0) {
        snprintf(s_error, sizeof(s_error), "%s is not a file of certificates", TLS_ROOTS_PATH);
        return TLS_ERR_ROOTS;
    }

    if (info.size > TLS_ROOTS_MAX) {
        snprintf(s_error, sizeof(s_error), "%s is larger than %u KB", TLS_ROOTS_PATH,
                 (unsigned)(TLS_ROOTS_MAX / 1024u));
        return TLS_ERR_ROOTS;
    }

    buffer = kmm_alloc((k_size_t)info.size + 1, KMM_PRIORITY_SYSTEM);
    if (buffer == NULL) {
        snprintf(s_error, sizeof(s_error), "no memory for the roots of %s", TLS_ROOTS_PATH);
        return TLS_ERR_MEMORY;
    }

    rc = vfs_open(TLS_ROOTS_PATH, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        kmm_free(buffer);
        snprintf(s_error, sizeof(s_error), "cannot read %s", TLS_ROOTS_PATH);
        return TLS_ERR_ROOTS;
    }

    while (got < (k_size_t)info.size) {
        rc = vfs_read(&file, buffer + got, (k_size_t)info.size - got);
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            break;
        }
        got += (k_size_t)rc;
    }

    (void)vfs_close(&file);

    if (rc < 0 || got == 0) {
        kmm_free(buffer);
        snprintf(s_error, sizeof(s_error), "cannot read %s", TLS_ROOTS_PATH);
        return TLS_ERR_ROOTS;
    }

    buffer[got] = '\0';

    rc = tls_roots_parse(chain, buffer, got + 1);
    kmm_free(buffer);

    if (rc != 0) {
        char reason[96];

        mbedtls_strerror(rc, reason, sizeof(reason));
        snprintf(s_error, sizeof(s_error), "%s is not a PEM store: %s", TLS_ROOTS_PATH, reason);
        return TLS_ERR_ROOTS;
    }

    if (chain->next == NULL && chain->raw.len == 0) {
        snprintf(s_error, sizeof(s_error), "%s holds no certificate", TLS_ROOTS_PATH);
        return TLS_ERR_ROOTS;
    }

    return TLS_OK;
}

int net_tls_ca_count(const void *pem, k_size_t size, k_size_t *certificates)
{
    mbedtls_x509_crt chain;
    const mbedtls_x509_crt *cursor;
    char *copy;
    k_size_t count = 0;
    int rc;

    if (pem == NULL || size == 0 || size == (k_size_t)-1) {
        return TLS_ERR_ROOTS;
    }

    /* The library reads a byte past the certificates for the zero of a PEM
     * buffer, and the caller's buffer is not ours to read past. */
    copy = kmm_alloc(size + 1, KMM_PRIORITY_SYSTEM);
    if (copy == NULL) {
        return TLS_ERR_MEMORY;
    }

    memcpy(copy, pem, size);
    copy[size] = '\0';

    mbedtls_x509_crt_init(&chain);

    rc = tls_roots_parse(&chain, copy, size + 1);
    kmm_free(copy);

    if (rc != 0) {
        mbedtls_x509_crt_free(&chain);
        return TLS_ERR_ROOTS;
    }

    for (cursor = &chain; cursor != NULL; cursor = cursor->next) {
        if (cursor->raw.len > 0) {
            count++;
        }
    }

    mbedtls_x509_crt_free(&chain);

    if (count == 0) {
        return TLS_ERR_ROOTS;
    }

    if (certificates != NULL) {
        *certificates = count;
    }

    return TLS_OK;
}

/* --- the policy of the clock --- */

bool net_tls_clock_ok(k_u64_t unix_seconds, bool clock_set)
{
    /*
     * Two conditions, one policy. A clock that was never set is the seconds
     * since the boot, which is 1970 more or less; a clock that was set to
     * 1970 is the same lie with a signature. Both can check no date, and a
     * connection whose dates are not checked is not authenticated. The
     * policy is a refusal (ADR 0024 §3), and the sentence of the failure
     * says what to run.
     */
    if (!clock_set) {
        return false;
    }

    return unix_seconds >= TLS_TIME_MIN_UNIX;
}

/* --- the connection --- */

static void tls_fail(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
}

/* The code of a failure of the library, as one sentence of the module. */
static void tls_error(int rc, const char *what)
{
    char reason[128];

    mbedtls_strerror(rc, reason, sizeof(reason));
    snprintf(s_error, sizeof(s_error), "%s: %s", what, reason);
}

static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct tls_session *session = (struct tls_session *)ctx;
    int sent;

    sent = session->base->send(session->base_ctx, buf, (k_size_t)len);

    if (sent < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    if (sent == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    return sent;
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct tls_session *session = (struct tls_session *)ctx;
    int got;

    got = session->base->recv(session->base_ctx, buf, (k_size_t)len);

    if (got < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    return got;
}

/* Release everything a session may have brought up, in the reverse order. */
static void tls_session_free(struct tls_session *session)
{
    if (session == NULL) {
        return;
    }

    if (session->handshaken) {
        (void)mbedtls_ssl_close_notify(&session->ssl);
    }

    mbedtls_ssl_free(&session->ssl);
    mbedtls_ssl_config_free(&session->conf);
    mbedtls_x509_crt_free(&session->roots);
    mbedtls_ctr_drbg_free(&session->drbg);
    mbedtls_entropy_free(&session->entropy);

    if (session->base_ctx != NULL && session->base != NULL) {
        session->base->close(session->base_ctx);
    }

    kmm_free(session);
}

/*
 * The chain and the name of the server. With authmode REQUIRED the library
 * fails the handshake by itself; the flags are read afterwards so that the
 * console says *why* - "not trusted" and "another name" are two different
 * mistakes of a person or of a mirror.
 */
static int tls_verify(struct tls_session *session)
{
    uint32_t flags;
    char info[128];

    flags = mbedtls_ssl_get_verify_result(&session->ssl);
    if (flags == 0) {
        return TLS_OK;
    }

    info[0] = '\0';
    (void)mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);

    while (info[0] != '\0' && (info[strlen(info) - 1] == '\n' || info[strlen(info) - 1] == ' ')) {
        info[strlen(info) - 1] = '\0';
    }

    snprintf(s_error, sizeof(s_error),
             "the certificate of the server is not trusted (flags 0x%lx): %s", (unsigned long)flags,
             info);

    return TLS_ERR_CERT;
}

static int tls_open(const struct http_transport *self, const char *host, int port, void **ctx)
{
    struct tls_session *session;
    int rc;

    if (ctx == NULL) {
        return -1;
    }

    *ctx = NULL;
    s_error[0] = '\0';

    if (self == NULL || self->user == NULL || host == NULL || host[0] == '\0') {
        tls_fail("the TLS transport has no transport below it");
        return -1;
    }

    /*
     * The clock before the socket: a machine of 1970 has nothing to check a
     * certificate with, and the connection is refused before a byte of it
     * is sent.
     */
    if (!net_tls_clock_ok(k_clock_now_unix(), k_clock_present())) {
        tls_fail("the clock is not set (1970): the date of a certificate cannot be checked - "
                 "run `ntp` first");
        return -1;
    }

    if (tls_platform_init() != 0) {
        tls_fail("the TLS platform layer did not come up");
        return -1;
    }

    session = kmm_alloc(sizeof(*session), KMM_PRIORITY_SYSTEM);
    if (session == NULL) {
        tls_fail("no memory for a TLS session");
        return -1;
    }

    memset(session, 0, sizeof(*session));
    session->base = self->user;

    mbedtls_ssl_init(&session->ssl);
    mbedtls_ssl_config_init(&session->conf);
    mbedtls_x509_crt_init(&session->roots);
    mbedtls_entropy_init(&session->entropy);
    mbedtls_ctr_drbg_init(&session->drbg);

    rc = session->base->open(session->base, host, port, &session->base_ctx);
    if (rc != 0) {
        snprintf(s_error, sizeof(s_error), "cannot connect to %s:%d", host, port);
        tls_session_free(session);
        return -1;
    }

    if (tls_roots_load(&session->roots) != TLS_OK) {
        tls_session_free(session);
        return -1;
    }

    rc = mbedtls_ctr_drbg_seed(&session->drbg, mbedtls_entropy_func, &session->entropy,
                               (const unsigned char *)s_personalization,
                               sizeof(s_personalization) - 1);
    if (rc != 0) {
        tls_error(rc, "the random generator of the machine did not start");
        tls_session_free(session);
        return -1;
    }

    rc = mbedtls_ssl_config_defaults(&session->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        tls_error(rc, "the TLS configuration was refused");
        tls_session_free(session);
        return -1;
    }

    mbedtls_ssl_conf_rng(&session->conf, mbedtls_ctr_drbg_random, &session->drbg);
    mbedtls_ssl_conf_ca_chain(&session->conf, &session->roots, NULL);
    mbedtls_ssl_conf_authmode(&session->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    /* The floor of the protocol. A machine of 1.x speaks TLS 1.2 and 1.3
     * and nothing older; the default of the library is the same, and the
     * call says it once instead of trusting a default. */
    mbedtls_ssl_conf_min_tls_version(&session->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    rc = mbedtls_ssl_setup(&session->ssl, &session->conf);
    if (rc != 0) {
        tls_error(rc, "the TLS session did not start");
        tls_session_free(session);
        return -1;
    }

    /*
     * The name of the URL is the name checked in the certificate and, when
     * it is a name and not an address, the SNI of the handshake. A mirror
     * reached by an address is checked against the address.
     */
    rc = mbedtls_ssl_set_hostname(&session->ssl, host);
    if (rc != 0) {
        tls_error(rc, "the name of the server was refused");
        tls_session_free(session);
        return -1;
    }

    mbedtls_ssl_set_bio(&session->ssl, session, tls_bio_send, tls_bio_recv, NULL);

    for (;;) {
        rc = mbedtls_ssl_handshake(&session->ssl);
        if (rc == 0) {
            break;
        }

        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (tls_verify(session) != TLS_OK) {
                tls_session_free(session);
                return -1;
            }

            tls_error(rc, "the TLS handshake failed");
            tls_session_free(session);
            return -1;
        }
    }

    rc = tls_verify(session);
    if (rc != TLS_OK) {
        tls_session_free(session);
        return -1;
    }

    session->handshaken = true;
    *ctx = session;

    return 0;
}

static int tls_send(void *ctx, const void *data, k_size_t size)
{
    struct tls_session *session = (struct tls_session *)ctx;
    const unsigned char *bytes = (const unsigned char *)data;
    k_size_t done = 0;

    if (session == NULL || !session->handshaken) {
        return -1;
    }

    while (done < size) {
        int rc = mbedtls_ssl_write(&session->ssl, bytes + done, (size_t)(size - done));

        if (rc <= 0) {
            tls_error(rc, "the TLS write failed");
            return -1;
        }

        done += (k_size_t)rc;
    }

    return (int)done;
}

static int tls_recv(void *ctx, void *data, k_size_t size)
{
    struct tls_session *session = (struct tls_session *)ctx;
    int rc;

    if (session == NULL || !session->handshaken) {
        return -1;
    }

    for (;;) {
        rc = mbedtls_ssl_read(&session->ssl, (unsigned char *)data, (size_t)size);

        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }

        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return 0;
        }

        if (rc < 0) {
            tls_error(rc, "the TLS read failed");
            return -1;
        }

        return rc;
    }
}

static void tls_close(void *ctx)
{
    tls_session_free((struct tls_session *)ctx);
}

/* The template: `user` is the transport below, set by the caller. */
static const struct http_transport s_tls_transport = {
    .open = tls_open,
    .send = tls_send,
    .recv = tls_recv,
    .close = tls_close,
    .user = NULL,
};

const struct http_transport *net_tls_transport(void)
{
    return &s_tls_transport;
}

const char *net_tls_last_error(void)
{
    return s_error;
}
