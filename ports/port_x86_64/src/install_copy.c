/*
 * Micro OS — the copy of the system and the manifest that checks it.
 *
 * The source is what the running machine really has: the image of the kernel
 * in boot module 0, the programs of /bin and the defaults of /etc. Nothing is
 * invented here and no host file is read: the same VFS calls copy from the
 * live root and write to the volume mounted below the temporary point.
 *
 * The manifest is written as the copy goes, so the hash of a file is the hash
 * of the bytes that were written and not of a second read of the source. The
 * hash of the manifest itself is kept beside it; a check that cannot check the
 * list it reads would be a check of nothing.
 */
#include "port_x86_64_install_copy.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "sha256.h"

#include "port_x86_64.h"

/* A kilobyte is the block of both volumes; half of it keeps the stack of the
 * shell, which runs this command, well under its thirty-two kilobytes. */
#define COPY_CHUNK 512u
#define COPY_DIR_BATCH 8u
#define COPY_DEPTH_MAX 5u
#define COPY_LINE_MAX 256u
#define COPY_MANIFEST_MAX (64u * 1024u)

static bool streq(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static bool is_dot(const char *name)
{
    return streq(name, ".") || streq(name, "..");
}

/* The files L3 wrote and the two of the manifest: the defaults of /etc must
 * not replace the record of the install that made this volume. */
static bool is_kept(const char *path)
{
    return streq(path, "/etc/install") || streq(path, "/etc/volumes") ||
           streq(path, INSTALL_MANIFEST_PATH) || streq(path, INSTALL_MANIFEST_HASH_PATH);
}

static int write_all(struct vfs_file *file, const char *data, k_size_t size)
{
    k_size_t at = 0u;

    while (at < size) {
        int written = vfs_write(file, data + at, size - at);

        if (written <= 0) {
            return written == 0 ? KERR_IO : written;
        }
        at += (k_size_t)written;
    }

    return 0;
}

static void digest_hex(const k_u8_t digest[MICROOS_SHA256_SIZE],
                       char hex[2u * MICROOS_SHA256_SIZE + 1u])
{
    static const char digits[] = "0123456789abcdef";
    k_size_t i;

    for (i = 0u; i < MICROOS_SHA256_SIZE; i++) {
        hex[i * 2u] = digits[digest[i] >> 4];
        hex[i * 2u + 1u] = digits[digest[i] & 0x0Fu];
    }
    hex[2u * MICROOS_SHA256_SIZE] = '\0';
}

static bool hex_is(const char *text, k_size_t length)
{
    k_size_t i;

    if (strlen(text) != length) {
        return false;
    }

    for (i = 0u; i < length; i++) {
        char c = text[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

static bool hex_eq(const char *a, const char *b, k_size_t length)
{
    k_size_t i;

    if (strlen(a) != length || strlen(b) != length) {
        return false;
    }

    for (i = 0u; i < length; i++) {
        char x = a[i] >= 'A' && a[i] <= 'F' ? (char)(a[i] + ('a' - 'A')) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'F' ? (char)(b[i] + ('a' - 'A')) : b[i];

        if (x != y) {
            return false;
        }
    }

    return true;
}

/* One line of the manifest: the logical path, the size and the digest. The
 * same bytes go to the file and into the hash of the manifest. */
static int manifest_line(struct vfs_file *manifest, struct microos_sha256_ctx *hash,
                         const char *path, k_u64_t size, const k_u8_t digest[MICROOS_SHA256_SIZE])
{
    char hex[2u * MICROOS_SHA256_SIZE + 1u];
    char line[VFS_PATH_MAX + 96u];
    int length;
    int rc;

    digest_hex(digest, hex);
    length = snprintf(line, sizeof(line), "%s %llu %s\n", path, (unsigned long long)size, hex);
    if (length < 0 || (k_size_t)length >= sizeof(line)) {
        return KERR_NAMETOOLONG;
    }

    rc = write_all(manifest, line, (k_size_t)length);
    if (rc != 0) {
        return rc;
    }

    microos_sha256_update(hash, (const k_u8_t *)line, (size_t)length);
    return 0;
}

/* Close the copy, put its line in the manifest and count it. */
static int finish_copy(struct vfs_file *out, const char *target, const char *logical, k_u64_t size,
                       struct microos_sha256_ctx *file_hash, struct vfs_file *manifest,
                       struct microos_sha256_ctx *manifest_hash, k_u32_t *count)
{
    k_u8_t digest[MICROOS_SHA256_SIZE];
    int rc;

    rc = vfs_fsync(out);
    if (rc != 0 && rc != KERR_NOTSUP) {
        (void)vfs_close(out);
        kshell_printf("install-copy: %s: sync failed (%d)\n", target, rc);
        return rc;
    }

    rc = vfs_close(out);
    if (rc != 0) {
        return rc;
    }

    microos_sha256_final(file_hash, digest);
    rc = manifest_line(manifest, manifest_hash, logical, size, digest);
    if (rc == 0) {
        (*count)++;
    }

    return rc;
}

/* The kernel of boot module 0 is bytes in memory, not a file of the live
 * root. Its digest is of exactly the bytes written to the volume. */
static int copy_memory(const char *target, const k_u8_t *data, k_u64_t size,
                       struct vfs_file *manifest, struct microos_sha256_ctx *manifest_hash,
                       k_u32_t *count)
{
    struct microos_sha256_ctx file_hash;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t buffer[COPY_CHUNK];
    struct vfs_file out;
    k_u64_t at = 0u;
    int rc;

    rc = vfs_open(target, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &out, priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot create %s (%d)\n", target, rc);
        return rc;
    }

    microos_sha256_init(&file_hash);

    while (at < size) {
        k_size_t chunk = size - at > COPY_CHUNK ? COPY_CHUNK : (k_size_t)(size - at);
        int written;

        memcpy(buffer, data + at, chunk);
        written = vfs_write(&out, buffer, chunk);
        if (written <= 0) {
            (void)vfs_close(&out);
            kshell_printf("install-copy: %s: write failed (%d)\n", target, written);
            return written == 0 ? KERR_IO : written;
        }
        microos_sha256_update(&file_hash, buffer, (size_t)written);
        at += (k_u64_t)written;
    }

    return finish_copy(&out, target, INSTALL_KERNEL_PATH, at, &file_hash, manifest, manifest_hash,
                       count);
}

static int copy_file(const char *source, const char *target, struct vfs_file *manifest,
                     struct microos_sha256_ctx *manifest_hash, k_u32_t *count)
{
    struct microos_sha256_ctx file_hash;
    k_u8_t in_priv[VFS_FILE_DATA_SIZE];
    k_u8_t out_priv[VFS_FILE_DATA_SIZE];
    k_u8_t buffer[COPY_CHUNK];
    struct vfs_file in;
    struct vfs_file out;
    k_u64_t total = 0u;
    int rc;

    rc = vfs_open(source, VFS_O_RDONLY, &in, in_priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot read %s (%d)\n", source, rc);
        return rc;
    }

    rc = vfs_open(target, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &out, out_priv);
    if (rc != 0) {
        (void)vfs_close(&in);
        kshell_printf("install-copy: cannot create %s (%d)\n", target, rc);
        return rc;
    }

    microos_sha256_init(&file_hash);

    for (;;) {
        int got = vfs_read(&in, buffer, sizeof(buffer));
        int written;

        if (got < 0) {
            rc = got;
            kshell_printf("install-copy: %s: read failed (%d)\n", source, rc);
            break;
        }
        if (got == 0) {
            break;
        }

        written = vfs_write(&out, buffer, (k_size_t)got);
        if (written <= 0) {
            rc = written == 0 ? KERR_IO : written;
            kshell_printf("install-copy: %s: write failed (%d)\n", target, rc);
            break;
        }
        microos_sha256_update(&file_hash, buffer, (size_t)written);
        total += (k_u64_t)written;
    }

    (void)vfs_close(&in);
    if (rc != 0) {
        (void)vfs_close(&out);
        return rc;
    }

    return finish_copy(&out, target, source, total, &file_hash, manifest, manifest_hash, count);
}

/* A file the installer already wrote on the volume, hashed from the volume
 * and put in the manifest as if it had been copied. */
static int manifest_existing(const char *target, const char *logical, struct vfs_file *manifest,
                             struct microos_sha256_ctx *manifest_hash, k_u32_t *count)
{
    struct microos_sha256_ctx file_hash;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t buffer[COPY_CHUNK];
    struct vfs_file in;
    k_u8_t digest[MICROOS_SHA256_SIZE];
    k_u64_t total = 0u;
    int rc;

    rc = vfs_open(target, VFS_O_RDONLY, &in, priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot read %s (%d)\n", target, rc);
        return rc;
    }

    microos_sha256_init(&file_hash);

    for (;;) {
        int got = vfs_read(&in, buffer, sizeof(buffer));

        if (got < 0) {
            (void)vfs_close(&in);
            kshell_printf("install-copy: %s: read failed (%d)\n", target, got);
            return got;
        }
        if (got == 0) {
            break;
        }
        microos_sha256_update(&file_hash, buffer, (size_t)got);
        total += (k_u64_t)got;
    }

    rc = vfs_close(&in);
    if (rc != 0) {
        return rc;
    }

    microos_sha256_final(&file_hash, digest);
    rc = manifest_line(manifest, manifest_hash, logical, total, digest);
    if (rc == 0) {
        (*count)++;
    }

    return rc;
}

static int apply_mode(const char *source, const char *target)
{
    struct vfs_stat info;
    int rc;

    if (vfs_stat(source, &info) != 0 || info.mode == 0u) {
        return 0;
    }

    rc = vfs_chmod(target, info.mode);
    if (rc != 0) {
        kshell_printf("install-copy: %s: cannot keep the mode %o (%d)\n", target,
                      (unsigned)info.mode, rc);
    }

    return rc;
}

/* Copy one tree of the live root. The entries are read in batches so that a
 * directory of sixty programs fits even on a filesystem with a small list. */
static int copy_directory(const char *point, const char *source, k_u32_t depth,
                          struct vfs_file *manifest, struct microos_sha256_ctx *manifest_hash,
                          k_u32_t *count)
{
    struct vfs_dirent entries[COPY_DIR_BATCH];
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file dir;
    char here[VFS_PATH_MAX];
    int rc;

    if (depth >= COPY_DEPTH_MAX) {
        kshell_printf("install-copy: %s: the tree is deeper than %u level(s)\n", source,
                      (unsigned)COPY_DEPTH_MAX);
        return KERR_TOOBIG;
    }

    if (snprintf(here, sizeof(here), "%s%s", point, source) >= (int)sizeof(here)) {
        return KERR_NAMETOOLONG;
    }

    rc = vfs_open(source, VFS_O_DIRECTORY, &dir, priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot read the directory %s (%d)\n", source, rc);
        return rc;
    }

    for (;;) {
        k_size_t found = 0u;
        k_size_t i;

        rc = vfs_readdir(&dir, entries, COPY_DIR_BATCH, &found);
        if (rc != 0 || found == 0u) {
            break;
        }

        for (i = 0u; i < found; i++) {
            char child_source[VFS_PATH_MAX];
            char child_target[VFS_PATH_MAX];
            const char *name = entries[i].name;

            if (is_dot(name)) {
                continue;
            }

            if (snprintf(child_source, sizeof(child_source), "%s/%s", source, name) >=
                    (int)sizeof(child_source) ||
                snprintf(child_target, sizeof(child_target), "%s/%s", here, name) >=
                    (int)sizeof(child_target)) {
                rc = KERR_NAMETOOLONG;
                break;
            }

            if (entries[i].type == 2u) {
                rc = vfs_mkdir(child_target);
                if (rc == KERR_EXIST) {
                    rc = 0;
                }
                if (rc == 0) {
                    rc = apply_mode(child_source, child_target);
                }
                if (rc == 0) {
                    rc = copy_directory(point, child_source, depth + 1u, manifest, manifest_hash,
                                        count);
                }
            } else if (entries[i].type == 1u && !is_kept(child_source)) {
                rc = copy_file(child_source, child_target, manifest, manifest_hash, count);
                if (rc == 0) {
                    rc = apply_mode(child_source, child_target);
                }
            } else if (is_kept(child_source)) {
                /* The record of the install and the manifest are its own;
                 * neither is a default of /etc to be copied over. */
            } else {
                kshell_printf("install-copy: %s: skipped, not a regular file or directory\n",
                              child_source);
            }

            if (rc != 0) {
                break;
            }
        }
    }

    (void)vfs_close(&dir);
    return rc;
}

static int write_manifest_hash(const char *point, const char *hex)
{
    char path[VFS_PATH_MAX];
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file out;
    char text[2u * MICROOS_SHA256_SIZE + 2u];
    int length;
    int rc;

    if (snprintf(path, sizeof(path), "%s%s", point, INSTALL_MANIFEST_HASH_PATH) >=
        (int)sizeof(path)) {
        return KERR_NAMETOOLONG;
    }

    length = snprintf(text, sizeof(text), "%s\n", hex);
    if (length < 0 || (k_size_t)length >= sizeof(text)) {
        return KERR_NAMETOOLONG;
    }

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &out, priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot create %s (%d)\n", path, rc);
        return rc;
    }

    rc = write_all(&out, text, (k_size_t)length);
    if (rc == 0) {
        rc = vfs_fsync(&out);
    }
    if (rc == KERR_NOTSUP) {
        rc = 0;
    }

    {
        int closed = vfs_close(&out);

        if (rc == 0) {
            rc = closed;
        }
    }

    return rc;
}

int port_x86_64_install_copy(const char *point, k_u32_t *out_files)
{
    static const char header[] = "# Micro OS install manifest v1\n# path size sha256\n";
    struct microos_sha256_ctx manifest_hash;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file manifest;
    k_u8_t digest[MICROOS_SHA256_SIZE];
    char path[VFS_PATH_MAX];
    char hex[2u * MICROOS_SHA256_SIZE + 1u];
    k_u64_t start = 0u;
    k_u64_t size = 0u;
    k_u32_t count = 0u;
    int rc;

    if (point == NULL || out_files == NULL) {
        return KERR_INVAL;
    }

    if (snprintf(path, sizeof(path), "%s%s", point, INSTALL_MANIFEST_PATH) >= (int)sizeof(path)) {
        return KERR_NAMETOOLONG;
    }

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &manifest, priv);
    if (rc != 0) {
        kshell_printf("install-copy: cannot create %s (%d)\n", path, rc);
        return rc;
    }

    microos_sha256_init(&manifest_hash);
    microos_sha256_update(&manifest_hash, (const k_u8_t *)header, sizeof(header) - 1u);
    rc = write_all(&manifest, header, sizeof(header) - 1u);
    if (rc != 0) {
        (void)vfs_close(&manifest);
        return rc;
    }

    if (snprintf(path, sizeof(path), "%s/etc/install", point) >= (int)sizeof(path)) {
        rc = KERR_NAMETOOLONG;
    } else {
        rc = manifest_existing(path, "/etc/install", &manifest, &manifest_hash, &count);
    }
    if (rc == 0) {
        if (snprintf(path, sizeof(path), "%s/etc/volumes", point) >= (int)sizeof(path)) {
            rc = KERR_NAMETOOLONG;
        } else {
            rc = manifest_existing(path, "/etc/volumes", &manifest, &manifest_hash, &count);
        }
    }

    if (rc == 0) {
        if (!port_x86_64_boot_module(0u, &start, &size) || size == 0u) {
            kshell_printf("install-copy: the boot has no kernel module to copy\n");
            rc = KERR_NOENT;
        } else if (size > 64u * 1024u * 1024u) {
            kshell_printf("install-copy: the kernel module is %llu byte(s), too large\n",
                          (unsigned long long)size);
            rc = KERR_TOOBIG;
        } else {
            int length = snprintf(path, sizeof(path), "%s/boot", point);

            if (length < 0 || (k_size_t)length >= sizeof(path)) {
                rc = KERR_NAMETOOLONG;
            } else {
                /* The mount point exists; asking mkdir_p to make it again
                 * would ask the volume to create its own root. */
                rc = vfs_mkdir(path);
                if (rc == KERR_EXIST) {
                    rc = 0;
                }
            }
            if (rc != 0) {
                kshell_printf("install-copy: cannot create /boot (%d)\n", rc);
            } else if (snprintf(path, sizeof(path), "%s%s", point, INSTALL_KERNEL_PATH) >=
                       (int)sizeof(path)) {
                rc = KERR_NAMETOOLONG;
            } else {
                rc = copy_memory(path, (const k_u8_t *)(uintptr_t)start, size, &manifest,
                                 &manifest_hash, &count);
            }
        }
    }

    if (rc == 0) {
        int length = snprintf(path, sizeof(path), "%s/bin", point);

        if (length < 0 || (k_size_t)length >= sizeof(path)) {
            rc = KERR_NAMETOOLONG;
        } else {
            rc = vfs_mkdir(path);
            if (rc == KERR_EXIST) {
                rc = 0;
            }
        }
    }
    if (rc == 0) {
        rc = copy_directory(point, "/bin", 0u, &manifest, &manifest_hash, &count);
    }
    if (rc == 0) {
        rc = copy_directory(point, "/etc", 0u, &manifest, &manifest_hash, &count);
    }

    if (rc == 0) {
        rc = vfs_fsync(&manifest);
        if (rc == KERR_NOTSUP) {
            rc = 0;
        }
    }

    {
        int closed = vfs_close(&manifest);

        if (rc == 0) {
            rc = closed;
        }
    }

    if (rc != 0) {
        *out_files = count;
        return rc;
    }

    microos_sha256_final(&manifest_hash, digest);
    digest_hex(digest, hex);
    rc = write_manifest_hash(point, hex);

    if (rc == 0) {
        kshell_printf("install-copy: copied %u file(s); manifest sha256:%s\n", (unsigned)count,
                      hex);
    }

    *out_files = count;
    return rc;
}

/* --- the read-only check ------------------------------------------------ */

/*
 * The close of an ext2 file settles the volume even when it was only read.
 * A read-only mount of a gated disk cannot settle, so the close reports IO;
 * the bytes of the read are still the bytes the manifest names. The check
 * below is about those bytes, not about the right to write metadata.
 */
static int close_read(struct vfs_file *file)
{
    int rc = vfs_close(file);

    return rc == KERR_IO ? 0 : rc;
}

struct line_reader {
    char buffer[COPY_LINE_MAX];
    k_size_t used;
    k_size_t at;
};

/* 1: a line is in `out`; 0: the file ended; negative: a KERR_* code. */
static int next_line(struct vfs_file *file, struct line_reader *reader, char *out, k_size_t max)
{
    for (;;) {
        k_size_t i;

        for (i = reader->at; i < reader->used; i++) {
            if (reader->buffer[i] == '\n') {
                k_size_t length = i - reader->at;

                if (length >= max) {
                    return KERR_TOOBIG;
                }
                memcpy(out, reader->buffer + reader->at, length);
                out[length] = '\0';
                reader->at = i + 1u;
                if (reader->at == reader->used) {
                    reader->at = 0u;
                    reader->used = 0u;
                }
                return 1;
            }
        }

        if (reader->at != 0u) {
            memmove(reader->buffer, reader->buffer + reader->at, reader->used - reader->at);
            reader->used -= reader->at;
            reader->at = 0u;
        }
        if (reader->used == sizeof(reader->buffer)) {
            return KERR_TOOBIG;
        }

        {
            int got = vfs_read(file, reader->buffer + reader->used,
                               sizeof(reader->buffer) - reader->used);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                if (reader->used == 0u) {
                    return 0;
                }
                if (reader->used >= max) {
                    return KERR_TOOBIG;
                }
                memcpy(out, reader->buffer, reader->used);
                out[reader->used] = '\0';
                reader->used = 0u;
                return 1;
            }
            reader->used += (k_size_t)got;
        }
    }
}

/* The three fields of a manifest line, read by hand: sscanf of the host
 * resolves to a C23 entry point the freestanding kernel does not link. */
static bool parse_line(const char *line, char *path, k_size_t path_size, k_u64_t *out_size,
                       char *want, k_size_t want_size)
{
    const char *p = line;
    k_size_t n = 0u;
    k_u64_t value = 0u;

    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1u >= path_size) {
            return false;
        }
        path[n++] = *p++;
    }
    path[n] = '\0';
    if (*p != ' ') {
        return false;
    }

    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (k_u64_t)(*p - '0');
        p++;
    }
    if (*p != ' ') {
        return false;
    }

    while (*p == ' ') {
        p++;
    }
    n = 0u;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r') {
        if (n + 1u >= want_size) {
            return false;
        }
        want[n++] = *p++;
    }
    want[n] = '\0';

    while (*p == ' ' || *p == '\r') {
        p++;
    }

    if (*p != '\0' || path[0] != '/' || !hex_is(want, 2u * MICROOS_SHA256_SIZE)) {
        return false;
    }

    *out_size = value;
    return true;
}

