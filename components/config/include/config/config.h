/*
 * Micro OS — the configuration of the board.
 *
 * The files of /etc are the configuration: a file name is the key, the
 * content is the value, and a program reads them through the VFS as it reads
 * any other file. The store behind them (the NVS on the board) is the copy
 * that survives a restart and a formatted volume: the boot writes back every
 * key that /etc lost.
 *
 * The file wins over the store. A person edits /etc, and the next boot (or
 * `config sync`) saves the files; a key of the store that /etc does not have
 * is written back from the store. That way the two sides converge without a
 * timestamp and without a journal.
 */
#ifndef MICRO_OS_CONFIG_CONFIG_H
#define MICRO_OS_CONFIG_CONFIG_H

#include "kernel/types.h"

/* The directory of the configuration. */
#define CONFIG_DIR "/etc"

/* The key: a file name, and the NVS gives a key 15 characters. */
#define CONFIG_KEY_MAX 16

/* The value: one line of text, as the NVS stores a string. */
#define CONFIG_VALUE_MAX 128

/* The most keys one pass looks at. */
#define CONFIG_MAX_KEYS 16

/*
 * A place where the configuration survives a restart: the NVS on the board,
 * a table in the tests. `next` walks the keys from the index 0 and answers
 * -2 after the last one; the other negative codes are real errors.
 */
struct config_store {
    int (*load)(const char *key, char *value, k_size_t size);
    int (*save)(const char *key, const char *value);
    int (*next)(k_size_t index, char *key, k_size_t size);
};

/* Install the store of the platform, or take it away with NULL. */
void config_use_store(const struct config_store *store);

/* The installed store, NULL when the board has none. */
const struct config_store *config_store_get(void);

/*
 * Write the keys of the store that `dir` does not have. A file that is there
 * is left alone: the file is the newer truth. Returns the number of the
 * files written, or a negative error when the directory cannot be read.
 */
int config_restore(const char *dir);

/*
 * Save every file of `dir` into the store. Returns the number of the keys
 * saved, or a negative error when the directory cannot be read.
 */
int config_persist(const char *dir);

/* Restore, then persist: the file and the store end up the same. */
int config_sync(const char *dir);

/* Register the `config` command of the shell. */
int config_register(void);

#endif /* MICRO_OS_CONFIG_CONFIG_H */
