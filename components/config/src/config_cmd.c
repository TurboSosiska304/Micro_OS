/*
 * Micro OS — the `config` command of the shell.
 *
 * It shows what the store keeps and saves the files of /etc on demand: the
 * boot does the same, the command is there for a person who has just edited
 * a file and does not want to restart the board.
 *
 * A system of parameters (ADR 0021) is read and written with the same command:
 * `config get display.rotation` reads a key, `config set display.rotation 90`
 * writes the file and applies it at once, and `config list` names the systems
 * of this machine and the keys of one of them.
 */
#include "config/config.h"

#include <string.h>

#include "config/session.h"
#include "config/users.h"
#include "kshell/kshell.h"
#include "kernel/vfs.h"
#include "settings/settings.h"

static void print_systems(void)
{
    k_size_t i;

    kshell_printf("config: the systems are");
    for (i = 0; i < settings_known_count(); i++) {
        kshell_printf(" %s", settings_known_at(i)->name);
    }
    kshell_printf("\n");
}

static void print_keys(const char *system)
{
    const struct settings_known *known = settings_known_get(system);
    const char *keys;
    k_size_t at = 0;

    if (known == NULL) {
        return;
    }

    keys = known->keys;
    kshell_printf("config: %s has the keys", system);

    while (keys[at] != '\0') {
        k_size_t start;

        while (keys[at] == ' ') {
            at++;
        }
        start = at;
        while (keys[at] != '\0' && keys[at] != ' ') {
            at++;
        }
        if (at > start) {
            kshell_printf(" %.*s", (int)(at - start), keys + start);
        }
    }
    kshell_printf("\n");
}

/* `display.rotation` into its two words, when the line has a dot. */
static bool split_spec(const char *spec, char *system, k_size_t system_size, char *key,
                       k_size_t key_size)
{
    const char *dot = strchr(spec, '.');
    k_size_t length;

    if (dot == NULL || dot == spec || dot[1] == '\0') {
        return false;
    }

    length = (k_size_t)(dot - spec);
    if (length + 1 > system_size || strlen(dot + 1) + 1 > key_size) {
        return false;
    }

    memcpy(system, spec, length);
    system[length] = '\0';
    memcpy(key, dot + 1, strlen(dot + 1) + 1);

    return true;
}

/* The sentence of a refused read or write, with the names of the system. */
static int report(const char *system, const char *key, int status)
{
    kshell_printf("config: %s", settings_strerror(status));

    if (status == SETTINGS_ENOSYS) {
        kshell_printf(": %s", system);
    } else if (status == SETTINGS_EKEY) {
        kshell_printf(": %s.%s", system, key);
    }
    kshell_printf("\n");

    if (status == SETTINGS_ENOSYS) {
        print_systems();
    } else if (status == SETTINGS_EKEY) {
        print_keys(system);
    }

    return KSHELL_ERROR;
}

static int cmd_get(int argc, char **argv)
{
    char system[SETTINGS_KEY_MAX];
    char key[SETTINGS_KEY_MAX];
    char value[SETTINGS_VALUE_MAX];
    int rc;

    if (argc != 3 || !split_spec(argv[2], system, sizeof(system), key, sizeof(key))) {
        kshell_printf("usage: config get <system>.<key>\n");
        return KSHELL_USAGE;
    }

    rc = settings_get(system, key, NULL, value, sizeof(value));
    if (rc != SETTINGS_OK) {
        return report(system, key, rc);
    }

    kshell_printf("%s.%s=%s\n", system, key, value);

    return KSHELL_OK;
}

static int cmd_set(int argc, char **argv)
{
    char system[SETTINGS_KEY_MAX];
    char key[SETTINGS_KEY_MAX];
    const char *value;
    int rc;

    /* Two shapes: `config set display.rotation 90` and the Linux one,
     * `config set display rotation 90` (the shell counts the name of the
     * command and the word `set` as well). */
    if (argc == 5) {
        if (strlen(argv[2]) + 1 > sizeof(system) || strlen(argv[3]) + 1 > sizeof(key)) {
            return report(argv[2], argv[3], SETTINGS_EBADKEY);
        }
        memcpy(system, argv[2], strlen(argv[2]) + 1);
        memcpy(key, argv[3], strlen(argv[3]) + 1);
        value = argv[4];
    } else if (argc == 4 && split_spec(argv[2], system, sizeof(system), key, sizeof(key))) {
        value = argv[3];
    } else {
        kshell_printf("usage: config set <system>.<key> <value>\n");
        kshell_printf("       config set <system> <key> <value>\n");
        return KSHELL_USAGE;
    }

    rc = settings_set(system, key, value);

    if (rc == SETTINGS_KEPT) {
        kshell_printf("%s.%s=%s (kept: this machine does not apply %s)\n", system, key, value,
                      system);
        return KSHELL_OK;
    }
    if (rc != SETTINGS_OK) {
        return report(system, key, rc);
    }

    kshell_printf("%s.%s=%s\n", system, key, value);

    return KSHELL_OK;
}