static int hash_file(const char *path, k_u8_t digest[MICROOS_SHA256_SIZE], k_u64_t *out_size)
{
    struct microos_sha256_ctx hash;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t buffer[COPY_CHUNK];
    struct vfs_file file;
    k_u64_t total = 0u;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return rc;
    }

    microos_sha256_init(&hash);

    for (;;) {
        int got = vfs_read(&file, buffer, sizeof(buffer));

        if (got < 0) {
            rc = got;
            break;
        }
        if (got == 0) {
            break;
        }
        microos_sha256_update(&hash, buffer, (size_t)got);
        total += (k_u64_t)got;
    }

    {
        int closed = close_read(&file);

        if (rc == 0) {
            rc = closed;
        }
    }

    if (rc == 0) {
        microos_sha256_final(&hash, digest);
        *out_size = total;
    }

    return rc;
}

static int read_manifest_hash(const char *path, char hex[2u * MICROOS_SHA256_SIZE + 1u])
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    char text[2u * MICROOS_SHA256_SIZE + 2u];
    int got;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return rc;
    }

    got = vfs_read(&file, text, sizeof(text) - 1u);
    rc = close_read(&file);
    if (got < 0) {
        return got;
    }
    if (rc != 0) {
        return rc;
    }
    if ((k_size_t)got < 2u * MICROOS_SHA256_SIZE) {
        return KERR_IO;
    }

    memcpy(hex, text, 2u * MICROOS_SHA256_SIZE);
    hex[2u * MICROOS_SHA256_SIZE] = '\0';

    /* One digest and one newline, and nothing the size of a digest hidden
     * behind it: a hash file of two lines is not the hash of the manifest. */
    if (!hex_is(hex, 2u * MICROOS_SHA256_SIZE) || ((k_size_t)got != 2u * MICROOS_SHA256_SIZE + 1u ||
                                                   text[2u * MICROOS_SHA256_SIZE] != '\n')) {
        return KERR_IO;
    }

    return 0;
}

