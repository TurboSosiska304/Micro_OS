/*
 * Micro OS — the configuration of the board.
 *
 * A key of the configuration is a file of /etc: its name is the key and its
 * content is the value. The store is the key-value store of the port (the
 * flash of the chip, a file per key on a laptop), so the boot can bring the
 * files back after `mkfs -y`.
 */
#include "config/config.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/kv.h"
#include "kernel/vfs.h"

static const struct config_store *g_store;

void config_use_store(const struct config_store *store)
{
    g_store = store;
}

const struct config_store *config_store_get(void)
{
    return g_store;
}

/* A key of the store that is not a file of /etc: it only asks the store a
 * question, and it is erased at once. */
#define CONFIG_PROBE_KEY "cfg-probe"

bool config_store_usable(void)
{
    char key[CONFIG_KEY_MAX];
    char value[CONFIG_VALUE_MAX];
    int rc;

    if (g_store == NULL || g_store->next == NULL || g_store->save == NULL) {
        return false;
    }

    rc = g_store->next(0, key, sizeof(key));
    if (rc == 0) {
        return true; /* a key came back: the walk is answered */
    }
    if (rc != -2) {
        return false; /* a real error of the store, not an empty one */
    }

    /*
     * An empty walk is either an empty store or a machine without one, and
     * the contract of the port has no call that tells the two apart. A key
     * written and erased again is the question they answer differently.
     */
    if (g_store->load != NULL && g_store->load(CONFIG_PROBE_KEY, value, sizeof(value)) >= 0) {
        return true; /* a probe of an earlier run that was not erased */
    }

    if (g_store->save(CONFIG_PROBE_KEY, "1") != 0) {
        return false;
    }

    if (g_store->erase != NULL) {
        (void)g_store->erase(CONFIG_PROBE_KEY);
    }

    return true;
}

/*
 * The adapter of the port's key-value store. The contract of the port speaks
 * the language of the store (`-2` for a key that is not there, a length for a
 * value), the configuration speaks the language of /etc; this is the only
 * place that translates the two.
 */
static int kv_load(const char *key, char *value, k_size_t size)
{
    int rc = k_kv_get(key, value, size);

    /* -2 keeps meaning "no such key", every other code is a real error. */
    return rc >= 0 ? 0 : rc;
}

static int kv_save(const char *key, const char *value)
{
    int rc = k_kv_set(key, value);

    if (rc != 0) {
        return rc;
    }

    /* The key is set, and the store is asked to make it survive the restart. */
    return k_kv_commit();
}

static int kv_next(k_size_t index, char *key, k_size_t size)
{
    return k_kv_next(index, key, size);
}

static int kv_erase(const char *key)
{
    return k_kv_erase(key);
}

static const struct config_store s_kv_store = {
    .load = kv_load,
    .save = kv_save,
    .next = kv_next,
    .erase = kv_erase,
};

void config_store_use_platform(void)
{
    config_use_store(&s_kv_store);
}

/* `dir` and `name` joined with a slash, when the path fits. */
static int join_path(const char *dir, const char *name, char *path, k_size_t size)
{
    k_size_t dlen = strlen(dir);
    k_size_t nlen = strlen(name);

    if (dlen + 1 + nlen + 1 > size) {
        return -1;
    }

    memcpy(path, dir, dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, name, nlen + 1);

    return 0;
}

/*
 * Read a value and strip the line ending a person leaves at the end of a
 * file. Returns 0, 1 when the value is longer than the buffer, or a negative
 * error.
 */
static int read_value(const char *path, char *value, k_size_t size)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_size_t used = 0;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return rc;
    }

    for (;;) {
        int n;

        if (used >= size - 1) {
            char probe;

            n = vfs_read(&file, &probe, 1);
            rc = n > 0 ? 1 : (n < 0 ? n : 0);
            break;
        }

        n = vfs_read(&file, value + used, size - 1 - used);
        if (n < 0) {
            rc = n;
            break;
        }
        if (n == 0) {
            rc = 0;
            break;
        }

        used += (k_size_t)n;
    }

    vfs_close(&file);

    while (used > 0 && (value[used - 1] == '\n' || value[used - 1] == '\r' ||
                        value[used - 1] == ' ' || value[used - 1] == '\t')) {
        used--;
    }
    value[used] = '\0';

    return rc;
}

