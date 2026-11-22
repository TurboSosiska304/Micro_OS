/*
 * Micro OS — the systems of parameters of /etc.
 *
 * The file is text and the parser is a loop over its lines: no memory is
 * taken, the table of pairs is the caller's, and a file that does not fit is
 * refused whole instead of being half read. `settings_rewrite` is the other
 * direction and the reason a rewrite does not lose a line: it copies the file
 * as it is and replaces the one line it was asked about, so a comment and a
 * key of a newer release survive the `config set` of an older one.
 */
#include "settings/settings.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

/* A key is `[a-z0-9_.-]`: the alphabet is the one the shell can type, so a
 * key is also a word of a command line without quoting. */
static bool key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

static bool key_ok(const char *key)
{
    k_size_t i;

    if (key == NULL || key[0] == '\0') {
        return false;
    }

    for (i = 0; key[i] != '\0'; i++) {
        if (!key_char(key[i])) {
            return false;
        }
        if (i + 1 >= SETTINGS_KEY_MAX) {
            return false; /* one more character and the terminator would not fit */
        }
    }

    return true;
}

static bool value_ok(const char *value)
{
    k_size_t i;

    if (value == NULL || value[0] == '\0') {
        return false;
    }

    for (i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];

        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (i + 1 >= SETTINGS_VALUE_MAX) {
            return false;
        }
    }

    /* The blanks at the ends are not part of a value when the file is read,
     * so a set of " 90" would come back as "90": the same rule starts here. */
    if (value[0] == ' ' || value[0] == '\t' || value[i - 1] == ' ' || value[i - 1] == '\t') {
        return false;
    }

    return true;
}

/* Copy a whole string into a buffer: a value is never cut, the caller checks
 * the room before it calls. */
