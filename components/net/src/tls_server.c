/*
 * Micro OS — the serving side of TLS: mbedTLS with the certificate of the
 * machine (net/tls_server.h).
 *
 * The library is the vendored one of third_party/mbedtls, built by this
 * component with its configuration; the serving side adds the other half of
 * the protocol (`MBEDTLS_SSL_SRV_C`) and a pair of files - a certificate and
 * the secret key of it - read through the VFS and parsed in memory.
 *
 * There is one server on this machine and one configuration of it, so the
 * certificate is parsed once, on the first session, and kept. A session of
 * its own is an `mbedtls_ssl_context`, an input buffer and an output buffer
 * of four kilobytes: sixteen sessions cost fewer than a hundred kilobytes of
 * the PSRAM, and the arena of a program of the machine is not touched.
 *
 * The socket below is the one of the kernel: the protocol is a stream, and
 * the two callbacks of this file move its bytes. A wait of the port that
 * answers "not yet" is WANT_READ of the library, and the handshake loops on
 * the clock of the kernel with a deadline - a client that says half a hello
 * and stops costs no task for ever.
 *
 * The clock of the certificate is the clock of the machine: mbedTLS checks
 * the dates of its own certificate when a client offers none, and a machine
 * of 1970 says so in the console instead of serving a certificate nobody can
 * validate.
 */
#include "net/tls_server.h"

#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/vfs.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "net/tls_platform.h"
#include "psa/crypto.h"

/* The time a client has to complete a handshake, and the step of the retry. */
#define TLS_SERVER_HANDSHAKE_MS 6000u
#define TLS_SERVER_POLL_MS 10

/* The sentence of the last failure, for the console. */
static char s_error[384];

/* The personalisation of the DRBG, as on the client side. */
static const char s_personalization[] = "microos-tls-server";

/* The pair of the server and the moment of the last failure. */
static char s_certificate[VFS_PATH_MAX] = TLS_SERVER_CERT_PATH;
static char s_key[VFS_PATH_MAX] = TLS_SERVER_KEY_PATH;

static mbedtls_ssl_config s_config;
static mbedtls_x509_crt s_cert;
static mbedtls_pk_context s_private_key;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_config_ready;

/* The session: the socket below and the state of the library. */
struct tls_server_session {
    k_socket_t *socket;
    mbedtls_ssl_context ssl;
};

long net_tls_time(long *t);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

bool net_tls_server_supported(void)
{
    return true;
}

const char *net_tls_server_last_error(void)
{
    return s_error;
}

int net_tls_server_set_pair(const char *certificate, const char *key)
{
    if (certificate == NULL || key == NULL) {
        return -1;
    }

    if (strlen(certificate) >= sizeof(s_certificate) || strlen(key) >= sizeof(s_key)) {
        return -1;
    }

    if (s_config_ready) {
        /* The pair of a running server is not swapped under the sessions
         * that use it: a caller stops the server and starts it again. */
        return -1;
    }

    memcpy(s_certificate, certificate, strlen(certificate) + 1);
    memcpy(s_key, key, strlen(key) + 1);

    return 0;
}

/* The library is told about the platform once, before its first use. The
 * calls are idempotent and there is one server on the machine. */
static int tls_server_platform_init(void)
{
    if (mbedtls_platform_set_snprintf(snprintf) != 0) {
        return -1;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        return -1;
    }

    return 0;
}

