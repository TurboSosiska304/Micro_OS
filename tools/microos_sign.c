/*
 * Micro OS — sign, verify and inspect an image (ADR 0020).
 *
 * A build hands this tool a flat binary and a secret key; the tool appends the
 * 120-byte trailer of ports/port_x86_64/loader/sign_image.h, which the boot
 * loader of the x86-64 machine verifies before it copies a byte of the
 * kernel. The same file is the reference implementation the tests call, so
 * the tool and the boot share the parser and the arithmetic rather than a
 * description of them.
 *
 *   microos_sign keygen --key FILE --pub FILE [--header FILE]
 *   microos_sign pubkey --key FILE [--pub FILE] [--header FILE]
 *   microos_sign sign   --key FILE [--in FILE] [--out FILE] [--sig FILE]
 *   microos_sign verify --pub FILE [--trailer FILE] FILE
 *   microos_sign info   FILE
 *
 * A key is text: 64 bytes of a secret and 32 bytes of a public key, written
 * as hex. The directory of the keys is a choice of the person who builds a
 * release; the tree keeps public keys only, and a private key belongs in a
 * path outside the repository or in a variable of the environment that names
 * one.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ed25519.h"
#include "sha256.h"
#include "sign_image.h"

#define EXIT_USAGE 2

static void usage(const char *program)
{
    fprintf(stderr,
            "usage:\n"
            "  %s keygen --key FILE --pub FILE [--header FILE]\n"
            "  %s pubkey --key FILE [--pub FILE] [--header FILE]\n"
            "  %s sign   --key FILE [--in FILE] [--out FILE] [--sig FILE]\n"
            "  %s verify --pub FILE [--trailer FILE] FILE\n"
            "  %s info   FILE\n",
            program, program, program, program, program);
}

static int read_file(const char *path, unsigned char **data, size_t *size)
{
    FILE *file;
    long length;
    unsigned char *buffer;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "microos_sign: %s: cannot seek\n", path);
        fclose(file);
        return -1;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "microos_sign: %s: cannot measure\n", path);
        fclose(file);
        return -1;
    }

    buffer = (unsigned char *)malloc((size_t)length + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "microos_sign: out of memory\n");
        fclose(file);
        return -1;
    }
    got = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        fprintf(stderr, "microos_sign: %s: short read\n", path);
        free(buffer);
        return -1;
    }

    *data = buffer;
    *size = got;

    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (size != 0 && fwrite(data, 1, size, file) != size) {
        fprintf(stderr, "microos_sign: %s: short write\n", path);
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

/* A key file is hex with any whitespace between the digits. */
static int read_hex(const char *path, unsigned char *out, size_t expected)
{
    unsigned char *text = NULL;
    size_t length = 0;
    size_t i;
    size_t used = 0;
    int high = -1;

    if (read_file(path, &text, &length) != 0) {
        return -1;
    }

    for (i = 0; i < length; i++) {
        char character = (char)text[i];
        int value = hex_value(character);

        if (value < 0) {
            if (character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                continue;
            }
            fprintf(stderr, "microos_sign: %s: not a hex key\n", path);
            free(text);
            return -1;
        }
        if (high < 0) {
            high = value;
        } else {
            if (used >= expected) {
                fprintf(stderr, "microos_sign: %s: the key is too long\n", path);
                free(text);
                return -1;
            }
            out[used++] = (unsigned char)((high << 4) | value);
            high = -1;
        }
    }

    free(text);

    if (used != expected || high >= 0) {
        fprintf(stderr, "microos_sign: %s: the key must be %zu bytes of hex\n", path, expected);
        return -1;
    }

    return 0;
}

static void write_hex(FILE *file, const unsigned char *data, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        fprintf(file, "%02x", data[i]);
        if ((i + 1) % 32 == 0 && i + 1 != size) {
            fputc('\n', file);
        }
    }
    fputc('\n', file);
}

