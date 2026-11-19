/*
 * Micro OS — the store behind the configuration.
 *
 * Every call is the call of the port: the kernel adds the door and no
 * meaning. That is what keeps the upper layers from including
 * kernel/port.h and from knowing whether the board has an NVS or a file.
 */
#include "kernel/kv.h"

#include "kernel/port.h"

int k_kv_get(const char *key, char *buf, k_size_t size)
{
    return k_port_kv_get(key, buf, size);
}

int k_kv_set(const char *key, const char *value)
{
    return k_port_kv_set(key, value);
}

int k_kv_erase(const char *key)
{
    return k_port_kv_erase(key);
}

int k_kv_commit(void)
{
    return k_port_kv_commit();
}

int k_kv_next(k_size_t index, char *key, k_size_t size)
{
    return k_port_kv_next(index, key, size);
}