static void copy_text(char *dst, k_size_t size, const char *src)
{
    k_size_t i;

    for (i = 0; i + 1 < size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* The bounds of one line of the text, with the blanks and the line ending
 * around it taken off. */
struct line {
    k_size_t start;
    k_size_t end;
};

static void trim_line(const char *text, struct line *line)
{
    while (line->start < line->end && (text[line->start] == ' ' || text[line->start] == '\t')) {
        line->start++;
    }
    while (line->end > line->start && (text[line->end - 1] == ' ' || text[line->end - 1] == '\t' ||
                                       text[line->end - 1] == '\r')) {
        line->end--;
    }
}

/* The next line of the text; false after the last one. */
static bool next_line(const char *text, k_size_t len, k_size_t *pos, struct line *line)
{
    k_size_t at = *pos;

    if (at >= len) {
        return false;
    }

    line->start = at;
    while (at < len && text[at] != '\n') {
        at++;
    }
    line->end = at;
    *pos = (at < len) ? at + 1 : at;

    trim_line(text, line);

    return true;
}

/*
 * The key of a line, when the line is a `key=value` one and the key is of the
 * alphabet. The value is not looked at here: the parser and the rewriter ask
 * different questions about it.
 */
static bool line_key(const char *text, const struct line *line, k_size_t *key_start,
                     k_size_t *key_end)
{
    k_size_t at = line->start;
    k_size_t end;

    if (line->start == line->end || text[line->start] == '#') {
        return false;
    }

    while (at < line->end && text[at] != '=') {
        at++;
    }
    if (at == line->end) {
        return false; /* no '=' in the line at all */
    }

    end = at;
    while (end > line->start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        end--;
    }

    if (end == line->start || end - line->start + 1 > SETTINGS_KEY_MAX) {
        return false;
    }

    for (at = line->start; at < end; at++) {
        if (!key_char(text[at])) {
            return false;
        }
    }

    *key_start = line->start;
    *key_end = end;

    return true;
}

/* The value of a line whose key ended at `key_end` (the '=' follows it, with
 * the blanks around it not part of either side). */
static void line_value(const char *text, const struct line *line, k_size_t key_end,
                       k_size_t *value_start, k_size_t *value_end)
{
    k_size_t at = key_end;

    while (at < line->end && (text[at] == ' ' || text[at] == '\t')) {
        at++;
    }
    if (at < line->end && text[at] == '=') {
        at++;
    }
    while (at < line->end && (text[at] == ' ' || text[at] == '\t')) {
        at++;
    }

    *value_start = at;
    *value_end = line->end;
}

static int find_pair(const struct settings_table *table, const char *key, k_size_t key_len)
{
    k_size_t i;

    for (i = 0; i < table->count; i++) {
        if (strlen(table->pairs[i].key) == key_len &&
            memcmp(table->pairs[i].key, key, key_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

int settings_parse(const char *text, k_size_t len, struct settings_table *out)
{
    k_size_t pos = 0;
    struct line line;

    if (out == NULL) {
        return SETTINGS_EBADVALUE;
    }

    memset(out, 0, sizeof(*out));

    if (text == NULL) {
        return SETTINGS_OK;
    }

    while (next_line(text, len, &pos, &line)) {
        k_size_t key_start;
        k_size_t key_end;
        k_size_t value_start;
        k_size_t value_end;
        k_size_t value_len;
        k_size_t i;
        int index;

        if (line.start == line.end || text[line.start] == '#') {
            continue;
        }

        if (!line_key(text, &line, &key_start, &key_end)) {
            out->bad_lines++;
            continue;
        }

        line_value(text, &line, key_end, &value_start, &value_end);
        value_len = value_end - value_start;

        if (value_len == 0 || value_len + 1 > SETTINGS_VALUE_MAX) {
            out->bad_lines++;
            continue;
        }

        for (i = value_start; i < value_end; i++) {
            unsigned char c = (unsigned char)text[i];

            if (c < 0x20 || c == 0x7F) {
                out->bad_lines++;
                break;
            }
        }
        if (i < value_end) {
            continue;
        }

        /* A later line of the same key wins, the way a configuration file of
         * a person is read: the last word is the one that counts. */
        index = find_pair(out, text + key_start, key_end - key_start);
        if (index < 0) {
            if (out->count >= SETTINGS_KEYS_MAX) {
                out->overflow = true;
                continue;
            }
            index = (int)out->count++;
            memcpy(out->pairs[index].key, text + key_start, key_end - key_start);
            out->pairs[index].key[key_end - key_start] = '\0';
        }

        memcpy(out->pairs[index].value, text + value_start, value_len);
        out->pairs[index].value[value_len] = '\0';
    }

    return out->overflow ? SETTINGS_ETOOLONG : SETTINGS_OK;
}

const char *settings_find(const struct settings_table *table, const char *key)
{
    k_size_t i;

    if (table == NULL || key == NULL) {
        return NULL;
    }

    for (i = 0; i < table->count; i++) {
        if (strcmp(table->pairs[i].key, key) == 0) {
            return table->pairs[i].value;
        }
    }

    return NULL;
}

static int append(char *out, k_size_t size, k_size_t *used, const char *text, k_size_t len)
{
    if (*used + len + 1 > size) {
        return SETTINGS_ETOOLONG;
    }

    memcpy(out + *used, text, len);
    *used += len;

    return SETTINGS_OK;
}

int settings_rewrite(const char *text, k_size_t len, const char *key, const char *value, char *out,
                     k_size_t out_size)
{
    k_size_t pos = 0;
    k_size_t used = 0;
    bool replaced = false;
    struct line line;
    int rc;

    if (text == NULL) {
        len = 0;
    }
    if (out == NULL || out_size == 0) {
        return SETTINGS_ETOOLONG;
    }

    while (next_line(text, len, &pos, &line)) {
        k_size_t key_start;
        k_size_t key_end;

        if (line_key(text, &line, &key_start, &key_end) && key_end - key_start == strlen(key) &&
            memcmp(text + key_start, key, key_end - key_start) == 0) {
            rc = append(out, out_size, &used, key, strlen(key));
            if (rc == SETTINGS_OK) {
                rc = append(out, out_size, &used, "=", 1);
            }
            if (rc == SETTINGS_OK) {
                rc = append(out, out_size, &used, value, strlen(value));
            }
            if (rc == SETTINGS_OK) {
                rc = append(out, out_size, &used, "\n", 1);
            }
            if (rc != SETTINGS_OK) {
                return rc;
            }

            replaced = true;
            continue;
        }

        /* The line is copied as it is, a comment or a key of a release this
         * one does not know: the file is not this function's to tidy. */
        rc = append(out, out_size, &used, text + line.start, line.end - line.start);
        if (rc == SETTINGS_OK) {
            rc = append(out, out_size, &used, "\n", 1);
        }
        if (rc != SETTINGS_OK) {
            return rc;
        }
    }

    if (!replaced) {
        rc = append(out, out_size, &used, key, strlen(key));
        if (rc == SETTINGS_OK) {
            rc = append(out, out_size, &used, "=", 1);
        }
        if (rc == SETTINGS_OK) {
            rc = append(out, out_size, &used, value, strlen(value));
        }
        if (rc == SETTINGS_OK) {
            rc = append(out, out_size, &used, "\n", 1);
        }
        if (rc != SETTINGS_OK) {
            return rc;
        }
    }

    out[used] = '\0';

    return (int)used;
}

const char *settings_value(const struct settings_table *table, const char *key, const char *def)
{
    const char *value = settings_find(table, key);

    return value != NULL ? value : def;
}

/* A decimal number of the text, without the library; a value beyond 99999 is
 * not one of the settings and is refused as a text. */
static bool parse_int(const char *text, int *out)
{
    long value = 0;
    bool negative = false;
    k_size_t i = 0;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (text[0] == '-') {
        negative = true;
        i++;
    }
    if (text[i] == '\0') {
        return false;
    }

    for (; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
        if (value > 100000) {
            return false;
        }
    }

    *out = (int)(negative ? -value : value);

    return true;
}

int settings_number(const struct settings_table *table, const char *key, int def, int min, int max)
{
    const char *value = settings_find(table, key);
    int number;

    if (value == NULL) {
        return def;
    }

    if (!parse_int(value, &number) || number < min || number > max) {
        kwarn("settings: %s: '%s' is not a number from %d to %d, the default %d is used", key,
              value, min, max, def);
        return def;
    }

    return number;
}

bool settings_flag(const struct settings_table *table, const char *key, bool def)
{
    const char *value = settings_find(table, key);

    if (value == NULL) {
        return def;
    }

    if (strcmp(value, "1") == 0) {
        return true;
    }
    if (strcmp(value, "0") == 0) {
        return false;
    }

    kwarn("settings: %s: '%s' is not 0 or 1, the default %d is used", key, value, def ? 1 : 0);

    return def;
}

/* --- the systems of the tree --- */

/*
 * The defaults match the behaviour of the machine before the settings
 * existed: the panel in its portrait, inverted (the glass of this model shows
 * a negative without INVON), the backlight at its only level, the dark
 * console and the 5x7 font at its own size. `line_gap` is the gap between two
 * lines of the picture of the x86-64 console (0, 2 or 4 pixels); the panel of
 * the board has no gap and keeps the value.
 */
static const struct settings_known g_known[] = {
    {"display", "# the screen\nrotation=0\ninvert=1\nbrightness=100\ntheme=dark\n",
     "rotation invert brightness theme"},
    {"font", "# the font\nscale=1\ncolumns=0\nrows=0\nline_gap=0\n", "scale columns rows line_gap"},
    {"console", "# the console\ninvert=0\ntheme=dark\n", "invert theme"},
};

k_size_t settings_known_count(void)
{
    return K_ARRAY_SIZE(g_known);
}

const struct settings_known *settings_known_at(k_size_t index)
{
    if (index >= K_ARRAY_SIZE(g_known)) {
        return NULL;
    }

    return &g_known[index];
}

const struct settings_known *settings_known_get(const char *name)
{
    k_size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < K_ARRAY_SIZE(g_known); i++) {
        if (strcmp(g_known[i].name, name) == 0) {
            return &g_known[i];
        }
    }

    return NULL;
}

/* Is `word` one of the space separated words of `list`? */
static bool list_has(const char *list, const char *word)
{
    k_size_t at = 0;
    k_size_t len = strlen(word);

    while (list[at] != '\0') {
        k_size_t start;

        while (list[at] == ' ') {
            at++;
        }
        start = at;
        while (list[at] != '\0' && list[at] != ' ') {
            at++;
        }
        if (at - start == len && memcmp(list + start, word, len) == 0) {
            return true;
        }
    }

    return false;
}

bool settings_key_known(const char *system, const char *key)
{
    const struct settings_known *known = settings_known_get(system);

    if (known == NULL || key == NULL) {
        return false;
    }

    return list_has(known->keys, key);
}

/* --- the appliers of the machine --- */

static const struct settings_system *g_appliers[SETTINGS_SYSTEMS_MAX];
static k_size_t g_applier_count;

int settings_register(const struct settings_system *system)
{
    if (system == NULL || system->name == NULL || settings_known_get(system->name) == NULL) {
        return SETTINGS_ENOSYS;
    }
    if (settings_applier(system->name) != NULL) {
        return SETTINGS_EFULL;
    }
    if (g_applier_count >= SETTINGS_SYSTEMS_MAX) {
        return SETTINGS_EFULL;
    }

    g_appliers[g_applier_count++] = system;

    return SETTINGS_OK;
}

const struct settings_system *settings_applier(const char *system)
{
    k_size_t i;

    if (system == NULL) {
        return NULL;
    }

    for (i = 0; i < g_applier_count; i++) {
        if (strcmp(g_appliers[i]->name, system) == 0) {
            return g_appliers[i];
        }
    }

    return NULL;
}

bool settings_applied(const char *system)
{
    const struct settings_system *applier = settings_applier(system);

    return applier != NULL && applier->apply != NULL;
}

void settings_reset(void)
{
    g_applier_count = 0;
}

/* --- the files of /etc --- */

int settings_path(const char *system, char *out, k_size_t size)
{
    k_size_t at = 0;
    k_size_t i;

    if (settings_known_get(system) == NULL || out == NULL || size == 0) {
        return SETTINGS_ENOSYS;
    }

    for (i = 0; SETTINGS_DIR[i] != '\0'; i++) {
        if (at + 1 >= size) {
            return SETTINGS_ENOTEXT;
        }
        out[at++] = SETTINGS_DIR[i];
    }
    if (at + 1 >= size) {
        return SETTINGS_ENOTEXT;
    }
    out[at++] = '/';
    for (i = 0; system[i] != '\0'; i++) {
        if (at + 1 >= size) {
            return SETTINGS_ENOTEXT;
        }
        out[at++] = system[i];
    }
    out[at] = '\0';

    return SETTINGS_OK;
}

/*
 * The text of a system file. 0 with the text, 1 when the file is not there,
 * SETTINGS_ETOOLONG when it is longer than the buffer (the text in the buffer
 * is then the beginning of the file and is not used) and SETTINGS_EFILE for
 * every other refusal.
 */
static int read_text(const char *system, char *text, k_size_t size)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char path[VFS_PATH_MAX];
    k_size_t used = 0;
    int rc;

    rc = settings_path(system, path, sizeof(path));
    if (rc != SETTINGS_OK) {
        return rc;
    }

    rc = vfs_open(path, VFS_O_RDONLY, &file, priv);
    if (rc == KERR_NOENT) {
        return 1;
    }
    if (rc != 0) {
        return SETTINGS_EFILE;
    }

    for (;;) {
        int n;

        if (used >= size - 1) {
            char probe;

            n = vfs_read(&file, &probe, 1);
            rc = (n > 0) ? SETTINGS_ETOOLONG : (n < 0 ? SETTINGS_EFILE : 0);
            break;
        }

        n = vfs_read(&file, text + used, size - 1 - used);
        if (n < 0) {
            rc = SETTINGS_EFILE;
            break;
        }
        if (n == 0) {
            rc = 0;
            break;
        }

        used += (k_size_t)n;
    }

    (void)vfs_close(&file);
    text[used] = '\0';

    return rc;
}

static int write_text(const char *path, const char *text, k_size_t len)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    int rc;

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        return SETTINGS_EFILE;
    }

    if (vfs_write(&file, text, len) != (int)len) {
        (void)vfs_close(&file);
        return SETTINGS_EFILE;
    }

    return vfs_close(&file) == 0 ? SETTINGS_OK : SETTINGS_EFILE;
}

/*
 * Parse the text, name the keys the system does not know, then call the
 * applier of this machine. Returns true when the machine took the system.
 *
 * The loader reports what it finds in a file a person wrote; the setter does
 * not, because the file was named once at the load of the boot and a person
 * who sets a key knows what the file holds.
 */
static bool apply_text(const char *system, const char *text, k_size_t len, bool report)
{
    struct settings_table table;
    const struct settings_system *applier;
    k_size_t i;

    if (settings_parse(text, len, &table) != SETTINGS_OK) {
        /* The loader and the setter refuse such a file before this point;
         * the defaults of the tree are small enough that this is a bug. */
        kwarn("settings: %s: the file has more pairs than the table, it is not read", system);
        return false;
    }

    for (i = 0; i < table.count; i++) {
        if (report && !settings_key_known(system, table.pairs[i].key)) {
            kwarn("settings: %s: the key '%s' is not known, it is kept", system,
                  table.pairs[i].key);
        }
    }
    if (report && table.bad_lines > 0) {
        kwarn("settings: %s: %u line(s) of the file were not read", system,
              (unsigned)table.bad_lines);
    }

    applier = settings_applier(system);
    if (applier == NULL || applier->apply == NULL) {
        kwarn("settings: this machine does not apply %s, %s/%s is kept", system, SETTINGS_DIR,
              system);
        return false;
    }

    applier->apply(system, &table);

    return true;
}

int settings_load(const char *system)
{
    const struct settings_known *known = settings_known_get(system);
    char text[SETTINGS_TEXT_MAX + 1];
    int status;

    if (known == NULL) {
        return SETTINGS_ENOSYS;
    }

    status = read_text(system, text, sizeof(text));

    if (status == 1) {
        /* A fresh machine has no file: the defaults are the state of the
         * machine, and the boot writes the file for a person to read. */
        copy_text(text, sizeof(text), known->defaults);
        status = SETTINGS_OK;
    } else if (status == SETTINGS_ETOOLONG) {
        kwarn("settings: %s: %s is longer than %d characters, the defaults are used", system,
              SETTINGS_DIR, SETTINGS_TEXT_MAX);
        copy_text(text, sizeof(text), known->defaults);
    } else if (status != SETTINGS_OK) {
        kwarn("settings: %s: the file cannot be read, the defaults are used", system);
        copy_text(text, sizeof(text), known->defaults);
    }

    (void)apply_text(system, text, strlen(text), true);

    return status;
}

int settings_load_all(void)
{
    k_size_t i;
    int loaded = 0;

    for (i = 0; i < settings_known_count(); i++) {
        const struct settings_known *known = settings_known_at(i);

        if (settings_load(known->name) != SETTINGS_ENOSYS) {
            loaded++;
        }
    }

    return loaded;
}

int settings_defaults(const char *dir)
{
    char path[VFS_PATH_MAX];
    k_size_t i;
    int written = 0;
    int rc;

    if (dir == NULL) {
        dir = SETTINGS_DIR;
    }

    rc = vfs_mkdir_p(dir);
    if (rc != 0) {
        kwarn("settings: cannot make the directory %s (rc=%d)", dir, rc);
        return SETTINGS_EFILE;
    }

    for (i = 0; i < settings_known_count(); i++) {
        const struct settings_known *known = settings_known_at(i);
        struct vfs_stat info;
        k_size_t at = 0;
        k_size_t j;

        for (j = 0; dir[j] != '\0' && at + 1 < sizeof(path); j++) {
            path[at++] = dir[j];
        }
        if (at + 1 >= sizeof(path)) {
            continue;
        }
        path[at++] = '/';
        for (j = 0; known->name[j] != '\0' && at + 1 < sizeof(path); j++) {
            path[at++] = known->name[j];
        }
        path[at] = '\0';

        if (vfs_stat(path, &info) == 0) {
            continue;
        }

        if (write_text(path, known->defaults, strlen(known->defaults)) != SETTINGS_OK) {
            kwarn("settings: cannot write %s", path);
            continue;
        }

        (void)vfs_chmod(path, 0644u);
        written++;
    }

    return written;
}

int settings_get(const char *system, const char *key, const char *def, char *out, k_size_t size)
{
    const struct settings_known *known = settings_known_get(system);
    struct settings_table table;
    char text[SETTINGS_TEXT_MAX + 1];
    const char *value;
    int rc;

    if (known == NULL) {
        return SETTINGS_ENOSYS;
    }
    if (!key_ok(key)) {
        return SETTINGS_EBADKEY;
    }

    rc = read_text(system, text, sizeof(text));
    if (rc != SETTINGS_OK) {
        /* A missing, broken or long file is the defaults: the same answer a
         * boot of this machine would use. */
        copy_text(text, sizeof(text), known->defaults);
    }

    (void)settings_parse(text, strlen(text), &table);
    value = settings_find(&table, key);

    if (value == NULL) {
        (void)settings_parse(known->defaults, strlen(known->defaults), &table);
        value = settings_find(&table, key);
    }
    if (value == NULL) {
        value = def;
    }
    if (value == NULL) {
        return SETTINGS_EKEY;
    }
    if (strlen(value) + 1 > size) {
        return SETTINGS_ENOTEXT;
    }

    copy_text(out, size, value);

    return SETTINGS_OK;
}

int settings_set(const char *system, const char *key, const char *value)
{
    const struct settings_known *known = settings_known_get(system);
    char text[SETTINGS_TEXT_MAX + 1];
    char next[SETTINGS_TEXT_MAX + 1];
    char path[VFS_PATH_MAX];
    int length;
    int rc;

    if (known == NULL) {
        return SETTINGS_ENOSYS;
    }
    if (!key_ok(key)) {
        return SETTINGS_EBADKEY;
    }
    if (!settings_key_known(system, key)) {
        return SETTINGS_EKEY;
    }
    if (!value_ok(value)) {
        return SETTINGS_EBADVALUE;
    }

    rc = read_text(system, text, sizeof(text));
    if (rc == 1) {
        copy_text(text, sizeof(text), known->defaults);
    } else if (rc != SETTINGS_OK) {
        /* A file that cannot be read cannot be rewritten without losing what
         * is in it; a file over the limit has to be shortened by a person. */
        return rc;
    }

    length = settings_rewrite(text, strlen(text), key, value, next, sizeof(next));
    if (length < 0) {
        return length;
    }

    /*
     * The rewrite must leave a file this parser reads: a file of twelve keys
     * that gains a thirteenth would fit the text and still be refused at the
     * next boot, so the new text is parsed before it is written.
     */
    {
        struct settings_table check;

        if (settings_parse(next, (k_size_t)length, &check) != SETTINGS_OK) {
            return SETTINGS_ETOOLONG;
        }
    }

    rc = settings_path(system, path, sizeof(path));
    if (rc != SETTINGS_OK) {
        return rc;
    }

    /*
     * The file is written whole in one call. The VFS has no rename, so the
     * rewrite is not atomic: a power cut in the middle leaves a short file,
     * which the parser reports at the next boot, and the store of the port
     * still has the text of the last `config sync` (ADR 0021).
     */
    rc = write_text(path, next, (k_size_t)length);
    if (rc != SETTINGS_OK) {
        return rc;
    }

    return apply_text(system, next, (k_size_t)length, false) ? SETTINGS_OK : SETTINGS_KEPT;
}

const char *settings_strerror(int status)
{
    switch (status) {
    case SETTINGS_OK:
        return "ok";
    case SETTINGS_KEPT:
        return "the value is kept, this machine does not apply the system";
    case SETTINGS_EBADKEY:
        return "a key is [a-z0-9_.-] and at most 15 characters";
    case SETTINGS_EBADVALUE:
        return "a value is printable text of at most 31 characters";
    case SETTINGS_ENOSYS:
        return "there is no such system of this machine";
    case SETTINGS_EKEY:
        return "the system does not know the key";
    case SETTINGS_EFILE:
        return "the file cannot be read or written";
    case SETTINGS_ETOOLONG:
        return "the file is longer than 127 characters";
    case SETTINGS_ENOTEXT:
        return "the buffer of the caller is too small";
    case SETTINGS_EFULL:
        return "the table of the systems is full";
    default:
        return "unknown status";
    }
}
