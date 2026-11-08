/*
 * Micro OS — the store of the configuration in the NVS of the ESP32.
 *
 * The NVS is the right place for a handful of small strings: it survives the
 * restart, it wears the flash evenly, and it is still there when the volume
 * of MicroFS was formatted. Every key is a file of /etc.
 */
#include "config/config_nvs.h"

#include <string.h>

#include "config/config.h"
#include "kernel/log.h"
#include "nvs.h"
#include "nvs_flash.h"

/* A namespace of its own: the records of the radio are not files of /etc. */
#define CONFIG_NVS_NAMESPACE "config"

static void copy_key(char *dst, k_size_t size, const char *src)
{
    k_size_t i;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int nvs_load(const char *key, char *value, k_size_t size)
{
    nvs_handle_t handle;
    size_t len = size;
    esp_err_t rc;

    rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        return -1;
    }

    rc = nvs_get_str(handle, key, value, &len);
    nvs_close(handle);

    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        return -1;
    }

    return 0;
}

static int nvs_save(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t rc;

    rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (rc != ESP_OK) {
        return -1;
    }

    rc = nvs_set_str(handle, key, value);
    if (rc == ESP_OK) {
        rc = nvs_commit(handle);
    }
    nvs_close(handle);

    return rc == ESP_OK ? 0 : -1;
}

/*
 * Walk the keys of the namespace. The walk starts at the beginning every
 * time: the store is small, and an index is the whole of the interface the
 * configuration needs.
 */
static int nvs_next(k_size_t index, char *key, k_size_t size)
{
    nvs_iterator_t iterator = NULL;
    k_size_t seen = 0;
    esp_err_t rc;

    rc = nvs_entry_find(NVS_DEFAULT_PART_NAME, CONFIG_NVS_NAMESPACE, NVS_TYPE_STR, &iterator);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return -2;
    }
    if (rc != ESP_OK) {
        kerror("config: cannot read the keys of the NVS (rc=%d)", (int)rc);
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

    return -2;
}

static const struct config_store s_nvs_store = {
    .load = nvs_load,
    .save = nvs_save,
    .next = nvs_next,
};

int config_nvs_register(void)
{
    esp_err_t rc = nvs_flash_init();

    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        kwarn("config: the NVS is erased and initialised again");

        if (nvs_flash_erase() != ESP_OK) {
            kerror("config: the NVS cannot be erased");
            return -1;
        }

        rc = nvs_flash_init();
    }

    if (rc != ESP_OK) {
        kerror("config: the NVS cannot be initialised (rc=%d)", (int)rc);
        return -1;
    }

    config_use_store(&s_nvs_store);

    return 0;
}
