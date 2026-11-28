/*
 * Micro OS — what the files of pkg share.
 *
 * Every path of the component is written as it is in the tree - "/var/pkg",
 * "/usr/bin/hello" - and the root of the call (`--root`) is put in front of
 * it here, in one place. The tests are the reason: a volume mounted at
 * /mnt/test is a machine whose /var and /usr are the ones of the test and not
 * the ones of the run.
 */
#ifndef MICRO_OS_PKG_PRIVATE_H
#define MICRO_OS_PKG_PRIVATE_H

#include "pkg/pkg.h"

#include "kernel/vfs.h"

/* The root of the tree, without a trailing slash; empty is the tree itself. */
extern char g_pkg_root[VFS_PATH_MAX];

/* The voice of the command. */
extern int g_pkg_quiet;
extern int g_pkg_verbose;
extern int g_pkg_color;

/* A path of the tree, with the root of the call in front of it. */
int pkg_join(char *out, k_size_t size, const char *path);

/* The files of the tree; every path is a path of the tree, not of the root. */
int pkg_read(const char *path, void *buf, k_size_t size, k_size_t *got);
int pkg_write(const char *path, const void *buf, k_size_t size);
int pkg_append(const char *path, const void *buf, k_size_t size);
int pkg_size(const char *path, k_u64_t *size);
bool pkg_exists(const char *path);
int pkg_remove(const char *path);
int pkg_rename(const char *from, const char *to);
int pkg_mkdir_p(const char *path);
int pkg_chmod(const char *path, k_u32_t mode);
int pkg_list(const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count);

/* The same read and size with a path that is already the one of the VFS: a
 * file a person names is not a file of the root of the install. */
int pkg_read_at(const char *real, void *buf, k_size_t size, k_size_t *got);
int pkg_size_at(const char *real, k_u64_t *size);

/* The messages: the `::` line of a step, a warning, an error and the plain
 * answer of a query. A quiet command keeps the errors and the answers. */
void pkg_say(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
void pkg_warn(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
void pkg_error(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
void pkg_plain(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));

/* The database: /var/pkg/local/<name>, one line per field. */
int pkg_db_read(const char *name, struct pkg_record *out);
int pkg_db_write(const struct pkg_record *record);
int pkg_db_remove(const char *name);
int pkg_db_list(struct pkg_record *out, k_size_t max, k_size_t *count);

/* The log of what was installed and removed: /var/pkg/log. */
void pkg_log(const char *action, const char *name, const char *version);

/* The key of a repository from its file, without the built-in fallback. */
int pkg_key_file(const char *repo, k_u8_t key[PKG_KEY_SIZE], bool *found);

/* The signature of a file, sixty-four raw bytes. */
int pkg_signature_load(const char *path, k_u8_t signature[PKG_SIGNATURE_SIZE]);

/* The bytes of a package, checked against the entry of the index: the size,
 * the digest, the signature and the header of MOX1. The buffer is the
 * caller's. */
int pkg_package_check(const struct pkg_entry *entry, const k_u8_t signature[PKG_SIGNATURE_SIZE],
                      const k_u8_t key[PKG_KEY_SIZE], const void *data, k_size_t size);

/* The size of the file of an index that a machine takes. */
#define PKG_INDEX_TEXT_MAX 16384

#endif /* MICRO_OS_PKG_PRIVATE_H */