/* The bytes of a file of the VFS, with the zero that PEM asks for. */
static int tls_server_read(const char *path, char **buffer, k_size_t *length)
{
    struct vfs_stat info;
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_size_t got = 0;
    int rc;

    rc = vfs_stat(path, &info);
    if (rc != 0 || info.type != 1 || info.size == 0) {
        snprintf(s_error, sizeof(s_error), "%s is not a file of the machine", path);
        return -1;
    }

    if (info.size > TLS_SERVER_CERT_MAX) {
        snprintf(s_error, sizeof(s_error), "%s is larger than %u KB", path,
                 (unsigned)(TLS_SERVER_CERT_MAX / 1024u));
        return -1;
    }

    *buffer = kmm_alloc((k_size_t)info.size + 1, KMM_PRIORITY_SYSTEM);
    if (*buffer == NULL) {
        snprintf(s_error, sizeof(s_error), "no memory for %s", path);
        return -1;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        kmm_free(*buffer);
        *buffer = NULL;
        snprintf(s_error, sizeof(s_error), "cannot open %s", path);
        return -1;
    }

    while (got < (k_size_t)info.size) {
        rc = vfs_read(&file, *buffer + got, (k_size_t)info.size - got);

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
        kmm_free(*buffer);
        *buffer = NULL;
        snprintf(s_error, sizeof(s_error), "cannot read %s", path);
        return -1;
    }

    (*buffer)[got] = '\0';
    *length = got + 1;

    return 0;
}

/*
 * The certificate and the key, parsed into the chain and the context of the
 * module. The two are PEM files of /etc/ssl, and a machine that booted
 * without a certificate is a server that refuses to start, not one that
 * hands a client a certificate of no authority.
 */
static int tls_server_pair_load(void)
{
    char *buffer = NULL;
    k_size_t length = 0;
    int rc;

    mbedtls_x509_crt_init(&s_cert);
    mbedtls_pk_init(&s_private_key);

    if (tls_server_read(s_certificate, &buffer, &length) != 0) {
        return -1;
    }

    rc = mbedtls_x509_crt_parse(&s_cert, (const unsigned char *)buffer, (size_t)length);
    kmm_free(buffer);

    if (rc != 0) {
        char reason[96];

        mbedtls_strerror(rc, reason, sizeof(reason));
        snprintf(s_error, sizeof(s_error), "%s is not a certificate: %s", s_certificate, reason);
        return -1;
    }

    if (tls_server_read(s_key, &buffer, &length) != 0) {
        return -1;
    }

    rc = mbedtls_pk_parse_key(&s_private_key, (const unsigned char *)buffer, (size_t)length, NULL,
                              0, mbedtls_ctr_drbg_random, &s_drbg);
    kmm_free(buffer);

    if (rc != 0) {
        char reason[96];

        mbedtls_strerror(rc, reason, sizeof(reason));
        snprintf(s_error, sizeof(s_error), "%s is not a key: %s", s_key, reason);
        return -1;
    }

    return 0;
}

/* The configuration of the server, built once. A failure leaves the error of
 * the library in the sentence of the module. */
static int tls_server_config_init(void)
{
    int rc;

    if (s_config_ready) {
        return 0;
    }

    if (tls_server_platform_init() != 0) {
        snprintf(s_error, sizeof(s_error), "the platform layer of mbedTLS did not start");
        return -1;
    }

    /* The generator first: the parse of the key takes a random number for
     * an encrypted PEM, and a context that was never seeded is not a place
     * to find one. */
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);

    rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                               (const unsigned char *)s_personalization,
                               sizeof(s_personalization) - 1u);
    if (rc != 0) {
        mbedtls_strerror(rc, s_error, sizeof(s_error));
        return -1;
    }

    if (tls_server_pair_load() != 0) {
        return -1;
    }

    mbedtls_ssl_config_init(&s_config);

    rc = mbedtls_ssl_config_defaults(&s_config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        mbedtls_strerror(rc, s_error, sizeof(s_error));
        return -1;
    }

    mbedtls_ssl_conf_rng(&s_config, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ssl_conf_min_tls_version(&s_config, MBEDTLS_SSL_VERSION_TLS1_2);

    /* A browser that asks for the certificate of the client does not get
     * one: the machine has users at a console and no certificate for them. */
    mbedtls_ssl_conf_authmode(&s_config, MBEDTLS_SSL_VERIFY_NONE);

    rc = mbedtls_ssl_conf_own_cert(&s_config, &s_cert, &s_private_key);
    if (rc != 0) {
        mbedtls_strerror(rc, s_error, sizeof(s_error));
        return -1;
    }

    s_config_ready = true;

    return 0;
}

