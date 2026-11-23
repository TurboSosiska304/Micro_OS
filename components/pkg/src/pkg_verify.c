/*
 * Micro OS — the trust of an install (ADR 0024 §4).
 *
 * The wire is not trusted and the mirror is not trusted: a package is a
 * file whose digest and signature are the only reasons to put a byte of it
 * in /usr. The same two primitives as the signed image of ADR 0020 are used
 * here - the SHA-256 and the Ed25519 of ports/port_x86_64/loader - so a
 * broken arithmetic is a broken boot as well, and one repair fixes both.
 *
 * The key of a repository is a file of /etc/pkg/keys, thirty-two bytes in
 * hexadecimal. When the file is not there, the key of the image is the key
 * of the repository: the release signs the kernel and the index with the
 * same key, and a machine that boots an image of the project trusts the
 * programs of the project without a second file to copy.
 */
#include <stdio.h>
#include <string.h>

#include "ed25519.h"
#include "kernel/exec.h"
#include "kernel/vfs.h"
#include "pkg_private.h"
#include "sha256.h"

/* A build that does not name the key of the image has no built-in key: the
 * tests bring theirs in /etc/pkg/keys, and the host has no image to trust. */
#ifndef MICRO_OS_PKG_BUILTIN_KEY
#define MICRO_OS_PKG_BUILTIN_KEY 0
#endif

#if MICRO_OS_PKG_BUILTIN_KEY
#include "sign_public_key.h"
#endif

void pkg_sha256_hex(const void *data, k_size_t size, char hex[PKG_SHA256_HEX])
{
    static const char digits[] = "0123456789abcdef";
    k_u8_t digest[MICROOS_SHA256_SIZE];
    k_size_t i;

    microos_sha256((const uint8_t *)data, (size_t)size, digest);

    for (i = 0; i < MICROOS_SHA256_SIZE; i++) {
        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0Fu];
    }

    hex[PKG_SHA256_HEX - 1u] = '\0';
}

int pkg_signature_check(const k_u8_t key[PKG_KEY_SIZE], const void *data, k_size_t size,
                        const k_u8_t signature[PKG_SIGNATURE_SIZE])
{
    if (key == NULL || data == NULL || signature == NULL) {
        return PKG_ESIGN;
    }

    if (microos_ed25519_verify((const uint8_t *)signature, (const uint8_t *)data, (size_t)size,
                               (const uint8_t *)key) != 0) {
        return PKG_ESIGN;
    }

    return PKG_OK;
}

static int pkg_builtin_key(const k_u8_t **key)
{
#if MICRO_OS_PKG_BUILTIN_KEY
    *key = loader_public_key;
    return PKG_OK;
#else
    *key = NULL;
    return PKG_ENOENT;
#endif
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

int pkg_key_file(const char *repo, k_u8_t key[PKG_KEY_SIZE], bool *found)
{
    char path[VFS_PATH_MAX];
    char text[PKG_KEY_SIZE * 2u + 8u];
    k_size_t got = 0;
    k_size_t i;
    int n;

    if (found != NULL) {
        *found = false;
    }

    if (repo == NULL || key == NULL) {
        return PKG_ESIGN;
    }

    n = snprintf(path, sizeof(path), "%s/%s.pub", PKG_KEY_DIR, repo);
    if (n < 0 || (k_size_t)n >= sizeof(path)) {
        return PKG_ESIGN;
    }

    if (pkg_read(path, text, sizeof(text) - 1u, &got) != PKG_OK) {
        return PKG_ENOENT;
    }

    text[got] = '\0';

    for (i = 0; i < PKG_KEY_SIZE; i++) {
        int high = hex_value(text[i * 2u]);
        int low = (text[i * 2u] != '\0') ? hex_value(text[i * 2u + 1u]) : -1;

        if (high < 0 || low < 0) {
            return PKG_ESIGN;
        }

        key[i] = (k_u8_t)((high << 4) | low);
    }

    if (found != NULL) {
        *found = true;
    }

    return PKG_OK;
}

const k_u8_t *pkg_repository_key(const char *repo)
{
    static k_u8_t key[PKG_KEY_SIZE];
    bool found = false;
    const k_u8_t *builtin = NULL;

    if (pkg_key_file(repo, key, &found) == PKG_OK && found) {
        return key;
    }

    if (pkg_builtin_key(&builtin) == PKG_OK) {
        return builtin;
    }

    return NULL;
}

int pkg_signature_load(const char *path, k_u8_t signature[PKG_SIGNATURE_SIZE])
{
    k_size_t got = 0;

    if (path == NULL || signature == NULL) {
        return PKG_EIO;
    }

    if (pkg_read(path, signature, PKG_SIGNATURE_SIZE, &got) != PKG_OK) {
        return PKG_ENOENT;
    }

    return (got == PKG_SIGNATURE_SIZE) ? PKG_OK : PKG_ESIGN;
}

int pkg_size_check(k_u64_t size)
{
    return (size <= (k_u64_t)PKG_PACKAGE_MAX) ? PKG_OK : PKG_ESIZE;
}

int pkg_package_check(const struct pkg_entry *entry, const k_u8_t signature[PKG_SIGNATURE_SIZE],
                      const k_u8_t key[PKG_KEY_SIZE], const void *data, k_size_t size)
{
    struct k_exec_image image;
    char digest[PKG_SHA256_HEX];
    k_size_t i;
    int rc;

    if (entry == NULL || data == NULL || key == NULL || signature == NULL) {
        return PKG_ESIGN;
    }

    if ((k_u64_t)size != entry->size) {
        return PKG_ESIZE;
    }

    pkg_sha256_hex(data, size, digest);

    for (i = 0; i < PKG_SHA256_HEX - 1u; i++) {
        char a = digest[i];
        char b = entry->sha256[i];

        if (a >= 'A' && a <= 'F') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'F') {
            b = (char)(b - 'A' + 'a');
        }

        if (a != b) {
            return PKG_ESIGN;
        }
    }

    if (entry->sha256[PKG_SHA256_HEX - 1u] != '\0') {
        return PKG_ESIGN;
    }

    rc = pkg_signature_check(key, data, size, signature);
    if (rc != PKG_OK) {
        return rc;
    }

    /* The signature says the bytes are the ones of the repository; the
     * header says the bytes are a program of this machine (ADR 0018). */
    if (k_exec_parse(data, size, &image) != 0) {
        return PKG_ESIGN;
    }

    if (image.arch != K_EXEC_ARCH_X86_64) {
        return PKG_EMACHINE;
    }

    return PKG_OK;
}