static int write_hex_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }
    write_hex(file, data, size);
    if (fclose(file) != 0) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int write_public_header(const char *path, const unsigned char public_key[32])
{
    FILE *file = fopen(path, "w");
    size_t i;

    if (file == NULL) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(file, "/* Generated by microos_sign: the public key of the boot loader.\n"
                  " * The private half is not here and is not in this tree. */\n"
                  "#ifndef MICRO_OS_PORT_X86_64_LOADER_SIGN_PUBLIC_KEY_H\n"
                  "#define MICRO_OS_PORT_X86_64_LOADER_SIGN_PUBLIC_KEY_H\n\n"
                  "static const unsigned char loader_public_key[32] = {\n");
    for (i = 0; i < 32; i++) {
        fprintf(file, "    0x%02x%s", public_key[i], i == 31 ? "" : ",");
        if ((i + 1) % 8 == 0) {
            fputc('\n', file);
        } else {
            fputc(' ', file);
        }
    }
    fprintf(file, "};\n\n"
                  "#endif /* MICRO_OS_PORT_X86_64_LOADER_SIGN_PUBLIC_KEY_H */\n");

    if (fclose(file) != 0) {
        fprintf(stderr, "microos_sign: %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int random_seed(unsigned char seed[32])
{
    FILE *file = fopen("/dev/urandom", "rb");
    size_t got;

    if (file == NULL) {
        fprintf(stderr, "microos_sign: /dev/urandom: %s\n", strerror(errno));
        return -1;
    }
    got = fread(seed, 1, 32, file);
    fclose(file);
    if (got != 32) {
        fprintf(stderr, "microos_sign: /dev/urandom: short read\n");
        return -1;
    }

    return 0;
}

static int derive_public(const unsigned char secret[64], unsigned char public_key[32])
{
    unsigned char candidate[64];

    /* The output of a key pair is the seed followed by its public key, the
       layout of NaCl. Derive it from the seed and compare the two halves. */
    microos_ed25519_keypair(public_key, candidate, secret);
    if (memcmp(candidate + 32, secret + 32, 32) != 0) {
        return -1;
    }

    return 0;
}

static int command_keygen(int argc, char **argv)
{
    const char *key_path = NULL;
    const char *pub_path = NULL;
    const char *header_path = NULL;
    unsigned char seed[32];
    unsigned char public_key[32];
    unsigned char secret[64];
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else if (strcmp(argv[i], "--pub") == 0 && i + 1 < argc) {
            pub_path = argv[++i];
        } else if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header_path = argv[++i];
        } else {
            return EXIT_USAGE;
        }
    }
    if (key_path == NULL || pub_path == NULL) {
        return EXIT_USAGE;
    }
    if (random_seed(seed) != 0) {
        return 1;
    }

    microos_ed25519_keypair(public_key, secret, seed);

    if (write_hex_file(key_path, secret, sizeof(secret)) != 0 ||
        write_hex_file(pub_path, public_key, sizeof(public_key)) != 0 ||
        (header_path != NULL && write_public_header(header_path, public_key) != 0)) {
        return 1;
    }

    return 0;
}

static int command_pubkey(int argc, char **argv)
{
    const char *key_path = NULL;
    const char *pub_path = NULL;
    const char *header_path = NULL;
    unsigned char secret[64];
    unsigned char public_key[32];
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else if (strcmp(argv[i], "--pub") == 0 && i + 1 < argc) {
            pub_path = argv[++i];
        } else if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header_path = argv[++i];
        } else {
            return EXIT_USAGE;
        }
    }
    if (key_path == NULL || (pub_path == NULL && header_path == NULL)) {
        return EXIT_USAGE;
    }
    if (read_hex(key_path, secret, sizeof(secret)) != 0) {
        return 1;
    }
    if (derive_public(secret, public_key) != 0) {
        fprintf(stderr, "microos_sign: %s: the secret does not match its public half\n", key_path);
        return 1;
    }

    if ((pub_path != NULL && write_hex_file(pub_path, public_key, sizeof(public_key)) != 0) ||
        (header_path != NULL && write_public_header(header_path, public_key) != 0)) {
        return 1;
    }

    return 0;
}

/*
 * A payload that already ends with a valid trailer is signed again without
 * the old one: a build directory that was signed once is not an image with
 * two signatures. The old signature has no value for this key anyway.
 */
