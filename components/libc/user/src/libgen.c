/*
 * Micro OS — the two halves of a path, for `find`.
 *
 * A path of this system is absolute and has no `.` or `..` by the time a
 * command sees it (the resolver of the library folds them), so the two
 * functions only cut at the last slash. `basename` of a path that ends in a
 * slash answers the directory, as POSIX says, and `dirname` of a name with no
 * slash answers ".". Both may write into the buffer of the caller; a caller
 * that needs its path afterwards copies it first, which is what POSIX allows
 * and what `find` does.
 *
 * The functions keep the static buffers of a machine of one thread: the answer
 * of one is valid until the next call of the same function.
 */
#include <libgen.h>

#include <string.h>

char *basename(char *path)
{
    static char root[] = "/";
    char *end;

    if (path == NULL || path[0] == '\0') {
        return root;
    }

    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }
    *end = '\0';

    /* A path of slashes only is the root, and the name of the root is the
     * root itself, as POSIX says. */
    if (end == path + 1 && path[0] == '/') {
        return root;
    }

    while (end > path && end[-1] != '/') {
        end--;
    }
    if (end == path) {
        return path;
    }

    return end;
}

char *dirname(char *path)
{
    static char root[] = ".";
    char *end;

    if (path == NULL || path[0] == '\0') {
        return root;
    }

    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }
    while (end > path && end[-1] != '/') {
        end--;
    }
    if (end == path) {
        return root;
    }
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }
    *end = '\0';

    return path;
}
