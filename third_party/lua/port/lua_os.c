/*
 * Micro OS — the two calls of a process the system does not have.
 *
 * `os.execute` and `io.popen` are the only places where the release of Lua
 * starts a program of the machine, and this system starts no program from a
 * line: there is no shell, no `PATH` lookup of `execve` and no second process
 * that a pipe could talk to. The hooks of `port/luaconf_port.h` point here,
 * and the answer is the failure of the call with `ENOSYS`, which the library
 * of Lua reports as `nil, "not implemented"` and the number of the error.
 *
 * The functions are compiled with the same flags as the library and are the
 * only file of it that knows the name of the library of a program.
 */
#include "lprefix.h"

#include <errno.h>
#include <stdio.h>

#include "lua.h"

int lua_port_system(const char *command)
{
    /*
     * `os.execute()` with no command is the question "is there a shell?", and
     * the answer of this system is no: 0. A command is the failure of a call
     * that cannot run, and the number of the error says why.
     */
    if (command == NULL) {
        return 0;
    }

    (void)command;
    errno = ENOSYS;

    return -1;
}

FILE *lua_port_popen(lua_State *state, const char *command, const char *mode)
{
    (void)state;
    (void)command;
    (void)mode;
    errno = ENOSYS;

    return NULL;
}

int lua_port_pclose(lua_State *state, FILE *stream)
{
    (void)state;
    (void)stream;
    errno = ENOSYS;

    return -1;
}
