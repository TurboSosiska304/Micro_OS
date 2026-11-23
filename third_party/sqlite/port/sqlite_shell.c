/*
 * Micro OS — the shell of the release with the entry named for the program.
 *
 * `shell.c` of the amalgamation carries a `main` of its own, and the image of
 * `/bin/sqlite3` needs one `main` of its own: the console and the input of
 * this machine are prepared before SQLite sees a file (`programs/sqlite3.c`).
 * The file is compiled whole, with the macro that renames its entry; the
 * vendored source is not edited, and no `-include` of the build reaches the
 * runtime of the library (which would rename the `main` of `start.c` too).
 */
#define main sqlite3_shell_main

#include "../shell.c"
