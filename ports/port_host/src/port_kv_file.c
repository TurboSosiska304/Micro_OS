/*
 * Micro OS — the key-value store of the host.
 *
 * This file is the port side of kernel/port/kv.h: a file per key under a
 * directory of the temporary storage, so that the tests see the real
 * persistence of the configuration and not a table in the memory.
 *
 * The directory is $MICROOS_KV_DIR, so a test gets a store of its own and
 * does not disturb the one the simulator keeps.
 */
#include "kernel/port/kv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KV_DIR_ENV "MICROOS_KV_DIR"
#define KV_DIR_DEFAULT "/tmp/microos-kv"

/* A directory, a slash, a key of the contract and a NUL fit here easily. */
#define KV_PATH_MAX 256

static const char *kv_dir_name(void)
{
    const char *dir = getenv(KV_DIR_ENV);

    return (dir != NULL && dir[0] != '\0') ? dir : KV_DIR_DEFAULT;
}

/*
 * A key is a file name: nothing that could leave the directory of the store
 * is accepted, because the name comes from a file of /etc that a person can
 * write.
 */
static bool key_ok(const char *key)
{
    if (key == NULL || key[0] == '\0' || key[0] == '.' || strlen(key) >= K_PORT_KV_KEY_MAX) {
        /* A leading dot belongs to the temporary files of a save. */
        return false;
    }
    if (strchr(key, '/') != NULL) {
        return false;
    }

    return strcmp(key, ".") != 0 && strcmp(key, "..") != 0;
}

static int kv_path(const char *key, char *path, k_size_t size)
{
    int n;

    if (!key_ok(key)) {
        return -1;
    }

    n = snprintf(path, size, "%s/%s", kv_dir_name(), key);
    if (n < 0 || (k_size_t)n >= size) {
        return -1;
    }

    return 0;
}

/* The directory is made on the first write and left to the platform after. */
static int kv_dir_make(void)
{
    if (mkdir(kv_dir_name(), 0700) == 0 || errno == EEXIST) {
        return 0;
    }

    return -1;
}

int k_port_kv_get(const char *key, char *buf, k_size_t size)
{
    char path[KV_PATH_MAX];
    char probe;
    ssize_t n;
    int fd;

    if (buf == NULL || size == 0 || kv_path(key, path, sizeof(path)) != 0) {
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? -2 : -1;
    }

    n = read(fd, buf, size - 1);
    if (n < 0) {
        close(fd);
        return -1;
    }

    /*
     * A value that does not fit is an error: a cut value would be saved back
     * as the truth. The probe tells a value of exactly size - 1 bytes from a
     * longer one.
     */
    if ((k_size_t)n == size - 1) {
        ssize_t extra = read(fd, &probe, 1);

        if (extra != 0) {
            close(fd);
            return -1;
        }
    }

    buf[n] = '\0';
    close(fd);

    return (int)n;
}

int k_port_kv_set(const char *key, const char *value)
{
    char path[KV_PATH_MAX];
    char tmp[KV_PATH_MAX];
    k_size_t len;
    int fd;
    int pathlen;
    int rc = 0;

    if (value == NULL || kv_path(key, path, sizeof(path)) != 0) {
        return -1;
    }

    len = strlen(value);
    if (len >= K_PORT_KV_VALUE_MAX) {
        return -1;
    }

    if (kv_dir_make() != 0) {
        return -1;
    }

    /*
     * Write beside the key and rename over it: a restart in the middle of a
     * save leaves the old value, never half of a new one. The leading dot
     * keeps the temporary file out of the walk of the keys.
     */
    /* A temporary name of its own per save: the tasks of the host are the
     * threads of one process, and getpid() would give them all the same
     * name - two saves of one key would then lose each other. */
    pathlen = snprintf(tmp, sizeof(tmp), "%s/.tmp.XXXXXX", kv_dir_name());
    if (pathlen < 0 || (k_size_t)pathlen >= sizeof(tmp)) {
        return -1;
    }

    fd = mkstemp(tmp);
    if (fd < 0) {
        return -1;
    }

    while (rc == 0 && len > 0) {
        ssize_t n = write(fd, value, len);

        if (n <= 0) {
            rc = -1;
            break;
        }
        value += (k_size_t)n;
        len -= (k_size_t)n;
    }

    if (rc == 0 && fsync(fd) != 0) {
        rc = -1;
    }
    if (close(fd) != 0) {
        rc = -1;
    }
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    return 0;
}

int k_port_kv_erase(const char *key)
{
    char path[KV_PATH_MAX];

    if (kv_path(key, path, sizeof(path)) != 0) {
        return -1;
    }

    if (unlink(path) != 0) {
        return errno == ENOENT ? -2 : -1;
    }

    return 0;
}

int k_port_kv_commit(void)
{
    /*
     * Each value was renamed into place; the rename itself is durable only
     * when the directory is flushed, and the promise of the contract is
     * that the keys survive a restart.
     */
    int fd = open(kv_dir_name(), O_RDONLY);

    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (fsync(fd) != 0) {
        int rc = errno;

        close(fd);
        return rc == EINVAL || rc == EBADF ? 0 : -1;
    }

    close(fd);

    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void free_names(char **names, k_size_t count)
{
    k_size_t i;

    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

int k_port_kv_next(k_size_t index, char *key, k_size_t size)
{
    const char *dir_name = kv_dir_name();
    char **names = NULL;
    k_size_t count = 0;
    k_size_t capacity = 0;
    DIR *dir;
    struct dirent *entry;
    int rc = 0;

    if (key == NULL || size == 0) {
        return -1;
    }

    dir = opendir(dir_name);
    if (dir == NULL) {
        /* No directory is an empty store: nothing has been written yet. */
        return errno == ENOENT ? -2 : -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        k_size_t len = strlen(entry->d_name);
        char *copy;

        /* ".", ".." and a temporary file of a save are not keys. A key may
         * not start with a dot (key_ok), so the dot is enough. */
        if (entry->d_name[0] == '.' || len >= K_PORT_KV_KEY_MAX) {
            continue;
        }

        if (count == capacity) {
            k_size_t grown = capacity == 0 ? 8 : capacity * 2;
            char **more = realloc(names, grown * sizeof(*names));

            if (more == NULL) {
                rc = -1;
                break;
            }
            names = more;
            capacity = grown;
        }

        copy = malloc(len + 1);
        if (copy == NULL) {
            rc = -1;
            break;
        }
        memcpy(copy, entry->d_name, len + 1);
        names[count++] = copy;
    }

    closedir(dir);

    if (rc == 0 && count > 1) {
        qsort(names, count, sizeof(*names), cmp_name);
    }

    if (rc != 0) {
        free_names(names, count);
        return -1;
    }
    if (index >= count) {
        free_names(names, count);
        return -2;
    }

    if (strlen(names[index]) >= size) {
        free_names(names, count);
        return -1;
    }

    strcpy(key, names[index]);
    free_names(names, count);

    return 0;
}