/* --- the two callbacks of the stream --- */

static int tls_server_send_cb(void *context, const unsigned char *buf, size_t len)
{
    k_socket_t *socket = context;
    int n = k_socket_send(socket, buf, (k_size_t)len, true);

    if (n <= 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    return n;
}

static int tls_server_recv_cb(void *context, unsigned char *buf, size_t len)
{
    k_socket_t *socket = context;
    int n = k_socket_recv(socket, buf, (k_size_t)len);

    if (n == 0) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }

    if (n < 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    return n;
}

/* The handshake, with the clock as the only bound: a client of a home
 * network is either there at once or gone. */
int net_tls_server_open(k_socket_t *socket, void **context)
{
    struct tls_server_session *session;
    k_u64_t deadline;
    int rc;

    if (socket == NULL || context == NULL) {
        return -1;
    }

    *context = NULL;

    if (tls_server_config_init() != 0) {
        return -1;
    }

    session = kmm_alloc(sizeof(*session), KMM_PRIORITY_USER);
    if (session == NULL) {
        snprintf(s_error, sizeof(s_error), "no memory for a session of TLS");
        return -1;
    }

    memset(session, 0, sizeof(*session));
    session->socket = socket;
    mbedtls_ssl_init(&session->ssl);

    rc = mbedtls_ssl_setup(&session->ssl, &s_config);
    if (rc != 0) {
        mbedtls_strerror(rc, s_error, sizeof(s_error));
        mbedtls_ssl_free(&session->ssl);
        kmm_free(session);
        return -1;
    }

    /* The name the client asked for is not checked by a server: it is the
     * client that has to know to whom it speaks. */
    mbedtls_ssl_set_bio(&session->ssl, socket, tls_server_send_cb, tls_server_recv_cb, NULL);

    deadline = k_sysinfo_uptime_ms() + TLS_SERVER_HANDSHAKE_MS;

    while ((rc = mbedtls_ssl_handshake(&session->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_strerror(rc, s_error, sizeof(s_error));
            mbedtls_ssl_free(&session->ssl);
            kmm_free(session);
            return -1;
        }

        if (k_sysinfo_uptime_ms() >= deadline) {
            snprintf(s_error, sizeof(s_error), "the handshake took longer than %u ms",
                     (unsigned)TLS_SERVER_HANDSHAKE_MS);
            mbedtls_ssl_free(&session->ssl);
            kmm_free(session);
            return -1;
        }

        k_sched_sleep_ms(TLS_SERVER_POLL_MS);
    }

    *context = session;

    return 0;
}

int net_tls_server_send(void *context, const void *data, k_size_t size)
{
    struct tls_server_session *session = context;
    const unsigned char *bytes = data;
    k_size_t sent = 0;

    if (session == NULL) {
        return -1;
    }

    while (sent < size) {
        int n = mbedtls_ssl_write(&session->ssl, bytes + sent, size - sent);

        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            k_sched_sleep_ms(TLS_SERVER_POLL_MS);
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        sent += (k_size_t)n;
    }

    return (int)sent;
}

int net_tls_server_recv(void *context, void *data, k_size_t size)
{
    struct tls_server_session *session = context;
    int n;

    if (session == NULL) {
        return -1;
    }

    n = mbedtls_ssl_read(&session->ssl, data, size);

    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* The caller of a stream reads -1 as "not yet" and looks at its own
         * deadline: the port has no other way to say a wait. */
        return -1;
    }

    if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
    }

    if (n < 0) {
        return -1;
    }

    return n;
}

void net_tls_server_close(void *context)
{
    struct tls_server_session *session = context;

    if (session == NULL) {
        return;
    }

    /* The good bye is best effort: the client may have gone already, and the
     * session is closed by the caller either way. */
    (void)mbedtls_ssl_close_notify(&session->ssl);

    mbedtls_ssl_free(&session->ssl);
    kmm_free(session);
}
