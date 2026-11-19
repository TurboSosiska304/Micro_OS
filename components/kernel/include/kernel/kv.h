/*
 * Micro OS — the store that survives a restart.
 *
 * The configuration of /etc has a copy outside the volume of the flash, so
 * that `mkfs -y` does not take the settings away. The upper layers ask
 * here; the port answers with the NVS of the chip or with a file, and
 * nobody above this header can tell which one it is.
 *
 * The calls are the same as the contract of the port on purpose: this is
 * the door, not a second meaning. Linux does the same with the filesystem
 * of the kernel: the caller writes to a file, not to the driver of a
 * flash chip.
 */
#ifndef MICRO_OS_KERNEL_KV_H
#define MICRO_OS_KERNEL_KV_H

#include "kernel/types.h"

/* The largest key and value the store has to take (a name and a line). */
#define K_KV_KEY_MAX 16
#define K_KV_VALUE_MAX 128

/* The value of the key: its length, -2 when there is no such key, -1 on error. */
int k_kv_get(const char *key, char *buf, k_size_t size);

/* Store a value under a key. Returns 0 on success. */
int k_kv_set(const char *key, const char *value);

/* Forget a key. Returns 0, or -2 when there was no such key. */
int k_kv_erase(const char *key);

/* Make the stored keys survive a restart. Returns 0 on success. */
int k_kv_commit(void);

/* Walk the keys from the index 0: 0 and the key, -2 after the last one. */
int k_kv_next(k_size_t index, char *key, k_size_t size);

#endif /* MICRO_OS_KERNEL_KV_H */
