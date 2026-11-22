/*
 * Micro OS — the files and the voice of pkg.
 *
 * The component has no path of its own: every path is a path of the tree and
 * the root of the call is put in front of it here, so that a test mounts a
 * volume at /mnt/pkg and `pkg --root /mnt/pkg` is the machine of the test.
 * Every read and write goes through the VFS, as a program of /usr/bin would
 * do: pkg is a command of the shell today and becomes a program of the tree
 * when the calls it needs are all in the table (ADR 0024 §6).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "pkg_private.h"

char g_pkg_root[VFS_PATH_MAX];
int g_pkg_quiet;
int g_pkg_verbose;
int g_pkg_color;

int pkg_join(char *out, k_size_t size, const char *path)
{
    k_size_t root_len = strlen(g_pkg_root);
    k_size_t path_len;

    if (out == NULL || path == NULL) {
        return PKG_EIO;
    }

    path_len = strlen(path);

    if (root_len == 0 || strcmp(g_pkg_root, "/") == 0) {
        if (path_len >= size) {
            return PKG_EIO;
        }

        memcpy(out, path, path_len + 1);

        return PKG_OK;
    }

    if (root_len + path_len >= size) {
        return PKG_EIO;
    }

    memcpy(out, g_pkg_root, root_len);
    memcpy(out + root_len, path, path_len + 1);

    return PKG_OK;
}

int pkg_read(const char *path, void *buf, k_size_t size, k_size_t *got)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        if (got != NULL) {
            *got = 0;
        }

        return PKG_EIO;
    }

    return pkg_read_at(real, buf, size, got);
}

int pkg_read_at(const char *real, void *buf, k_size_t size, k_size_t *got)
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    k_size_t used = 0;
    int rc;

    if (got != NULL) {
        *got = 0;
    }

    rc = vfs_open(real, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return (rc == KERR_NOENT) ? PKG_ENOENT : PKG_EIO;
    }

    while (used < size) {
        int n = vfs_read(&file, (k_u8_t *)buf + used, size - used);

        if (n < 0) {
            (void)vfs_close(&file);
            return PKG_EIO;
        }

        if (n == 0) {
            break;
        }

        used += (k_size_t)n;
    }

    (void)vfs_close(&file);

    if (got != NULL) {
        *got = used;
    }

    return PKG_OK;
}

static int write_all(struct vfs_file *file, const void *buf, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)buf;
    k_size_t done = 0;

    while (done < size) {
        int n = vfs_write(file, bytes + done, size - done);

        if (n <= 0) {
            return PKG_EIO;
        }

        done += (k_size_t)n;
    }

    return PKG_OK;
}

int pkg_write(const char *path, const void *buf, k_size_t size)
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    char real[VFS_PATH_MAX];
    int rc;

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    rc = vfs_open(real, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        return PKG_EIO;
    }

    rc = write_all(&file, buf, size);

    (void)vfs_close(&file);

    return rc;
}

int pkg_append(const char *path, const void *buf, k_size_t size)
{
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    char real[VFS_PATH_MAX];
    int rc;

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    rc = vfs_open(real, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND, &file, priv);
    if (rc != 0) {
        return PKG_EIO;
    }

    rc = write_all(&file, buf, size);

    (void)vfs_close(&file);

    return rc;
}

int pkg_size(const char *path, k_u64_t *size)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    return pkg_size_at(real, size);
}

int pkg_size_at(const char *real, k_u64_t *size)
{
    struct vfs_stat info;

    if (size == NULL || real == NULL) {
        return PKG_EIO;
    }

    if (vfs_stat(real, &info) != 0 || info.type != 1) {
        return PKG_ENOENT;
    }

    *size = info.size;

    return PKG_OK;
}

bool pkg_exists(const char *path)
{
    struct vfs_stat info;
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return false;
    }

    return vfs_stat(real, &info) == 0;
}

int pkg_remove(const char *path)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    return (vfs_unlink(real) == 0) ? PKG_OK : PKG_ENOENT;
}

int pkg_rename(const char *from, const char *to)
{
    char real_from[VFS_PATH_MAX];
    char real_to[VFS_PATH_MAX];
    int rc;

    if (pkg_join(real_from, sizeof(real_from), from) != PKG_OK ||
        pkg_join(real_to, sizeof(real_to), to) != PKG_OK) {
        return PKG_EIO;
    }

    rc = vfs_rename(real_from, real_to);

    return (rc == 0) ? PKG_OK : rc;
}

int pkg_mkdir_p(const char *path)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    return (vfs_mkdir_p(real) == 0) ? PKG_OK : PKG_EIO;
}

int pkg_chmod(const char *path, k_u32_t mode)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    return (vfs_chmod(real, mode) == 0) ? PKG_OK : PKG_EIO;
}

int pkg_list(const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count)
{
    char real[VFS_PATH_MAX];

    if (pkg_join(real, sizeof(real), path) != PKG_OK) {
        return PKG_EIO;
    }

    return (vfs_list(real, out, max, count) == 0) ? PKG_OK : PKG_ENOENT;
}

/* --- the voice --- */

static void pkg_line(const char *prefix, const char *color, const char *fmt, va_list ap)
{
    char text[192];

    if (g_pkg_color && color != NULL) {
        kshell_printf("\x1b[%sm%s\x1b[0m", color, prefix);
    } else {
        kshell_printf("%s", prefix);
    }

    vsnprintf(text, sizeof(text), fmt, ap);
    text[sizeof(text) - 1] = '\0';
    kshell_printf("%s\n", text);
}

void pkg_say(const char *fmt, ...)
{
    va_list ap;

    if (g_pkg_quiet) {
        return;
    }

    va_start(ap, fmt);
    pkg_line(":: ", "1;36", fmt, ap);
    va_end(ap);
}

void pkg_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    pkg_line("warning: ", "1;33", fmt, ap);
    va_end(ap);
}

void pkg_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    pkg_line("error: ", "1;31", fmt, ap);
    va_end(ap);
}

void pkg_plain(const char *fmt, ...)
{
    va_list ap;
    char text[192];

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    text[sizeof(text) - 1] = '\0';
    kshell_printf("%s\n", text);
}
