/*
 * Micro OS — the configuration of Lua for the userland of this system.
 *
 * The file is force-included into every source of Lua (`-include`), before
 * `lua.h` reads `luaconf.h`, so the hooks below are seen where luaconf checks
 * them. Nothing of the upstream release is edited: a setting the system does
 * not have is turned off here, and `luaconf.h` keeps its default when the
 * hook is not named.
 *
 * The system is not POSIX: there is no shell, no pipe to a process, no
 * temporary directory with `mkstemp`, no dynamic linker and no locale but the
 * C one. A program reaches the VFS through `components/libc/user`, and that
 * is the whole of its platform.
 */
#ifndef MICRO_OS_LUA_LUACONF_PORT_H
#define MICRO_OS_LUA_LUACONF_PORT_H

/*
 * `loadfile` looks a module up in these directories. The tree of the machine
 * is `/bin` and `/etc`; the working directory comes last, as in the default
 * of the release. There are no C modules: `require` of one is an error of the
 * loader, not a missing shared object.
 */
#define LUA_PATH_DEFAULT "/bin/?.lua;/etc/?.lua;./?.lua"
#define LUA_CPATH_DEFAULT ""

/*
 * `os.tmpname` needs a name that nobody else has and a call that makes it.
 * The system has no `/tmp` (the volume of the machine is one MicroFS) and no
 * `mkstemp`, so the hook reports that it could not make a name and Lua raises
 * its own error. It is a debt of the platform, not a call to `tmpnam`, which
 * would hand out a path that no file ever had.
 */
#define lua_tmpnam(b, e)                                                                           \
    {                                                                                              \
        (b)[0] = '\0';                                                                             \
        (e) = 1;                                                                                   \
    }

/* The size of the buffer `os.tmpname` builds its name in. The release names
 * the number inside the block this hook replaces, so it is named here. */
#define LUA_TMPNAMBUFSIZE 32

/*
 * `os.execute` and `io.popen` need a shell and a second process. The system
 * has neither: `fork` shares the memory of the parent and there is no
 * `execve` of a line. The port file returns the failure, and the library of
 * Lua turns it into `nil, "not implemented"` for `os.execute` and into the
 * same sentence for `io.popen`; the program of the machine cannot pretend
 * that a command ran.
 */
#define l_system(cmd) lua_port_system(cmd)
#define l_popen(L, c, m) lua_port_popen((L), (c), (m))
#define l_pclose(L, f) lua_port_pclose((L), (f))

/*
 * The declarations of those three calls. The macro is expanded in a source of
 * the library, and a C11 compiler refuses a call without a prototype; the
 * header is the one place every source sees.
 */
#include <stdio.h>

struct lua_State;
int lua_port_system(const char *command);
FILE *lua_port_popen(struct lua_State *state, const char *command, const char *mode);
int lua_port_pclose(struct lua_State *state, FILE *stream);

/*
 * The radix character of the numbers is the one of the C locale. The library
 * of a program has a `localeconv` that says the same, and this hook keeps the
 * source of Lua away from a structure of a locale that does not exist here.
 */
#define lua_getlocaledecpoint() ('.')

#endif /* MICRO_OS_LUA_LUACONF_PORT_H */
