/*
 * Micro OS — the `config` command of the shell.
 *
 * It shows what the store keeps and saves the files of /etc on demand: the
 * boot does the same, the command is there for a person who has just edited
 * a file and does not want to restart the board.
 */
#include "config/config.h"

#include <string.h>

#include "kshell/kshell.h"

static int cmd_config(int argc, char **argv)
{
    const struct config_store *store = config_store_get();

    if (argc == 1) {
        char key[CONFIG_KEY_MAX];
        char value[CONFIG_VALUE_MAX];
        k_size_t i;

        if (store == NULL || store->next == NULL) {
            kshell_printf("config: this board has no store\n");
            return KSHELL_ERROR;
        }

        for (i = 0; i < CONFIG_MAX_KEYS; i++) {
            if (store->next(i, key, sizeof(key)) != 0) {
                break;
            }
            if (store->load == NULL || store->load(key, value, sizeof(value)) != 0) {
                value[0] = '\0';
            }
            kshell_printf("%s=%s\n", key, value);
        }

        return KSHELL_OK;
    }

    if (argc == 2 && strcmp(argv[1], "sync") == 0) {
        int restored = config_restore(CONFIG_DIR);
        int saved;

        if (restored < 0) {
            kshell_printf("config: cannot restore (rc=%d)\n", restored);
            return KSHELL_ERROR;
        }

        saved = config_persist(CONFIG_DIR);
        if (saved < 0) {
            kshell_printf("config: cannot save (rc=%d)\n", saved);
            return KSHELL_ERROR;
        }

        kshell_printf("config: %d file(s) restored, %d saved\n", restored, saved);
        return KSHELL_OK;
    }

    kshell_printf("usage: config [sync]\n");

    return KSHELL_USAGE;
}

int config_register(void)
{
    return kshell_register("config", "the configuration of /etc: config, config sync", cmd_config);
}