static size_t strip_trailer(const unsigned char *payload, size_t size)
{
    uint64_t old_size = 0;

    if (size < MICROOS_SIGN_TRAILER_SIZE) {
        return size;
    }
    if (microos_sign_trailer_read(payload + size - MICROOS_SIGN_TRAILER_SIZE, &old_size, NULL) ==
            0 &&
        old_size == (uint64_t)(size - MICROOS_SIGN_TRAILER_SIZE)) {
        return size - MICROOS_SIGN_TRAILER_SIZE;
    }

    return size;
}

static int command_sign(int argc, char **argv)
{
    const char *key_path = NULL;
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *sig_path = NULL;
    unsigned char secret[64];
    unsigned char *payload = NULL;
    unsigned char *signed_image = NULL;
    size_t size = 0;
    size_t payload_size;
    unsigned char digest[MICROOS_SIGN_DIGEST_SIZE];
    unsigned char trailer[MICROOS_SIGN_TRAILER_SIZE];
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
            in_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
            sig_path = argv[++i];
        } else {
            return EXIT_USAGE;
        }
    }
    if (key_path == NULL || in_path == NULL) {
        return EXIT_USAGE;
    }
    if (out_path == NULL) {
        out_path = in_path;
    }
    if (read_hex(key_path, secret, sizeof(secret)) != 0) {
        return 1;
    }
    if (read_file(in_path, &payload, &size) != 0) {
        return 1;
    }

    payload_size = strip_trailer(payload, size);
    if (payload_size != size) {
        fprintf(stderr, "microos_sign: %s: the old signature is replaced\n", in_path);
    }
    if (payload_size > (size_t)-1 - MICROOS_SIGN_TRAILER_SIZE) {
        fprintf(stderr, "microos_sign: the image is too large\n");
        free(payload);
        return 1;
    }

    signed_image = (unsigned char *)malloc(payload_size + MICROOS_SIGN_TRAILER_SIZE);
    if (signed_image == NULL) {
        fprintf(stderr, "microos_sign: out of memory\n");
        free(payload);
        return 1;
    }

    memcpy(signed_image, payload, payload_size);
    microos_sha256(signed_image, payload_size, digest);
    microos_sign_trailer_make(trailer, (uint64_t)payload_size, digest);
    if (microos_sign_trailer_sign(trailer, secret) != 0) {
        fprintf(stderr, "microos_sign: the signature failed\n");
        free(payload);
        free(signed_image);
        return 1;
    }
    memcpy(signed_image + payload_size, trailer, MICROOS_SIGN_TRAILER_SIZE);

    if (write_file(out_path, signed_image, payload_size + MICROOS_SIGN_TRAILER_SIZE) != 0 ||
        (sig_path != NULL && write_file(sig_path, trailer, MICROOS_SIGN_TRAILER_SIZE) != 0)) {
        free(payload);
        free(signed_image);
        return 1;
    }

    free(payload);
    free(signed_image);

    return 0;
}

static int command_verify(int argc, char **argv)
{
    const char *pub_path = NULL;
    const char *trailer_path = NULL;
    const char *image_path = NULL;
    unsigned char public_key[MICROOS_SIGN_KEY_SIZE];
    unsigned char *image = NULL;
    unsigned char *trailer = NULL;
    unsigned char *combined = NULL;
    size_t image_size = 0;
    size_t trailer_size = 0;
    const char *why = NULL;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pub") == 0 && i + 1 < argc) {
            pub_path = argv[++i];
        } else if (strcmp(argv[i], "--trailer") == 0 && i + 1 < argc) {
            trailer_path = argv[++i];
        } else if (image_path == NULL) {
            image_path = argv[i];
        } else {
            return EXIT_USAGE;
        }
    }
    if (pub_path == NULL || image_path == NULL) {
        return EXIT_USAGE;
    }
    if (read_hex(pub_path, public_key, sizeof(public_key)) != 0 ||
        read_file(image_path, &image, &image_size) != 0) {
        return 1;
    }

    if (trailer_path != NULL) {
        if (read_file(trailer_path, &trailer, &trailer_size) != 0) {
            free(image);
            return 1;
        }
        if (trailer_size != MICROOS_SIGN_TRAILER_SIZE) {
            fprintf(stderr, "microos_sign: %s: the trailer must be %d bytes\n", trailer_path,
                    MICROOS_SIGN_TRAILER_SIZE);
            free(image);
            free(trailer);
            return 1;
        }
        combined = (unsigned char *)malloc(image_size + MICROOS_SIGN_TRAILER_SIZE);
        if (combined == NULL) {
            fprintf(stderr, "microos_sign: out of memory\n");
            free(image);
            free(trailer);
            return 1;
        }
        memcpy(combined, image, image_size);
        memcpy(combined + image_size, trailer, MICROOS_SIGN_TRAILER_SIZE);
        free(image);
        free(trailer);
        image = combined;
        image_size += MICROOS_SIGN_TRAILER_SIZE;
    }

    if (microos_sign_image_verify(image, image_size, public_key, &why) != 0) {
        fprintf(stderr, "refused: %s: %s\n", image_path, why != NULL ? why : "unknown reason");
        free(image);
        return 1;
    }

    printf("ok: %s: %zu bytes, sha-256 ", image_path, image_size - MICROOS_SIGN_TRAILER_SIZE);
    {
        unsigned char digest[MICROOS_SIGN_DIGEST_SIZE];
        size_t j;

        microos_sha256(image, image_size - MICROOS_SIGN_TRAILER_SIZE, digest);
        for (j = 0; j < sizeof(digest); j++) {
            printf("%02x", digest[j]);
        }
    }
    printf("\n");

    free(image);

    return 0;
}

