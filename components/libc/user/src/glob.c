/*
 * Micro OS — the names of a pattern over the tree of the VFS.
 *
 * A pattern is a path whose components may hold the wildcards of `fnmatch`:
 * `*`, `?` and a `[...]` set. The walk is one component at a time, because
 * that is the shape of the VFS: a component without a wildcard is the name
 * itself and one `stat` says whether it exists, and a component with one asks
 * the directory above for its entries and keeps the names `fnmatch` accepts.
 * A component that is not the last one has to be a directory; the last one is
 * any object, and a pattern that ends with `/` asks for a directory and keeps
 * the slash in the answer, as the standard says.
 *
 * A name that begins with a dot is the answer only to a pattern that begins
 * with a dot, and the two names `.` and `..` are never the answer: no command
 * of this system lists them, and a walk that returns them walks in a circle.
 * The order of the answer is the order of the directory unless GLOB_NOSORT is
 * asked, in which case the merge order of `qsort` is the one of the names.
 *
 * A directory that cannot be read is an error only when GLOB_ERR is set, and
 * then the callback of the caller decides: it answers 0 and the walk goes on,
 * or anything else and the answer is GLOB_ABORTED. A pattern that matched
 * nothing is GLOB_NOMATCH, or the pattern itself when GLOB_NOCHECK is set.
 */
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The last component, the last component that has to be a directory, and a
 * component that has a rest of the pattern under it. */
enum glob_mode { GLOB_LAST, GLOB_LAST_DIR, GLOB_NEXT };

struct glob_state {
    int flags;
    int (*error)(const char *path, int code);
    char **names;
    size_t count;
    size_t room;
    int failed; /* 0, GLOB_NOSPACE or GLOB_ABORTED */
};

/* The path of `name` under `dir`; a directory that ends with a slash does not
 * get a second one, and the root is the one slash of the system. */
static char *path_join(const char *dir, const char *name)
{
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);
    int slash = (dir_length > 0 && dir[dir_length - 1] != '/');
    char *path = (char *)malloc(dir_length + (size_t)slash + name_length + 1);

    if (path == NULL) {
        return NULL;
    }

    memcpy(path, dir, dir_length);
    if (slash) {
        path[dir_length++] = '/';
    }
    memcpy(path + dir_length, name, name_length + 1);

    return path;
}

static int glob_add_slash(struct glob_state *state, const char *path, int is_dir, int trailing)
{
    size_t length = strlen(path);
    int mark = trailing || (is_dir && (state->flags & GLOB_MARK) != 0);
    char *name;

    if (state->count + 2 > state->room) {
        size_t room = (state->room == 0) ? 16 : state->room * 2;
        char **names = (char **)realloc(state->names, room * sizeof(*names));

        if (names == NULL) {
            state->failed = GLOB_NOSPACE;
            return -1;
        }
        state->names = names;
        state->room = room;
    }

    name = (char *)malloc(length + (size_t)mark + 1);
    if (name == NULL) {
        state->failed = GLOB_NOSPACE;
        return -1;
    }
    memcpy(name, path, length);
    if (mark) {
        name[length++] = '/';
    }
    name[length] = '\0';

    state->names[state->count++] = name;
    state->names[state->count] = NULL;

    return 0;
}

static int glob_add(struct glob_state *state, const char *path, int is_dir)
{
    return glob_add_slash(state, path, is_dir, 0);
}

/* A name of the caller (the previous answer of GLOB_APPEND) moves into the new
 * array: the string belongs to the result and the array that held it is about
 * to go back to the arena, so the block is the same one and no copy is made. */
static int glob_keep(struct glob_state *state, char *name)
{
    if (state->count + 2 > state->room) {
        size_t room = (state->room == 0) ? 16 : state->room * 2;
        char **names = (char **)realloc(state->names, room * sizeof(*names));

        if (names == NULL) {
            state->failed = GLOB_NOSPACE;
            return -1;
        }
        state->names = names;
        state->room = room;
    }

    state->names[state->count++] = name;
    state->names[state->count] = NULL;

    return 0;
}

