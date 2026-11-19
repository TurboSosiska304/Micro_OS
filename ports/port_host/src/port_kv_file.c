/*
 * Micro OS — the key-value store of the host.
 *
 * This file is the port side of kernel/port/kv.h: a file per key under a
 * directory of the temporary storage, so that the tests see the real
 * persistence of the configuration and not a table in the memory.
 *
 * role-backends-kv: the body is filled by the portability work; until
 * then the store answers "there is nothing stored".
 */
#include "kernel/port.h"

int k_port_kv_get(const char *key, char *buf, k_size_t size)
{
    K_UNUSED(key);
    K_UNUSED(buf);
    K_UNUSED(size);
    return -2;
}

int k_port_kv_set(const char *key, const char *value)
{
    K_UNUSED(key);
    K_UNUSED(value);
    return -1;
}

int k_port_kv_erase(const char *key)
{
    K_UNUSED(key);
    return -2;
}

int k_port_kv_commit(void)
{
    return 0;
}

int k_port_kv_next(k_size_t index, char *key, k_size_t size)
{
    K_UNUSED(index);
    K_UNUSED(key);
    K_UNUSED(size);
    return -2;
}
