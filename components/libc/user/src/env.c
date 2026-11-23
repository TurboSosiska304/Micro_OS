/*
 * Micro OS — the environment of a program.
 *
 * `_start` receives the vector of the words "NAME=VALUE" from the port and
 * leaves it in `environ` (`src/start.c`); until a program changes it that
 * vector is the memory of the caller and the library only reads it. A call
 * that changes the environment - `setenv`, `unsetenv`, `putenv` - makes the
 * library copy the vector of pointers into the arena and grow it there, so
 * the words of the program and the words of the shell can live side by side
 * and a name is never a duplicate.
 *
 * The vector is the one of POSIX: a NULL-terminated array of pointers, and
 * `environ` names it. The words that `setenv` built belong to the library and
 * are given back when the name is replaced or removed; the word of a
 * `putenv` belongs to the caller, as the standard says, and is never freed
 * here. The parallel array of flags keeps the two apart.
 *
 * A caller that assigns `environ` itself starts over: the next call notices
 * that the vector of the library is not the one the program names and adopts
 * the new one. That is what a program with its own table expects, and what
 * the test of this file does between its cases.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The vector of the words of the program, set by `_start`. */
extern char **environ;

/* The vector the library owns, or NULL while `environ` is still the one of
 * the port. `g_names_s` and `g_owned_s` are the same length, `g_room_s` the
 * number of entries either of them can hold. */
static char **g_names_s;
static unsigned char *g_owned_s;
static size_t g_count_s;
static size_t g_room_s;

/* Give back what the library owns: the words it built and the two arrays. */
static void env_release(void)
{
    size_t at;

    if (g_names_s == NULL) {
        return;
    }

    for (at = 0; at < g_count_s; at++) {
        if (g_owned_s[at]) {
            free(g_names_s[at]);
        }
    }

    free(g_names_s);
    free(g_owned_s);

    g_names_s = NULL;
    g_owned_s = NULL;
    g_count_s = 0;
    g_room_s = 0;
}

static size_t env_length(void)
{
    size_t count = 0;

    if (environ == NULL) {
        return 0;
    }

    while (environ[count] != NULL) {
        count++;
    }

    return count;
}

/* A name of a variable: not empty and without the `=` of the word. */
static int env_name_ok(const char *name)
{
    return name != NULL && name[0] != '\0' && strchr(name, '=') == NULL;
}

/* Take the vector of the program into the arena. The pointers are copied,
 * the words are not: a word of the port is not the block of the library. */
static int env_adopt(void)
{
    char **names;
    unsigned char *owned;
    size_t count;
    size_t room;
    size_t at;

    if (g_names_s != NULL && environ == g_names_s) {
        return 0;
    }

    /* The program named another vector (or replaced the one of the port):
     * the copy of the library is stale and goes back to the arena first. */
    env_release();

    count = env_length();
    room = count + 8;
    names = (char **)calloc(room, sizeof(*names));
    owned = (unsigned char *)calloc(room, 1);
    if (names == NULL || owned == NULL) {
        free(names);
        free(owned);
        errno = ENOMEM;
        return -1;
    }

    for (at = 0; at < count; at++) {
        names[at] = environ[at];
    }
    names[count] = NULL;

    g_names_s = names;
    g_owned_s = owned;
    g_count_s = count;
    g_room_s = room;
    environ = g_names_s;

    return 0;
}

static int env_grow(size_t need)
{
    size_t room;
    char **names;
    unsigned char *owned;

    if (need + 1 <= g_room_s) {
        return 0;
    }

    room = (g_room_s == 0) ? 16 : g_room_s;
    while (room < need + 1) {
        room *= 2;
    }

    names = (char **)realloc(g_names_s, room * sizeof(*names));
    if (names == NULL) {
        errno = ENOMEM;
        return -1;
    }
    g_names_s = names;

    owned = (unsigned char *)realloc(g_owned_s, room);
    if (owned == NULL) {
        errno = ENOMEM;
        return -1;
    }
    g_owned_s = owned;
    g_room_s = room;
    environ = g_names_s;

    return 0;
}

/* The index of a name in the vector, or -1. */
static long env_find(const char *name, size_t length)
{
    size_t at;

    for (at = 0; at < g_count_s; at++) {
        if (strncmp(g_names_s[at], name, length) == 0 && g_names_s[at][length] == '=') {
            return (long)at;
        }
    }

    return -1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t name_length;
    size_t value_length;
    char *word;
    long at;

    if (!env_name_ok(name) || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (env_adopt() != 0) {
        return -1;
    }

    name_length = strlen(name);
    at = env_find(name, name_length);
    if (at >= 0 && !overwrite) {
        return 0;
    }

    value_length = strlen(value);
    word = (char *)malloc(name_length + 1 + value_length + 1);
    if (word == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(word, name, name_length);
    word[name_length] = '=';
    memcpy(word + name_length + 1, value, value_length + 1);

    if (at >= 0) {
        if (g_owned_s[at]) {
            free(g_names_s[at]);
        }
        g_names_s[at] = word;
        g_owned_s[at] = 1;
        return 0;
    }

    if (env_grow(g_count_s + 1) != 0) {
        free(word);
        return -1;
    }

    g_names_s[g_count_s] = word;
    g_owned_s[g_count_s] = 1;
    g_count_s++;
    g_names_s[g_count_s] = NULL;

    return 0;
}

int unsetenv(const char *name)
{
    size_t name_length;
    size_t at;

    if (!env_name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    if (env_adopt() != 0) {
        return -1;
    }

    name_length = strlen(name);
    at = 0;
    while (at < g_count_s) {
        if (strncmp(g_names_s[at], name, name_length) == 0 && g_names_s[at][name_length] == '=') {
            if (g_owned_s[at]) {
                free(g_names_s[at]);
            }
            memmove(&g_names_s[at], &g_names_s[at + 1], (g_count_s - at) * sizeof(*g_names_s));
            memmove(&g_owned_s[at], &g_owned_s[at + 1], g_count_s - at);
            g_count_s--;
            continue;
        }
        at++;
    }
    g_names_s[g_count_s] = NULL;

    return 0;
}

int putenv(char *word)
{
    const char *equal;
    size_t name_length;
    long at;

    if (word == NULL) {
        errno = EINVAL;
        return -1;
    }

    equal = strchr(word, '=');
    if (equal == NULL || equal == word) {
        errno = EINVAL;
        return -1;
    }
    if (env_adopt() != 0) {
        return -1;
    }

    name_length = (size_t)(equal - word);
    at = env_find(word, name_length);
    if (at >= 0) {
        if (g_owned_s[at]) {
            free(g_names_s[at]);
        }
        g_names_s[at] = word;
        g_owned_s[at] = 0;
        return 0;
    }

    if (env_grow(g_count_s + 1) != 0) {
        return -1;
    }

    g_names_s[g_count_s] = word;
    g_owned_s[g_count_s] = 0;
    g_count_s++;
    g_names_s[g_count_s] = NULL;

    return 0;
}