static int component_has_wildcard(const char *component, int flags)
{
    const char *at;

    for (at = component; *at != '\0'; at++) {
        if (*at == '*' || *at == '?' || *at == '[') {
            return 1;
        }
        if (*at == '\\' && (flags & GLOB_NOESCAPE) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Does the name begin with a dot and does the pattern not? The convention is
 * the one of a Unix-like system and it is all this reader needs of a period. */
static int hidden_refused(const char *component, const char *name)
{
    return name[0] == '.' && component[0] != '.';
}

static int dirent_is_dot(const char *name)
{
    return (name[0] == '.' && name[1] == '\0') ||
           (name[0] == '.' && name[1] == '.' && name[2] == '\0');
}

static int glob_walk(struct glob_state *state, const char *dir, const char *pattern);

/*
 * One entry under `dir`, an object whose name has to exist. The caller has
 * already decided that `name` matches; this call asks the filesystem what the
 * object is and then either adds it or walks the rest of the pattern in it.
 */
static int glob_entry(struct glob_state *state, const char *dir, const char *name, int mode,
                      const char *next)
{
    char *path = path_join(dir, name);
    struct stat info;
    int answer = 0;

    if (path == NULL) {
        state->failed = GLOB_NOSPACE;
        return -1;
    }

    if (stat(path, &info) != 0) {
        free(path);
        return 0;
    }

    if (mode == GLOB_NEXT) {
        if (S_ISDIR(info.st_mode)) {
            answer = glob_walk(state, path, next);
        }
    } else if (mode == GLOB_LAST_DIR) {
        if (S_ISDIR(info.st_mode)) {
            answer = glob_add_slash(state, path, 1, 1);
        }
    } else {
        answer = glob_add(state, path, S_ISDIR(info.st_mode));
    }

    free(path);

    return answer;
}

/*
 * One component of the pattern under `dir`. The component is given by its
 * length inside the pattern and copied here, because the pattern of the walk
 * goes on after the slash and `fnmatch` wants the text of the component.
 */
static int glob_component(struct glob_state *state, const char *dir, const char *pattern,
                          size_t length, int mode, const char *next)
{
    char *component = (char *)malloc(length + 1);
    int answer = 0;
    DIR *opened = NULL;
    struct dirent *entry;

    if (component == NULL) {
        state->failed = GLOB_NOSPACE;
        return -1;
    }
    memcpy(component, pattern, length);
    component[length] = '\0';

    if (!component_has_wildcard(component, state->flags)) {
        answer = glob_entry(state, dir, component, mode, next);
        free(component);
        return answer;
    }

    opened = opendir((dir[0] == '\0') ? "." : dir);
    if (opened == NULL) {
        if ((state->flags & GLOB_ERR) != 0) {
            int code = errno;

            if (state->error == NULL || state->error(dir[0] == '\0' ? "." : dir, code) != 0) {
                state->failed = GLOB_ABORTED;
            }
        }
        free(component);
        return -1;
    }

    errno = 0;
    while ((entry = readdir(opened)) != NULL) {
        int flags = (state->flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0;

        if (dirent_is_dot(entry->d_name) || hidden_refused(component, entry->d_name)) {
            continue;
        }
        if (fnmatch(component, entry->d_name, flags) != 0) {
            continue;
        }
        if (glob_entry(state, dir, entry->d_name, mode, next) != 0) {
            break;
        }
    }

    {
        int read_error = errno;

        closedir(opened);

        if (read_error != 0 && (state->flags & GLOB_ERR) != 0) {
            if (state->error == NULL || state->error(dir[0] == '\0' ? "." : dir, read_error) != 0) {
                state->failed = GLOB_ABORTED;
            }
        }
    }
    free(component);

    return answer;
}

static int glob_walk(struct glob_state *state, const char *dir, const char *pattern)
{
    const char *slash;
    const char *rest;
    size_t length;

    if (state->failed != 0) {
        return -1;
    }

    if (*pattern == '/') {
        while (*pattern == '/') {
            pattern++;
        }
        dir = "/";
    }

    if (*pattern == '\0') {
        /* The pattern names the directory itself: the root, or the result of
         * the walk of a pattern that ended with a slash. */
        struct stat info;

        if (dir[0] != '\0' && stat(dir, &info) == 0) {
            return glob_add(state, dir, S_ISDIR(info.st_mode));
        }
        return 0;
    }

    slash = strchr(pattern, '/');
    if (slash == NULL) {
        return glob_component(state, dir, pattern, strlen(pattern), GLOB_LAST, NULL);
    }

    length = (size_t)(slash - pattern);
    rest = slash;
    while (*rest == '/') {
        rest++;
    }

    if (*rest == '\0') {
        return glob_component(state, dir, pattern, length, GLOB_LAST_DIR, NULL);
    }

    return glob_component(state, dir, pattern, length, GLOB_NEXT, rest);
}

static int name_compare(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void glob_discard(struct glob_state *state)
{
    size_t at;

    for (at = 0; at < state->count; at++) {
        free(state->names[at]);
    }
    free(state->names);

    state->names = NULL;
    state->count = 0;
    state->room = 0;
}

void globfree(glob_t *result)
{
    size_t at;

    if (result == NULL || result->gl_pathv == NULL) {
        return;
    }

    for (at = 0; at < result->gl_pathc; at++) {
        free(result->gl_pathv[result->gl_offs + at]);
    }
    free(result->gl_pathv);

    result->gl_pathv = NULL;
    result->gl_pathc = 0;
}

int glob(const char *pattern, int flags, int (*error)(const char *path, int code), glob_t *result)
{
    struct glob_state state;
    char **array;
    size_t offs;
    size_t reached;
    size_t at;
    int answer = 0;

    if (pattern == NULL || result == NULL) {
        return GLOB_NOMATCH;
    }

    state.flags = flags;
    state.error = error;
    state.names = NULL;
    state.count = 0;
    state.room = 0;
    state.failed = 0;

    offs = (flags & GLOB_DOOFFS) ? result->gl_offs : 0;

    /* The names of the previous call stay and the array is rebuilt: the
     * strings move with the pointers, so the array alone goes back to the
     * arena and the answer of the caller is not copied. */
    if ((flags & GLOB_APPEND) != 0 && result->gl_pathv != NULL) {
        offs = result->gl_offs;
        for (at = 0; at < result->gl_pathc; at++) {
            if (glob_keep(&state, result->gl_pathv[offs + at]) != 0) {
                break;
            }
        }
        free(result->gl_pathv);
        result->gl_pathv = NULL;
        result->gl_pathc = 0;
    }

    reached = state.count;
    if (state.failed == 0) {
        (void)glob_walk(&state, "", pattern);
    }

    if (state.failed == 0 && state.count == reached && (flags & GLOB_NOCHECK) != 0) {
        (void)glob_add(&state, pattern, 0);
    }

    /* The call answers for the pattern it was given: GLOB_APPEND keeps the
     * names of the previous call in the array, but a pattern that added none
     * is GLOB_NOMATCH all the same. */
    answer = (state.count == reached) ? GLOB_NOMATCH : 0;

    if (state.failed != 0) {
        answer = state.failed;
        glob_discard(&state);
        return answer;
    }

    if (state.count == 0) {
        glob_discard(&state);
        result->gl_pathc = 0;
        result->gl_pathv = NULL;
        result->gl_offs = offs;
        return GLOB_NOMATCH;
    }

    if ((flags & GLOB_NOSORT) == 0 && state.count > 1) {
        qsort(state.names, state.count, sizeof(*state.names), name_compare);
    }

    array = (char **)malloc((offs + state.count + 1) * sizeof(*array));
    if (array == NULL) {
        glob_discard(&state);
        return GLOB_NOSPACE;
    }

    for (at = 0; at < offs; at++) {
        array[at] = NULL;
    }
    for (at = 0; at < state.count; at++) {
        array[offs + at] = state.names[at];
    }
    array[offs + state.count] = NULL;
    free(state.names);

    result->gl_pathc = state.count;
    result->gl_pathv = array;
    result->gl_offs = offs;

    return answer;
}