/* One line per system, then the keys of one of them when a name is given. */
static int cmd_list(int argc, char **argv)
{
    const struct settings_known *known;
    char path[VFS_PATH_MAX];
    k_size_t i;

    if (argc > 3) {
        kshell_printf("usage: config list [system]\n");
        return KSHELL_USAGE;
    }

    if (argc == 3) {
        char value[SETTINGS_VALUE_MAX];
        const char *keys;
        k_size_t at = 0;

        known = settings_known_get(argv[2]);
        if (known == NULL) {
            return report(argv[2], "", SETTINGS_ENOSYS);
        }

        keys = known->keys;
        while (keys[at] != '\0') {
            k_size_t start;
            char key[SETTINGS_KEY_MAX];
            k_size_t length;

            while (keys[at] == ' ') {
                at++;
            }
            start = at;
            while (keys[at] != '\0' && keys[at] != ' ') {
                at++;
            }
            length = at - start;
            if (length == 0 || length + 1 > sizeof(key)) {
                continue;
            }
            memcpy(key, keys + start, length);
            key[length] = '\0';

            if (settings_get(known->name, key, NULL, value, sizeof(value)) != SETTINGS_OK) {
                value[0] = '\0';
            }
            kshell_printf("%s.%s=%s\n", known->name, key, value);
        }

        return KSHELL_OK;
    }

    for (i = 0; i < settings_known_count(); i++) {
        known = settings_known_at(i);

        if (settings_path(known->name, path, sizeof(path)) != SETTINGS_OK) {
            path[0] = '\0';
        }

        kshell_printf("%-8s %s%s\n", known->name, path,
                      settings_applied(known->name) ? "" : "  (not applied here)");
    }

    return KSHELL_OK;
}

static int cmd_config(int argc, char **argv)
{
    const struct config_store *store = config_store_get();

    if (argc == 1) {
        char key[CONFIG_KEY_MAX];
        char value[CONFIG_VALUE_MAX];
        k_size_t i;

        /*
         * A machine without a store (x86-64 before it has a medium that
         * survives a reset) has no keys to print. The files of /etc are the
         * settings there, and the command says so instead of printing
         * nothing as if the store were empty.
         */
        if (!config_store_usable()) {
            kshell_printf(
                "config: this machine has no store; the files of /etc are the settings\n");
            return KSHELL_OK;
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

        if (i == 0) {
            kshell_printf("config: the store is empty; the files of /etc are the settings\n");
        }

        return KSHELL_OK;
    }

    if (argc == 2 && strcmp(argv[1], "sync") == 0) {
        int restored;
        int saved;

        /*
         * A sync without a store would only warn over every file of /etc and
         * then report "0 saved" as if it had done its work. It did not: the
         * copy that survives a restart is what the command is for, and this
         * machine has none.
         */
        if (!config_store_usable()) {
            kshell_printf(
                "config: this machine has no store; the files of /etc are the settings\n");
            return KSHELL_ERROR;
        }

        restored = config_restore(CONFIG_DIR);
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

    if (strcmp(argv[1], "get") == 0) {
        return cmd_get(argc, argv);
    }
    if (strcmp(argv[1], "set") == 0) {
        return cmd_set(argc, argv);
    }
    if (strcmp(argv[1], "list") == 0) {
        return cmd_list(argc, argv);
    }

    kshell_printf("usage: config [sync | get <system>.<key> | set <system>.<key> <value> | "
                  "list [system]]\n");

    return KSHELL_USAGE;
}

int config_register(void)
{
    static const struct kshell_help help = {
        .summary = "the configuration of /etc: the store and the systems",
        .usage = "config [sync] | config get <system>.<key> | config set <system>.<key> <value> "
                 "| config list [system]",
        .detail = "Without a word, the keys of the store and their values are printed.\n"
                  "`config sync` restores the files of /etc that the store has and saves the\n"
                  "ones it finds. A machine without a store (x86-64 before it has a medium\n"
                  "that survives a reset) says so: the files of /etc are the settings there,\n"
                  "and `sync` is an error, because there is nothing to save them into. A\n"
                  "system of parameters (`display`, `font`, `console`) is a file of /etc with\n"
                  "`key=value` lines; `get` reads a key, `set` writes the file and applies the\n"
                  "system at once, and `list` names the systems and the keys of one of them.\n",
    };
    int rc;

    /*
     * The store belongs to the port, so the registration of the component is
     * the moment it is handed to the configuration: the boot restores /etc
     * right after this call.
     */
    config_store_use_platform();

    rc = kshell_register_help("config", &help, cmd_config);

    /* The accounts of /etc and the three commands of the console. */
    rc |= users_register();

    /* A session: `logout`, `who` and `whoami` beside them. */
    rc |= session_register();

    return rc;
}