static int write_value(const char *path, const char *value)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    int rc;

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_write(&file, value, strlen(value));
    if (rc < 0) {
        vfs_close(&file);
        return rc;
    }

    return vfs_close(&file);
}

static int file_exists(const char *path)
{
    struct vfs_stat stat;

    return vfs_stat(path, &stat) == 0;
}

int config_restore(const char *dir)
{
    char key[CONFIG_KEY_MAX];
    char value[CONFIG_VALUE_MAX];
    char path[VFS_PATH_MAX];
    k_size_t i;
    int restored = 0;
    int rc;

    if (g_store == NULL || g_store->next == NULL || g_store->load == NULL || dir == NULL) {
        return -1;
    }

    rc = vfs_mkdir_p(dir);
    if (rc != 0) {
        kwarn("config: cannot create the directory %s (rc=%d)", dir, rc);
        return rc;
    }

    for (i = 0; i < CONFIG_MAX_KEYS; i++) {
        rc = g_store->next(i, key, sizeof(key));
        if (rc == -2) {
            break;
        }
        if (rc != 0) {
            kwarn("config: cannot walk the store (rc=%d)", rc);
            return rc;
        }

        if (key[0] == '\0' || strchr(key, '/') != NULL) {
            continue;
        }
        if (join_path(dir, key, path, sizeof(path)) != 0) {
            continue;
        }
        if (file_exists(path)) {
            continue;
        }

        if (g_store->load(key, value, sizeof(value)) != 0) {
            continue;
        }

        rc = write_value(path, value);
        if (rc != 0) {
            kwarn("config: cannot write %s (rc=%d)", path, rc);
            continue;
        }

        restored++;
    }

    return restored;
}

int config_persist(const char *dir)
{
    struct vfs_dirent entries[CONFIG_MAX_KEYS];
    k_size_t count = 0;
    k_size_t i;
    char path[VFS_PATH_MAX];
    char value[CONFIG_VALUE_MAX];
    int saved = 0;
    int rc;

    if (g_store == NULL || g_store->save == NULL || dir == NULL) {
        return -1;
    }

    rc = vfs_mkdir_p(dir);
    if (rc != 0) {
        kwarn("config: cannot create the directory %s (rc=%d)", dir, rc);
        return rc;
    }

    rc = vfs_list(dir, entries, K_ARRAY_SIZE(entries), &count);
    if (rc != 0) {
        kwarn("config: cannot read the directory %s (rc=%d)", dir, rc);
        return rc;
    }

    for (i = 0; i < count; i++) {
        if (entries[i].type != 1 || entries[i].name[0] == '.') {
            continue;
        }
        if (strlen(entries[i].name) >= CONFIG_KEY_MAX) {
            kwarn("config: the name %s is too long for the store", entries[i].name);
            continue;
        }
        if (join_path(dir, entries[i].name, path, sizeof(path)) != 0) {
            continue;
        }

        rc = read_value(path, value, sizeof(value));
        if (rc == 1) {
            kwarn("config: %s is longer than %d characters, it is not saved", path,
                  CONFIG_VALUE_MAX - 1);
            continue;
        }
        if (rc != 0) {
            kwarn("config: cannot read %s (rc=%d)", path, rc);
            continue;
        }

        if (g_store->save(entries[i].name, value) != 0) {
            kwarn("config: cannot save %s", entries[i].name);
            continue;
        }

        saved++;
    }

    return saved;
}

int config_sync(const char *dir)
{
    int restored = config_restore(dir);
    int saved;

    if (restored < 0) {
        return restored;
    }

    saved = config_persist(dir);
    if (saved < 0) {
        return saved;
    }

    return restored + saved;
}