static int command_info(int argc, char **argv)
{
    const char *image_path;
    unsigned char *image = NULL;
    size_t image_size = 0;
    const unsigned char *trailer;
    uint64_t payload_size = 0;
    unsigned char digest[MICROOS_SIGN_DIGEST_SIZE];
    size_t i;

    if (argc != 1) {
        return EXIT_USAGE;
    }
    image_path = argv[0];

    if (read_file(image_path, &image, &image_size) != 0) {
        return 1;
    }
    if (image_size < MICROOS_SIGN_TRAILER_SIZE) {
        fprintf(stderr, "microos_sign: %s: no signature trailer\n", image_path);
        free(image);
        return 1;
    }
    trailer = image + image_size - MICROOS_SIGN_TRAILER_SIZE;
    if (microos_sign_trailer_read(trailer, &payload_size, digest) != 0 ||
        payload_size != (uint64_t)(image_size - MICROOS_SIGN_TRAILER_SIZE)) {
        fprintf(stderr, "microos_sign: %s: no signature trailer\n", image_path);
        free(image);
        return 1;
    }

    printf("image:    %s\n", image_path);
    printf("payload:  %llu bytes\n", (unsigned long long)payload_size);
    printf("sha-256:  ");
    for (i = 0; i < sizeof(digest); i++) {
        printf("%02x", digest[i]);
    }
    printf("\nsignature: ");
    for (i = 0; i < MICROOS_ED25519_SIGNATURE_SIZE; i++) {
        printf("%02x", trailer[MICROOS_SIGN_HEADER_SIZE + i]);
    }
    printf("\n");

    free(image);

    return 0;
}

int main(int argc, char **argv)
{
    const char *command;

    if (argc < 2) {
        usage(argv[0]);
        return EXIT_USAGE;
    }
    command = argv[1];

    if (strcmp(command, "keygen") == 0) {
        int result = command_keygen(argc - 2, argv + 2);

        if (result == EXIT_USAGE) {
            usage(argv[0]);
        }
        return result;
    }
    if (strcmp(command, "pubkey") == 0) {
        int result = command_pubkey(argc - 2, argv + 2);

        if (result == EXIT_USAGE) {
            usage(argv[0]);
        }
        return result;
    }
    if (strcmp(command, "sign") == 0) {
        int result = command_sign(argc - 2, argv + 2);

        if (result == EXIT_USAGE) {
            usage(argv[0]);
        }
        return result;
    }
    if (strcmp(command, "verify") == 0) {
        int result = command_verify(argc - 2, argv + 2);

        if (result == EXIT_USAGE) {
            usage(argv[0]);
        }
        return result;
    }
    if (strcmp(command, "info") == 0) {
        int result = command_info(argc - 2, argv + 2);

        if (result == EXIT_USAGE) {
            usage(argv[0]);
        }
        return result;
    }

    usage(argv[0]);
    return EXIT_USAGE;
}
