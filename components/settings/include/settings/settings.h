/*
 * Micro OS — the settings of a machine.
 *
 * A system of parameters is one file of /etc whose name is the system
 * (`/etc/display`, `/etc/font`, `/etc/console`): one `key=value` per line,
 * `#` starts a comment, an empty line is not a line. The file is data of the
 * volume like any other, so `cat`, `write` and `config` edit it, and the store
 * of the port keeps the text across a format (ADR 0021).
 *
 * The parser is bounded and needs no memory of its own: one table of pairs of
 * a fixed size on the stack of the caller. A key the system does not know is
 * kept in the file and reported once; a value the system cannot parse is the
 * default of the key and a warning, never a failed boot.
 *
 * A machine applies a system through an applier it registers with its name
 * (the panel of the board, the VGA of x86-64). A machine without one keeps the
 * file and says so: a setting of another machine is not an error.
 */
#ifndef MICRO_OS_SETTINGS_SETTINGS_H
#define MICRO_OS_SETTINGS_SETTINGS_H

#include "kernel/types.h"

/* The directory the systems live in. */
#define SETTINGS_DIR "/etc"

/* A key of [a-z0-9_.-] is at most this long, the terminator not counted. */
#define SETTINGS_KEY_MAX 16

/* A value is one line of printable text of at most this length. */
#define SETTINGS_VALUE_MAX 32

/*
 * The text of a whole system file. It is one value of the store of the port
 * (127 characters), so a file that fits here also survives a format: the
 * limit is not a taste of the parser but the size of the copy.
 */
#define SETTINGS_TEXT_MAX 127

/* The pairs one file may hold, and the room of the registry of the systems. */
#define SETTINGS_KEYS_MAX 12
#define SETTINGS_SYSTEMS_MAX 6

/* The statuses; a caller tells a refusal from a value that was kept. */
#define SETTINGS_OK 0
#define SETTINGS_KEPT 1 /* the file is written, this machine does not apply it */

#define SETTINGS_EBADKEY (-1)   /* not [a-z0-9_.-], empty or too long */
#define SETTINGS_EBADVALUE (-2) /* empty, not printable or too long */
#define SETTINGS_ENOSYS (-3)    /* no such system of this machine */
#define SETTINGS_EKEY (-4)      /* the system does not know the key */
#define SETTINGS_EFILE (-5)     /* the file cannot be read or written */
#define SETTINGS_ETOOLONG (-6)  /* the file, or the rewrite, is over the limit */
#define SETTINGS_ENOTEXT (-7)   /* the buffer of the caller is too small */
#define SETTINGS_EFULL (-8)     /* the registry of the systems is full */

struct settings_pair {
    char key[SETTINGS_KEY_MAX];
    char value[SETTINGS_VALUE_MAX];
};

/*
 * One parsed file. `bad_lines` counts what the parser skipped (no `=`, a key
 * that is not one), `overflow` says that the file holds more pairs than the
 * table: a file that does not fit is refused whole and the defaults are used.
 */
struct settings_table {
    struct settings_pair pairs[SETTINGS_KEYS_MAX];
    k_size_t count;
    k_size_t bad_lines;
    bool overflow;
};

/*
 * What a machine does with the parsed file. The name is the system of the
 * file, the table holds the values; the applier parses its own keys with the
 * helpers below and never fails the boot.
 */
struct settings_system {
    const char *name;
    void (*apply)(const char *system, const struct settings_table *table);
};

/* The systems the tree knows, their defaults and their keys. */
struct settings_known {
    const char *name;
    const char *defaults; /* the text of a fresh file, with its comment */
    const char *keys;     /* the keys, separated by spaces */
};

/* --- parsing and rewriting: no file and no hardware in them --- */

/*
 * Read `len` bytes of text into `out`. A line without `=` or with a key that
 * is not one is counted and skipped; a value that does not fit is a skipped
 * line as well. A later line for the same key wins. Returns SETTINGS_OK, or
 * SETTINGS_ETOOLONG when the file holds more pairs than the table.
 */
int settings_parse(const char *text, k_size_t len, struct settings_table *out);

/* The value of a key, or NULL. */
const char *settings_find(const struct settings_table *table, const char *key);

/*
 * The text of `table` with `key=value` in place of the line of that key. The
 * comments, the unknown keys and the order of the file stay as they are; a
 * key that the file does not have is appended. Returns the length of `out`,
 * or SETTINGS_ETOOLONG when the new text does not fit `out_size`.
 */
int settings_rewrite(const char *text, k_size_t len, const char *key, const char *value, char *out,
                     k_size_t out_size);

/* --- the helpers of an applier --- */

/* The value of a key, or `def` when the line is not there. */
const char *settings_value(const struct settings_table *table, const char *key, const char *def);

/*
 * A number in [min, max]. A line that is not a number, or a number outside
 * the range, is the default and a warning: a broken value never fails a boot.
 */
int settings_number(const struct settings_table *table, const char *key, int def, int min, int max);

/* A flag of 0 or 1; any other value is the default and a warning. */
bool settings_flag(const struct settings_table *table, const char *key, bool def);

/* --- the systems of the tree --- */

k_size_t settings_known_count(void);
const struct settings_known *settings_known_at(k_size_t index);
const struct settings_known *settings_known_get(const char *name);
bool settings_key_known(const char *system, const char *key);

/* --- the machine applies a system --- */

/* Register the applier of a machine; the name is a known system. */
int settings_register(const struct settings_system *system);

/* The applier of a system, NULL when this machine does not apply it. */
const struct settings_system *settings_applier(const char *system);

/* True when this machine applies the system. */
bool settings_applied(const char *system);

/* Forget every applier: the boot registers them, the tests start clean. */
void settings_reset(void);

/* --- the files of /etc --- */

/*
 * Write the file of every known system that is not there, in `dir` (NULL is
 * SETTINGS_DIR). Returns the number of the files written, or a negative error
 * when the directory cannot be made.
 */
int settings_defaults(const char *dir);

/*
 * Read the file of a system, parse it, report what is odd and call the
 * applier. A missing file is the defaults; a file that is too long is
 * refused and the defaults are applied. Returns SETTINGS_OK, SETTINGS_ETOOLONG,
 * SETTINGS_EFILE, or SETTINGS_ENOSYS for a name that is not a system.
 */
int settings_load(const char *system);

/* Load every system in the order of the tree. Returns the number loaded. */
int settings_load_all(void);

/*
 * The value of a key: from the file, from the defaults of the system, or from
 * `def` when neither has it. `out` is NUL terminated; a small buffer is
 * SETTINGS_ENOTEXT, never a cut value.
 */
int settings_get(const char *system, const char *key, const char *def, char *out, k_size_t size);

/*
 * Set a key of a system: the file is read, the line replaced or appended, the
 * file written whole, and the system applied at once. A key the system does
 * not know is refused, so a typo does not reach the file; a key already in the
 * file (of a newer release, say) is kept by a rewrite that does not name it.
 * Returns SETTINGS_OK, SETTINGS_KEPT (written, not applied here) or a
 * negative status.
 */
int settings_set(const char *system, const char *key, const char *value);

/* The path of a system file in `out`; SETTINGS_ENOSYS for an unknown name. */
int settings_path(const char *system, char *out, k_size_t size);

/* The status of a call as a sentence, for the shell. */
const char *settings_strerror(int status);

#endif /* MICRO_OS_SETTINGS_SETTINGS_H */
