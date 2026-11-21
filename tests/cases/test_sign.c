/*
 * Micro OS — the signature of an image (ADR 0020).
 *
 * The vectors are the recorded ones of RFC 8032 and FIPS 180-4, not a value
 * this implementation produced: a typo in a constant or in a shift would
 * still agree with itself and would be caught only by the world's vectors.
 * The rest of the case is the fate of an image that is damaged in each of
 * the ways a boot has to refuse.
 */

#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "ed25519.h"
#include "sha256.h"
#include "sign_image.h"

static const unsigned char RFC8032_SEED[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
};

static const unsigned char RFC8032_PUBLIC[32] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

static const unsigned char RFC8032_SIGNATURE[64] = {
    0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
    0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
    0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
    0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b,
};

static const unsigned char SHA256_ABC[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

/* A payload with every byte different: a signature that ignored the bytes
   after the first block would still fail on this one. */
#define IMAGE_BYTES 1000u

static void make_image(const unsigned char seed[32], unsigned char *image, size_t *size)
{
    unsigned char public_key[MICROOS_ED25519_KEY_SIZE];
    unsigned char secret_key[MICROOS_ED25519_SECRET_SIZE];
    unsigned char digest[MICROOS_SHA256_SIZE];
    size_t i;

    for (i = 0; i < IMAGE_BYTES; i++) {
        image[i] = (unsigned char)(i * 7u + 3u);
    }

    microos_ed25519_keypair(public_key, secret_key, seed);
    microos_sha256(image, IMAGE_BYTES, digest);
    microos_sign_trailer_make(image + IMAGE_BYTES, IMAGE_BYTES, digest);
    KT_CHECK_INT_EQ(microos_sign_trailer_sign(image + IMAGE_BYTES, secret_key), 0);
    *size = IMAGE_BYTES + MICROOS_SIGN_TRAILER_SIZE;
}

void test_sign(void)
{
    unsigned char image[IMAGE_BYTES + MICROOS_SIGN_TRAILER_SIZE];
    unsigned char seed[32];
    unsigned char public_key[MICROOS_ED25519_KEY_SIZE];
    unsigned char secret_key[MICROOS_ED25519_SECRET_SIZE];
    unsigned char foreign_public[MICROOS_ED25519_KEY_SIZE];
    unsigned char foreign_secret[MICROOS_ED25519_SECRET_SIZE];
    unsigned char digest_abc[MICROOS_SHA256_SIZE];
    unsigned char signature[MICROOS_ED25519_SIGNATURE_SIZE];
    unsigned char damaged[sizeof(image)];
    size_t size = 0;
    const char *why = "not set";
    unsigned i;

    ktest_begin("sign: RFC 8032 and FIPS 180-4 vectors, then a damaged image");

    /* The hash of the three letters is the first vector of FIPS 180-4. */
    microos_sha256((const unsigned char *)"abc", 3, digest_abc);
    KT_CHECK(memcmp(digest_abc, SHA256_ABC, sizeof(SHA256_ABC)) == 0);

    /* The empty message of RFC 8032, section 7.1. */
    microos_ed25519_keypair(public_key, secret_key, RFC8032_SEED);
    KT_CHECK(memcmp(public_key, RFC8032_PUBLIC, sizeof(RFC8032_PUBLIC)) == 0);
    microos_ed25519_sign(signature, (const unsigned char *)"", 0, secret_key);
    KT_CHECK(memcmp(signature, RFC8032_SIGNATURE, sizeof(RFC8032_SIGNATURE)) == 0);
    KT_CHECK_INT_EQ(
        microos_ed25519_verify(RFC8032_SIGNATURE, (const unsigned char *)"", 0, RFC8032_PUBLIC), 0);

    /* A key that did not sign the message. */
    memset(seed, 0x11, sizeof(seed));
    microos_ed25519_keypair(foreign_public, foreign_secret, seed);

    /* The same signature over another byte and under another key is not a
       signature of either. */
    KT_CHECK_INT_EQ(
        microos_ed25519_verify(RFC8032_SIGNATURE, (const unsigned char *)"a", 1, RFC8032_PUBLIC),
        -1);
    KT_CHECK_INT_EQ(microos_ed25519_verify(signature, (const unsigned char *)"", 0, foreign_public),
                    -1);

    for (i = 0; i < sizeof(seed); i++) {
        seed[i] = (unsigned char)(0x40u + i);
    }
    make_image(seed, image, &size);
    KT_CHECK_INT_EQ(size, IMAGE_BYTES + MICROOS_SIGN_TRAILER_SIZE);

    microos_ed25519_keypair(public_key, secret_key, seed);
    KT_CHECK_INT_EQ(microos_sign_image_verify(image, size, public_key, &why), 0);
    KT_CHECK(why == NULL);

    /* One byte of the payload: the hash refuses before the curve is asked. */
    memcpy(damaged, image, size);
    damaged[17] ^= 0x40u;
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(damaged, size, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the checksum does not match");

    /* One byte of the signature: the hash is good and the curve refuses. */
    memcpy(damaged, image, size);
    damaged[size - 1] ^= 0x01u;
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(damaged, size, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the signature does not match");

    /* A key that did not sign this image. */
    memset(seed, 0x11, sizeof(seed));
    microos_ed25519_keypair(foreign_public, foreign_secret, seed);
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(image, size, foreign_public, &why), -1);
    KT_CHECK_STR_EQ(why, "the signature does not match");

    /* A trailer that is one byte short, or one that is not there at all. */
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(image, size - 1, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the image has no signature");
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(image, 32, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the image is too short for a signature");

    /* The size of the payload in the trailer belongs to another image. */
    memcpy(damaged, image, size);
    damaged[size - MICROOS_SIGN_TRAILER_SIZE + 16] ^= 0x01u;
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(damaged, size, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the signature belongs to another image");

    /* The magic of the trailer is not the one this boot knows. */
    memcpy(damaged, image, size);
    damaged[size - MICROOS_SIGN_TRAILER_SIZE] = 'X';
    why = NULL;
    KT_CHECK_INT_EQ(microos_sign_image_verify(damaged, size, public_key, &why), -1);
    KT_CHECK_STR_EQ(why, "the image has no signature");

    KT_CHECK(microos_sign_image_verify(image, size - MICROOS_SIGN_TRAILER_SIZE, public_key, &why) !=
             0);
}
