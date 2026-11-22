# Lua 5.4.7 on Micro OS

Lua is the second library of somebody else in this tree, after zlib. It is the
stress of the library of a program: a language with a parser, a code
generator, a virtual machine and a standard library, written in C89-compatible
C and expecting a C library, a clock, a formatter with floating point and
`setjmp`.

## The release

| | |
|---|---|
| version | 5.4.7 (13 Jun 2024) |
| source | <https://www.lua.org/ftp/lua-5.4.7.tar.gz> |
| license | MIT, Copyright (C) 1994-2024 Lua.org, PUC-Rio (`LICENSE`) |
| upstream note | `README` (the release note of the tarball) |

The files of `src/` are copied unchanged: the lexer, the parser, the code
generator, the virtual machine, the garbage collector, the string and table
code, the dump and undump of chunks, the libraries of the language, and
`lua.c`, the standalone interpreter, which is the `main` of `/bin/lua`. What
is not here is what the build does not use: `luac.c` (the bytecode compiler of
the release), `lua.hpp` (the C++ header), the `Makefile` and `doc/` of the
tarball. The release note names where to fetch them.

## The port layer

`port/luaconf_port.h` is force-included into every source of the library
(`-include`), before `luaconf.h` reads its hooks. Nothing of the release is
edited; the file turns off what this system does not have:

| Setting | Why |
|---|---|
| `LUA_PATH_DEFAULT`, `LUA_CPATH_DEFAULT` | `require` looks in `/bin` and `/etc`; there is no dynamic linker, so the C path is empty and `package.loadlib` answers "not supported" |
| `lua_tmpnam` (and `LUA_TMPNAMBUFSIZE`) | no `/tmp` of the system and no `mkstemp`: `os.tmpname` reports that it could not make a name |
| `l_system`, `l_popen`, `l_pclose` | no shell and no second process: `os.execute` and `io.popen` answer `nil, "not implemented"` and the errno of the call |
| `lua_getlocaledecpoint` | the radix is the point of the C locale, which is the only locale of the system |

`port/lua_os.c` implements the first and the third row: `lua_port_system`,
`lua_port_popen` and `lua_port_pclose` are the three calls of the platform the
release makes, and they answer the failure of a system without a shell.

`LUA_USE_READLINE` and `LUA_USE_DLOPEN` are not defined by anything: the
interpreter has no line editor and the library no `dlopen`. `LUA_USE_POSIX` is
not defined either, so the release takes its ISO C paths (`fopen`, `fread`,
`time`, `localtime`, `strftime`, `setjmp`/`longjmp`) and not the POSIX ones.

## The library of the program

The library of a program (`components/libc/user`) grew for this port: the
formatter learned `%f`, `%e`, `%g` and a precision, `strtod`/`strtoll`/`qsort`
arrived, and `setjmp`/`longjmp`, `math.h` with the x87 arithmetic, `time.h`
with the calendar of the C locale, `locale.h`, `ctype.h`, `assert.h`,
`signal.h`, `getenv` and `fprintf` are new. `docs/libc.md` and
`docs/porting-programs.md` have the tables.

`/bin/lua` is built with `-DULIBC_WITH_FLOAT` and **without**
`-mgeneral-regs-only`: a `lua_Number` is a `double`, the arithmetic is SSE and
x87, and the port sets `CR4.OSFXSR`, `CR4.OSXMMEXCPT` and `CR0.NE` in the
loader for a program of ring 3. The scheduler of the port saves neither the
XMM registers nor the x87 state, which is safe because `execve` waits for the
program that runs - one program at a time (`docs/debts.md`).

The arena of the program is `ULIBC_HEAP_SIZE=786432` (768 KiB of the bss of
the image; MOX1 caps the bss at 1 MiB). The image is `-Os`: about 140 KiB of
text and 24 KiB of data (168,080 bytes).

## The image and its ceiling

The image of `/bin/lua` is the first program past the transfer edge of one
call: `sys_execve` reads the file in pieces of `KSYS_MAX_IO` (64 KiB) into one
block of the allocator, and `K_EXEC_MAX_IMAGE` (8 MiB) is the ceiling of the
file. 164 KiB is well below it; a larger one is `E2BIG` before a byte is
read. The block is asked with the priority of a user buffer - the PSRAM on the
board, the heap of the port on x86-64 - and the image is then copied into the
window of the program, so the load holds two copies for a moment
(`docs/debts.md`).

## Tests

* host: `tests/cases/test_lua.c` compiles the whole core with the headers of
  the host and runs chunks through `luaL_newstate`: the numbers, the
  formatter, the tables, the strings, an error caught by `pcall` (the long
  jump), a script file and the clock;
* guest: `make x86-smoke` runs `lua -e print(2^10)` (`1024.0`),
  `print(math.pi)`, `string.format("%.3f", 1/3)`, the two values that are not
  numbers, the clock, the script `/etc/hello.lua` (a table, a file written and
  read, the strings) and a script that is not there; `make x86-disk-smoke`
  runs the interpreter out of the volume in the first boot and in the second
  one, after the reset.

## The license

`LICENSE` is the MIT text of the release, unchanged; the release note carries
it in `doc/readme.html` and this tree keeps it as a file. The image carries a
copy at `/etc/licenses/lua` beside `/bin/lua`, `tools/bundle.sh` puts
`LICENSE.lua` into the x86-64 archive, and a package that installs the
interpreter copies the same file beside the program. The text is not merged
into the MIT `LICENSE` of the tree.
