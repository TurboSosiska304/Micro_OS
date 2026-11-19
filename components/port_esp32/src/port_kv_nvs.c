/*
 * Micro OS — the key-value store of the ESP32: the NVS.
 *
 * This file is the port side of kernel/port/kv.h. The store lives in its
 * own namespace of the NVS: the records of the radio are secrets of the
 * driver and are not files of /etc.
 */
#include "kernel/port/kv.h"

#include <string.h>

#include "kernel/lock.h"
#include "kernel/log.h"
#include "nvs.h"
#include "nvs_flash.h"

/* A namespace of its own: the records of the radio are not files of /etc. */
#define KV_NVS_NAMESPACE "config"

/* 0 until the first call, 1 when the NVS is up, -1 when it cannot be used. */
static int g_nvs_state;

/*
 * Bring the NVS up once, on the first key. The boot of a board that never
 * touches the configuration should not pay for a flash mount, and a store
 * that cannot be opened is not retried on every call: the log says why it
 * failed, and the answer stays "there is no store".
 */
/* The first call may come from two tasks at once; one of them brings the
 * store up, the other waits. */
static k_mutex_t s_init_lock = K_MUTEX_INITIALIZER;

static int nvs_ready(void)
{
    esp_err_t rc;

    if (g_nvs_state != 0) {
        return g_nvs_state > 0 ? 0 : -1;
    }

    if (k_mutex_lock(&s_init_lock) != 0) {
        return -1;
    }

    if (g_nvs_state != 0) {
        int state = g_nvs_state;

        (void)k_mutex_unlock(&s_init_lock);
        return state > 0 ? 0 : -1;
    }

    rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        kwarn("kv: the NVS is erased and initialised again");

        if (nvs_flash_erase() != ESP_OK) {
            kerror("kv: the NVS cannot be erased");
            g_nvs_state = -1;
            return -1;
        }

        rc = nvs_flash_init();
    }

    if (rc != ESP_OK) {
        kerror("kv: the NVS cannot be initialised (rc=%d)", (int)rc);
        g_nvs_state = -1;
        return -1;
    }

    g_nvs_state = 1;

    (void)k_mutex_unlock(&s_init_lock);

    return 0;
}

/* The contract and the NVS agree on the size of a key, so one check serves. */
static bool key_ok(const char *key)
{
    return key != NULL && key[0] != '\0' && strlen(key) < K_PORT_KV_KEY_MAX;
}

/* A key of the store is a file name of /etc: it is copied, never truncated. */
static void copy_key(char *dst, k_size_t size, const char *src)
{
    k_size_t i;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

int k_port_kv_get(const char *key, char *buf, k_size_t size)
{
    nvs_handle_t handle;
    size_t len = size;
    esp_err_t rc;

    if (!key_ok(key) || buf == NULL || size == 0) {
        return -1;
    }
    if (nvs_ready() != 0) {
        return -1;
    }

    rc = nvs_open(KV_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        kerror("kv: cannot open the store (rc=%d)", (int)rc);
        return -1;
    }

    rc = nvs_get_str(handle, key, buf, &len);
    nvs_close(handle);

    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc == ESP_ERR_NVS_INVALID_LENGTH) {
        kwarn("kv: the buffer of '%s' is too small for its value", key);
        return -1;
    }
    if (rc != ESP_OK) {
        kerror("kv: cannot read '%s' (rc=%d)", key, (int)rc);
        return -1;
    }

    /* nvs_get_str counts the terminating '\0'; the contract does not. */
    return (int)(len - 1);
}

int k_port_kv_set(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t rc;

    if (!key_ok(key) || value == NULL || strlen(value) >= K_PORT_KV_VALUE_MAX) {
        return -1;
    }
    if (nvs_ready() != 0) {
        return -1;
    }

    rc = nvs_open(KV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        kerror("kv: cannot open the store for a write (rc=%d)", (int)rc);
        return -1;
    }

    rc = nvs_set_str(handle, key, value);
    nvs_close(handle);

    if (rc != ESP_OK) {
        kerror("kv: cannot write '%s' (rc=%d)", key, (int)rc);
        return -1;
    }

    return 0;
}

int k_port_kv_erase(const char *key)
{
    nvs_handle_t handle;
    esp_err_t rc;

    if (!key_ok(key)) {
        return -1;
    }
    if (nvs_ready() != 0) {
        return -1;
    }

    rc = nvs_open(KV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        kerror("kv: cannot open the store for an erase (rc=%d)", (int)rc);
        return -1;
    }

    rc = nvs_erase_key(handle, key);
    nvs_close(handle);

    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        kerror("kv: cannot erase '%s' (rc=%d)", key, (int)rc);
        return -1;
    }

    return 0;
}

int k_port_kv_commit(void)
{
    nvs_handle_t handle;
    esp_err_t rc;

    if (nvs_ready() != 0) {
        return -1;
    }

    rc = nvs_open(KV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        kerror("kv: cannot open the store to commit (rc=%d)", (int)rc);
        return -1;
    }

    rc = nvs_commit(handle);
    nvs_close(handle);

    if (rc != ESP_OK) {
        kerror("kv: cannot commit the store (rc=%d)", (int)rc);
        return -1;
    }

    return 0;
}

/*
 * Walk the keys of the namespace. The walk starts at the beginning every
 * time: the store is small, and an index is the whole of the interface the
 * configuration needs.
 */
int k_port_kv_next(k_size_t index, char *key, k_size_t size)
{
    nvs_iterator_t iterator = NULL;
    k_size_t seen = 0;
    esp_err_t rc;

    if (key == NULL || size == 0) {
        return -1;
    }
    if (nvs_ready() != 0) {
        return -1;
    }

    rc = nvs_entry_find(NVS_DEFAULT_PART_NAME, KV_NVS_NAMESPACE, NVS_TYPE_STR, &iterator);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        kerror("kv: cannot read the keys of the store (rc=%d)", (int)rc);
        return -1;
    }

    while (rc == ESP_OK && iterator != NULL) {
        if (seen == index) {
            nvs_entry_info_t info;

            nvs_entry_info(iterator, &info);
            copy_key(key, size, info.key);
            nvs_release_iterator(iterator);
            return 0;
        }

        seen++;
        rc = nvs_entry_next(&iterator);
    }

    if (iterator != NULL) {
        nvs_release_iterator(iterator);
    }

    /* Only "there are no more keys" is the end of the walk: a damaged store
     * has to be an error, or the configuration would restore nothing and
     * then overwrite itself. */
    if (rc != ESP_OK && rc != ESP_ERR_NVS_NOT_FOUND) {
        kerror("kv: the walk over the store failed (rc=%d)", (int)rc);
        return -1;
    }

    return -2;
}
