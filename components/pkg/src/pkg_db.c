/*
 * Micro OS — the database of the installed packages (ADR 0024 §5).
 *
 * One text file per package under /var/pkg/local, one field per line. A
 * directory of small files and not one table, because the table would be a
 * file that every install rewrites whole: a power cut in the middle of it
 * would lose the names of the programs that are still in /usr/bin, and the
 * installer would not know what it may remove or upgrade. A file that is
 * half written costs one record, and the program is still where it was.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"
#include "pkg_private.h"

#define PKG_RECORD_MAX 1024

static int record_path(char *out, k_size_t size, const char *name)
{
    int n = snprintf(out, size, "%s/%s", PKG_LOCAL_DIR, name);

    if (n < 0 || (k_size_t)n >= size) {
        return PKG_EIO;
    }

    return PKG_OK;
}

static k_u64_t record_number(const char *text)
{
    k_u64_t value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (k_u64_t)(*text - '0');
        text++;
    }

    return value;
}

/* Copy the value of one line into its field. */
static void record_field(struct pkg_record *record, const char *key, const char *value)
{
    if (strcmp(key, "name") == 0) {
        snprintf(record->name, sizeof(record->name), "%s", value);
    } else if (strcmp(key, "version") == 0) {
        snprintf(record->version, sizeof(record->version), "%s", value);
    } else if (strcmp(key, "machine") == 0) {
        snprintf(record->machine, sizeof(record->machine), "%s", value);
    } else if (strcmp(key, "file") == 0) {
        snprintf(record->file, sizeof(record->file), "%s", value);
    } else if (strcmp(key, "sha256") == 0) {
        snprintf(record->sha256, sizeof(record->sha256), "%s", value);
    } else if (strcmp(key, "license") == 0) {
        snprintf(record->license, sizeof(record->license), "%s", value);
    } else if (strcmp(key, "source") == 0) {
        snprintf(record->source, sizeof(record->source), "%s", value);
    } else if (strcmp(key, "abi") == 0) {
        record->abi = (k_u32_t)record_number(value);
    } else if (strcmp(key, "size") == 0) {
        record->size = record_number(value);
    } else if (strcmp(key, "explicit") == 0) {
        record->explicit_install = record_number(value) != 0u;
    }
}

int pkg_db_read(const char *name, struct pkg_record *out)
{
    char text[PKG_RECORD_MAX + 1];
    char path[VFS_PATH_MAX];
    k_size_t got = 0;
    k_size_t start = 0;
    k_size_t i;
    int rc;

    if (name == NULL || out == NULL) {
        return PKG_EIO;
    }

    memset(out, 0, sizeof(*out));

    if (record_path(path, sizeof(path), name) != PKG_OK) {
        return PKG_EIO;
    }

    rc = pkg_read(path, text, PKG_RECORD_MAX, &got);
    if (rc != PKG_OK) {
        return rc;
    }

    text[got] = '\0';

    for (i = 0; i <= got; i++) {
        if (i < got && text[i] != '\n') {
            continue;
        }

        text[i] = '\0';

        {
            char *equals = strchr(text + start, '=');

            if (equals != NULL) {
                *equals = '\0';
                record_field(out, text + start, equals + 1);
            }
        }

        start = i + 1;
    }

    return (out->name[0] != '\0') ? PKG_OK : PKG_EIO;
}

int pkg_db_write(const struct pkg_record *record)
{
    char text[PKG_RECORD_MAX];
    char path[VFS_PATH_MAX];
    int n;

    if (record == NULL || record->name[0] == '\0') {
        return PKG_EIO;
    }

    if (record_path(path, sizeof(path), record->name) != PKG_OK) {
        return PKG_EIO;
    }

    n = snprintf(text, sizeof(text),
                 "name=%s\nversion=%s\nmachine=%s\nabi=%u\nsize=%llu\nsha256=%s\nfile=%s\n"
                 "explicit=%u\n",
                 record->name, record->version, record->machine, (unsigned)record->abi,
                 (unsigned long long)record->size, record->sha256, record->file,
                 record->explicit_install ? 1u : 0u);

    if (n < 0 || (k_size_t)n >= sizeof(text)) {
        return PKG_EIO;
    }

    /*
     * The license and the source are added only when the record has them: a
     * record of a local file stays the one it was, and an older machine
     * skips the two lines it does not know.
     */
    if (record->license[0] != '\0') {
        int extra = snprintf(text + n, sizeof(text) - (k_size_t)n, "license=%s\n", record->license);

        if (extra < 0 || (k_size_t)extra >= sizeof(text) - (k_size_t)n) {
            return PKG_EIO;
        }

        n += extra;
    }

    if (record->source[0] != '\0') {
        int extra = snprintf(text + n, sizeof(text) - (k_size_t)n, "source=%s\n", record->source);

        if (extra < 0 || (k_size_t)extra >= sizeof(text) - (k_size_t)n) {
            return PKG_EIO;
        }

        n += extra;
    }

    return pkg_write(path, text, (k_size_t)n);
}

int pkg_db_remove(const char *name)
{
    char path[VFS_PATH_MAX];

    if (name == NULL || record_path(path, sizeof(path), name) != PKG_OK) {
        return PKG_EIO;
    }

    return pkg_remove(path);
}

int pkg_db_list(struct pkg_record *out, k_size_t max, k_size_t *count)
{
    struct vfs_dirent entries[PKG_INDEX_MAX];
    k_size_t found = 0;
    k_size_t used = 0;
    k_size_t i;

    if (out == NULL || count == NULL) {
        return PKG_EIO;
    }

    *count = 0;

    if (pkg_list(PKG_LOCAL_DIR, entries, K_ARRAY_SIZE(entries), &found) != PKG_OK) {
        return PKG_ENOENT;
    }

    for (i = 0; i < found && used < max; i++) {
        if (entries[i].type != 1) {
            continue;
        }

        if (pkg_db_read(entries[i].name, &out[used]) != PKG_OK) {
            continue; /* a record that cannot be read is not a package */
        }

        used++;
    }

    *count = used;

    return PKG_OK;
}

void pkg_log(const char *action, const char *name, const char *version)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "%llu %s %s %s\n", (unsigned long long)k_clock_now_unix(),
                     action != NULL ? action : "?", name != NULL ? name : "?",
                     version != NULL ? version : "?");

    if (n < 0 || (k_size_t)n >= sizeof(line)) {
        return;
    }

    (void)pkg_append(PKG_LOG_FILE, line, (k_size_t)n);
}
