/*
 * Micro OS — TLS on the serving side: the same stack, the other direction.
 *
 * The client (net/tls.h) has been there since 1.2.0 and lives in
 * third_party/mbedtls with the configuration of a client. A server needs the
 * other half of the handshake (`MBEDTLS_SSL_SRV_C`) and one thing a client
 * never has: a certificate and the secret key of it. Both are files of the
 * machine, read through the VFS and parsed in memory - the library has no
 * file system of its own here, and a certificate is not a reason to give it
 * one.
 *
 * A session is wrapped, not replaced: the server above this file hands a
 * socket of the kernel and receives a stream with the same two calls, so
 * net/httpd.c does not know whether the bytes below it are plain.
 *
 * The key of the server is a secret of the machine, and this is the honest
 * shape of the trust a machine without an MMU can give it: the file is read
 * by whoever can read /etc/ssl, and the model of the system says that is the
 * root. A server certificate of a test (`openssl req -x509`) is what the
 * smoke test uses; a machine on a network of its own makes its own.
 */
#ifndef MICRO_OS_NET_TLS_SERVER_H
#define MICRO_OS_NET_TLS_SERVER_H

#include "kernel/net.h"
#include "kernel/types.h"

/* The default names of the pair, under /etc/ssl. */
#define TLS_SERVER_CERT_PATH "/etc/ssl/server.crt"
#define TLS_SERVER_KEY_PATH "/etc/ssl/server.key"

/* The largest certificate and key this server reads. A chain of a real
 * authority is a few kilobytes; the ceiling keeps a hostile file from asking
 * for the memory of the machine. */
#define TLS_SERVER_CERT_MAX (32u * 1024u)
#define TLS_SERVER_KEY_MAX (8u * 1024u)

/*
 * Wrap the session in TLS and complete the handshake. 0 on success, and
 * `*context` is the stream of the session; a negative answer is a client that
 * did not become one, and the session is closed by the caller. The
 * certificate is read on the first call and kept for the life of the
 * server; a session that fails the handshake does not cost a parse.
 */
int net_tls_server_open(k_socket_t *socket, void **context);

/* The two calls of a stream (see the note above). */
int net_tls_server_send(void *context, const void *data, k_size_t size);
int net_tls_server_recv(void *context, void *data, k_size_t size);
void net_tls_server_close(void *context);

/* The sentence of the last failure, for the console. */
const char *net_tls_server_last_error(void);

/* Whether this image has the serving side of TLS at all: the board keeps its
 * copy of ESP-IDF for the client of the shell and does not build this file
 * (ADR 0018). The command says so instead of pretending to be HTTPS. */
bool net_tls_server_supported(void);

/*
 * The pair of the server, when a caller wants another one: the defaults of
 * the file above are used until this is called. The two paths are copied. 0
 * on success, -1 when they do not fit.
 */
int net_tls_server_set_pair(const char *certificate, const char *key);

#endif /* MICRO_OS_NET_TLS_SERVER_H */
