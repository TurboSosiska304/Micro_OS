/*
 * Micro OS — the tests of Lua, the second program library of somebody else.
 *
 * The library is the upstream 5.4.7 of `third_party/lua`, compiled for the
 * host with the headers of the host: the test is about the language and the
 * standard libraries, not about the library of the guest. The guest build is
 * the same sources in the static archive of `/bin/lua`, and the checks of the
 * machine are in `tools/x86_smoke.py` and `tools/x86_disk_smoke.py`.
 *
 * What is covered is the surface the port has to keep: a number with a point
 * in it (the `double` of the release), the formatter of the library behind
 * `string.format` and `tostring`, the tables and the strings, an error that a
 * `pcall` catches (the long jump of `ldo.c`), a script read from a file, and
 * the clock and the calendar of the standard library.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#define LUA_TEST_SCRIPT "microos_lua_test.lua"

static lua_State *g_state_s;

/* Run a chunk and leave its first result on the stack: the tests read the
 * answer with the API of the library and not with a print of the host. */
static int run(lua_State *state, const char *chunk)
{
    return luaL_dostring(state, chunk);
}

static void check_number(const char *chunk, lua_Number expected)
{
    lua_State *state = g_state_s;

    KT_CHECK_INT_EQ(run(state, chunk), 0);
    KT_CHECK(lua_isnumber(state, -1) != 0);
    KT_CHECK(lua_tonumber(state, -1) == expected);
    lua_pop(state, 1);
}

static void check_true(const char *chunk)
{
    lua_State *state = g_state_s;

    KT_CHECK_INT_EQ(run(state, chunk), 0);
    KT_CHECK(lua_toboolean(state, -1) != 0);
    lua_pop(state, 1);
}

static void check_string(const char *chunk, const char *expected)
{
    lua_State *state = g_state_s;

    KT_CHECK_INT_EQ(run(state, chunk), 0);
    KT_CHECK(lua_isstring(state, -1) != 0);
    KT_CHECK_STR_EQ(lua_tostring(state, -1), expected);
    lua_pop(state, 1);
}

static void test_lua_number(void)
{
    /* 2^10 is the first number of the port: a power of a double, and the
     * mantissa of it is exact in the format. */
    check_number("return 2^10", 1024.0);
    check_number("return 3/2", 1.5);
    /* The square of the square root is the square root's rounding, not the
     * value it came from: the check is the distance and not equality. */
    check_true("return math.abs(math.sqrt(2)^2 - 2) < 1e-15");
    check_number("return math.floor(-3.5)", -4.0);
    check_number("return math.ceil(-3.5)", -3.0);
    check_number("return math.fmod(7.5, 2)", 1.5);
    check_number("return math.abs(-2.25)", 2.25);
    check_number("return math.exp(0) + math.log(1)", 1.0);
    check_number("return math.sin(0) + math.cos(0)", 1.0);
    check_true("return math.huge > 1e308 and -math.huge < -1e308");
    check_true("return (0/0) ~= (0/0)");
}

static void test_lua_format(void)
{
    /* The formatter of the guest is the one of `components/libc/user`, not
     * the C library of the host: these lines are the shapes Lua asks of it. */
    check_string("return string.format('%.3f', 1/3)", "0.333");
    check_string("return string.format('%.2f', math.pi)", "3.14");
    check_string("return string.format('%d', 42)", "42");
    check_string("return string.format('%8.2f', -1.5)", "   -1.50");
    check_string("return string.format('%g', 0.5)", "0.5");
    check_string("return string.format('%s-%s', 'a', 'b')", "a-b");
    check_string("return tostring(1/2)", "0.5");
    check_string("return tostring(10)", "10");
    check_string("return ('%.14g'):format(2^10)", "1024");
}

static void test_lua_table_and_string(void)
{
    check_string("local t = {'alpha', 'beta', 'gamma'}; return t[#t]", "gamma");
    check_number("local t = {}; for i = 1, 10 do t[i] = i * i end; return t[10]", 100.0);
    check_string("return ('abc'):upper()", "ABC");
    check_string("return string.rep('x', 3)", "xxx");
    check_string("return table.concat({'a', 'b'}, ',')", "a,b");
    check_number("local t = {3, 1, 2}; table.sort(t); return t[1] * 10 + t[3]", 13.0);
    check_string("return string.sub('microos', 1, 5)", "micro");
}

static void test_lua_error(void)
{
#ifdef MICROOS_TSAN
    /*
     * The race detector cannot follow the long jump of the language: its
     * `longjmp` interceptor ends the process with "can't find longjmp buf",
     * and a program of ten lines with a `setjmp`/`longjmp` reproduces it on
     * this toolchain (`docs/debts.md`). The other cases still run; the error
     * path is checked by `make test`, `make test-asan` and the guest.
     */
    ktest_begin("lua: an error is a long jump (not under ThreadSanitizer)");
    return;
#else
    lua_State *state = g_state_s;

    /* The syntax error and the runtime error are the two paths of `ldo.c`:
     * the parser reports before a byte runs, and `error` is a long jump of
     * the state. A `pcall` catches it and the state is usable afterwards. */
    KT_CHECK(luaL_dostring(state, "this is not lua") != 0);
    lua_pop(state, 1);

    check_true("return pcall(function() error('a sentence') end) == false");
    check_string("local ok, message = pcall(function() error('a sentence') end); "
                 "return tostring(ok) .. ' ' .. tostring(message):match('a sentence')",
                 "false a sentence");
    check_number("return 1 + 1", 2.0);
#endif
}

static void test_lua_script_file(void)
{
    lua_State *state = g_state_s;
    FILE *file = fopen(LUA_TEST_SCRIPT, "w");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    fputs("local total = 0\n"
          "for i = 1, 4 do total = total + i end\n"
          "return total, string.format('%.1f', 2.5)\n",
          file);
    fclose(file);

    /* The loader of the library reads the file through the stream of the
     * host; on the machine it is the VFS behind `fopen` of the guest. */
    KT_CHECK_INT_EQ(luaL_dofile(state, LUA_TEST_SCRIPT), 0);
    KT_CHECK(lua_isinteger(state, -2) != 0);
    KT_CHECK_INT_EQ((int)lua_tointeger(state, -2), 10);
    KT_CHECK_STR_EQ(lua_tostring(state, -1), "2.5");
    lua_pop(state, 2);

    KT_CHECK(remove(LUA_TEST_SCRIPT) == 0);

    /* A file that is not there is the error of the loader and not a fault. */
    KT_CHECK(luaL_dofile(state, "microos_lua_test_missing.lua") != 0);
    lua_pop(state, 1);
}

static void test_lua_time(void)
{
    check_true("return os.time() >= 0");
    check_true("local t = os.date('*t'); return t.year >= 1970 and t.month >= 1 and t.month <= 12");
    check_true("return type(os.clock()) == 'number'");
    check_true("return os.date('%Y') == tostring(os.date('*t').year)");
}

void test_lua(void)
{
    lua_State *state = luaL_newstate();

    KT_CHECK(state != NULL);
    if (state == NULL) {
        return;
    }

    ktest_begin("lua: the language and its library");

    g_state_s = state;
    luaL_openlibs(state);

    luaL_checkversion(state);
    KT_CHECK_INT_EQ((int)LUA_VERSION_NUM, 504);

    test_lua_number();
    test_lua_format();
    test_lua_table_and_string();
    test_lua_error();
    test_lua_script_file();
    test_lua_time();

    lua_close(state);
    g_state_s = NULL;
}
