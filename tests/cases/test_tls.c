/*
 * Micro OS — the TLS client: the policy of the clock, the store of roots and
 * the layer of the transport.
 *
 * The handshake itself is not here: a server of TLS is not a part of the
 * unit tests, and the tests of the machine (`tools/x86_net_smoke.py`) put a
 * real one on the loopback of QEMU with a certificate of its own. What is
 * here is everything a handshake would use before and around it, and the
 * errors that must not be a silent success.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/vfs.h"
#include "net/http.h"
#include "net/tls.h"

/*
 * A certificate of a test authority, valid to 2036. It is the store of the
 * tests: two of them is a store of two, and a byte changed in the middle is
 * a store that is not PEM.
 */
static const char s_test_ca[] = "-----BEGIN CERTIFICATE-----\n"
                                "MIIC9DCCAdygAwIBAgIUEgaaayQ0Ez7bTyFP8bsMnFUQk5gwDQYJKoZIhvcNAQEL\n"
                                "BQAwGjEYMBYGA1UEAwwPTWljcm9PUyBUZXN0IENBMB4XDTI2MTEyMjE5NTYwN1oX\n"
                                "DTM2MTExOTE5NTYwN1owGjEYMBYGA1UEAwwPTWljcm9PUyBUZXN0IENBMIIBIjAN\n"
                                "BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAyKdUpeJT0rmyQAhcYqoMHuaxcX1U\n"
                                "TSRIlhtRic+4Enz3Nu81VOQrPNQSbZ8dE5ofzVkjtUSEltLeIWohw1VtKUxw46xM\n"
                                "/MFwYrFzZzuybkYAHBa41HdXoAQ/H8Lr6PyjH6VpPpOXjVVm43b7ZmaSMzIjZR+p\n"
                                "nPZhAwIjOiLDwsirlLHOdXcXOEn6QOhheP9bUE6Vr6qQsf0TXBSC9AfW1Zq5nx4J\n"
                                "BQGqbcPe2am3DeGCAB+8YiT9llWi+v5vOc+XzA9cVFVB1+DzzvGQT5DY37OaZPUG\n"
                                "haoPE1ioDN7jFYWxn9HluAGB8B7CBtmvojj7bd/k+c924j1xsBClTWxYEwIDAQAB\n"
                                "ozIwMDAdBgNVHQ4EFgQUdAfiWiURQk9TcTEKtqc3p0gjjo0wDwYDVR0TAQH/BAUw\n"
                                "AwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAfLJ+xrfh/MTT+gMhdLcmxzah9SzUGjza\n"
                                "XWaAIUkZSesXBG7XMoLuu+FHfTGfZ9Bjm2R+4l/lTPDK2Wc7T4SWdw/eM1a3Hyau\n"
                                "YFEIWkSAEyJeLPeNnsAhzOOPM6SbTwcfS7grQlT4il4n4qYDf0bZDY1fNxmlJjhe\n"
                                "shJFKykIw2oPVAGtQZ2rpIyQsupZWuwyHd9XHevFKh5N2qM9YSYr75ZOB8C25zyF\n"
                                "QMeg2/xTARMko0RMqGvVRQVG98VpDs+hXYNZcl49VBd/6OlGRKyr1wMYg1PU0KGl\n"
                                "z0EM3waogjWPb8IRG0GknFxKJMbhtjwmbZg5+b5S+DO7bJg8hDIuoQ==\n"
                                "-----END CERTIFICATE-----\n";

static void test_clock_policy(void)
{
    ktest_begin("tls: a machine of 1970 refuses the dates of a certificate");

    /* A clock that was never set is the seconds since the boot, 1970. */
    KT_CHECK(!net_tls_clock_ok(0, false));
    KT_CHECK(!net_tls_clock_ok(TLS_TIME_MIN_UNIX, false));
    KT_CHECK(!net_tls_clock_ok(1763000000u, false));

    /* A clock that was set to 1970 is the same lie with a signature. */
    KT_CHECK(!net_tls_clock_ok(0, true));
    KT_CHECK(!net_tls_clock_ok(TLS_TIME_MIN_UNIX - 1u, true));

    /* The first moment that can check a date, and a moment of today. */
    KT_CHECK(net_tls_clock_ok(TLS_TIME_MIN_UNIX, true));
    KT_CHECK(net_tls_clock_ok(1763000000u, true));
}

static void test_roots(void)
{
    char two[sizeof(s_test_ca) * 2];
    k_size_t certificates = 0;
    k_size_t length = strlen(s_test_ca);

    ktest_begin("tls: the store of roots is PEM, one certificate or many");

    KT_CHECK_INT_EQ(net_tls_ca_count(s_test_ca, length, &certificates), TLS_OK);
    KT_CHECK_INT_EQ(certificates, 1);

    /* A store is a concatenation of certificates, not one certificate. */
    memcpy(two, s_test_ca, length);
    memcpy(two + length, s_test_ca, length);
    certificates = 0;
    KT_CHECK_INT_EQ(net_tls_ca_count(two, length * 2, &certificates), TLS_OK);
    KT_CHECK_INT_EQ(certificates, 2);

    /* The count is not asked for when the caller wants only the answer. */
    KT_CHECK_INT_EQ(net_tls_ca_count(s_test_ca, length, NULL), TLS_OK);

    /* An empty store trusts nobody and is an error, not an empty success. */
    KT_CHECK_INT_EQ(net_tls_ca_count("", 0, NULL), TLS_ERR_ROOTS);
    KT_CHECK_INT_EQ(net_tls_ca_count(NULL, 0, NULL), TLS_ERR_ROOTS);
    KT_CHECK_INT_EQ(net_tls_ca_count("not a certificate\n", 18, NULL), TLS_ERR_ROOTS);

    /* A PEM that is cut in the middle is a broken store. */
    KT_CHECK_INT_EQ(net_tls_ca_count(s_test_ca, length / 2, NULL), TLS_ERR_ROOTS);
}

static void test_transport(void)
{
    const struct http_transport *tls = net_tls_transport();
    struct http_transport secure;
    void *ctx = NULL;

    ktest_begin("tls: the layer is a transport over another transport");

    KT_CHECK(tls != NULL);
    if (tls == NULL) {
        return;
    }

    KT_CHECK(tls->open != NULL);
    KT_CHECK(tls->send != NULL);
    KT_CHECK(tls->recv != NULL);
    KT_CHECK(tls->close != NULL);
    KT_CHECK(tls->user == NULL);

    /*
     * A layer without a transport under it is a failure with a sentence:
     * this is the shape of every error of the file, and the console prints
     * net_tls_last_error() instead of a bare code.
     */
    secure = *tls;
    KT_CHECK(secure.open(&secure, "example.com", 443, &ctx) != 0);
    KT_CHECK(ctx == NULL);
    KT_CHECK(strlen(net_tls_last_error()) > 0);
}

void test_tls(void)
{
    test_clock_policy();
    test_roots();
    test_transport();
}