int port_x86_64_install_verify(const char *point, k_u32_t *out_files)
{
    struct line_reader reader;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file manifest;
    char manifest_path[VFS_PATH_MAX];
    char hash_path[VFS_PATH_MAX];
    char want_hash[2u * MICROOS_SHA256_SIZE + 1u];
    char actual_hash[2u * MICROOS_SHA256_SIZE + 1u];
    char line[COPY_LINE_MAX];
    char target[VFS_PATH_MAX];
    k_u8_t digest[MICROOS_SHA256_SIZE];
    k_u64_t manifest_size = 0u;
    k_u64_t size = 0u;
    k_u32_t count = 0u;
    bool kernel_seen = false;
    bool program_seen = false;
    bool record_seen = false;
    int rc;

    if (point == NULL || out_files == NULL) {
        return KERR_INVAL;
    }

    if (snprintf(manifest_path, sizeof(manifest_path), "%s%s", point, INSTALL_MANIFEST_PATH) >=
            (int)sizeof(manifest_path) ||
        snprintf(hash_path, sizeof(hash_path), "%s%s", point, INSTALL_MANIFEST_HASH_PATH) >=
            (int)sizeof(hash_path)) {
        return KERR_NAMETOOLONG;
    }

    rc = read_manifest_hash(hash_path, want_hash);
    if (rc != 0) {
        kshell_printf("install-verify: cannot read the manifest hash (%d)\n", rc);
        return rc;
    }

    rc = hash_file(manifest_path, digest, &manifest_size);
    if (rc != 0) {
        kshell_printf("install-verify: cannot read %s (%d)\n", manifest_path, rc);
        return rc;
    }
    digest_hex(digest, actual_hash);
    if (!hex_eq(want_hash, actual_hash, 2u * MICROOS_SHA256_SIZE)) {
        kshell_printf("install-verify: MANIFEST MISMATCH sha256:%s, expected sha256:%s\n",
                      actual_hash, want_hash);
        return KERR_IO;
    }
    if (manifest_size > COPY_MANIFEST_MAX) {
        kshell_printf("install-verify: the manifest is %llu byte(s), too large\n",
                      (unsigned long long)manifest_size);
        return KERR_TOOBIG;
    }

    rc = vfs_open(manifest_path, VFS_O_RDONLY, &manifest, priv);
    if (rc != 0) {
        return rc;
    }

    memset(&reader, 0, sizeof(reader));

    for (;;) {
        char path[VFS_PATH_MAX];
        char want[2u * MICROOS_SHA256_SIZE + 1u];
        k_u64_t expected = 0u;
        int got = next_line(&manifest, &reader, line, sizeof(line));

        if (got <= 0) {
            rc = got;
            break;
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        if (!parse_line(line, path, sizeof(path), &expected, want, sizeof(want))) {
            kshell_printf("install-verify: a bad manifest line: %s\n", line);
            rc = KERR_IO;
            break;
        }

        if (snprintf(target, sizeof(target), "%s%s", point, path) >= (int)sizeof(target)) {
            rc = KERR_NAMETOOLONG;
            break;
        }

        rc = hash_file(target, digest, &size);
        if (rc != 0) {
            kshell_printf("install-verify: MISSING FILE %s (%d)\n", path, rc);
            break;
        }

        digest_hex(digest, actual_hash);
        if (size != (k_u64_t)expected) {
            kshell_printf("install-verify: MISMATCH %s: %llu byte(s), expected %llu\n", path,
                          (unsigned long long)size, expected);
            rc = KERR_IO;
            break;
        }
        if (!hex_eq(actual_hash, want, 2u * MICROOS_SHA256_SIZE)) {
            kshell_printf("install-verify: MISMATCH %s: sha256:%s, expected sha256:%s\n", path,
                          actual_hash, want);
            rc = KERR_IO;
            break;
        }

        count++;
        kernel_seen = kernel_seen || streq(path, INSTALL_KERNEL_PATH);
        program_seen = program_seen || streq(path, "/bin/hello");
        record_seen = record_seen || streq(path, "/etc/install");
    }

    {
        int closed = close_read(&manifest);

        if (rc == 0) {
            rc = closed;
        }
    }

    if (rc == 0 && (count == 0u || !kernel_seen || !program_seen || !record_seen)) {
        kshell_printf("install-verify: the manifest lacks the kernel, /bin or the record\n");
        rc = KERR_IO;
    }

    if (rc == 0) {
        kshell_printf("install-verify: verified %u file(s), manifest sha256:%s\n", (unsigned)count,
                      want_hash);
    }

    *out_files = count;
    return rc;
}
