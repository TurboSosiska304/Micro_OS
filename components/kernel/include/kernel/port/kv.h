/*
 * Micro OS — platform port: the key-value store.
 *
 * The configuration of /etc has to survive a restart and a formatted
 * volume, and the board already has a place for a handful of small
 * strings: the NVS on the ESP32, a file on the host. The kernel does not
 * care which one: it asks for a key, and the port answers.
 *
 * This is a port duty and not a registry because the board has one store.
 * A platform without one implements the calls as "nothing is stored",
 * which is honest: the configuration then lives only in /etc.
 */
#ifndef MICRO_OS_KERNEL_PORT_KV_H
#define MICRO_OS_KERNEL_PORT_KV_H

#include "kernel/types.h"

/* The keys are file names of /etc; the values are short strings. */
#define K_PORT_KV_KEY_MAX 16
#define K_PORT_KV_VALUE_MAX 128

/*
 * Read a key into a NUL terminated buffer. Returns the length of the value,
 * -2 when the key is not there, -1 on a real error. A buffer that is too
 * small is an error: a truncated value would be saved back as the truth.
 */
int k_port_kv_get(const char *key, char *buf, k_size_t size);

/* Store a key. Returns 0 on success. */
int k_port_kv_set(const char *key, const char *value);

/* Forget a key. Returns 0 on success, -2 when there was no such key. */
int k_port_kv_erase(const char *key);

/* Make the stored keys survive a restart. Returns 0 on success. */
int k_port_kv_commit(void);

/*
 * Walk the keys from the index 0: 0 and the key in the buffer, -2 after
 * the last one, -1 on a real error. The order is the order of the store;
 * the caller does not depend on it.
 */
int k_port_kv_next(k_size_t index, char *key, k_size_t size);

#endif /* MICRO_OS_KERNEL_PORT_KV_H */
