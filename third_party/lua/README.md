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

## The image and the transfer edge

The image of `/bin/lua` is **past the 64 KiB** the kernel reads in one block
(`KSYS_MAX_IO`, `sys_execve`): 164 KiB of text and data. The call refuses it
before a byte runs, and `exec /bin/lua` answers the sentence of `EINVAL`. The
port is complete - the host test runs the same sources - and the day the
kernel streams or maps an image (K3 of `docs/debts.md`) the program runs with
no change of this directory.

## Tests

* host: `tests/cases/test_lua.c` compiles the whole core with the headers of
  the host and runs chunks through `luaL_newstate`: the numbers, the
  formatter, the tables, the strings, an error caught by `pcall` (the long
  jump), a script file and the clock;
* guest: `tools/x86_smoke.py` and `tools/x86_disk_smoke.py` check that
  `/bin/lua`, `/etc/hello.lua` and `/etc/licenses/lua` are files of the
  volume, and that the kernel refuses an image past its transfer edge. The
  checks of the run are the ones of the day `KSYS_MAX_IO` stops being the
  ceiling.

## The license

`LICENSE` is the MIT text of the release, unchanged; the release note carries
it in `doc/readme.html` and this tree keeps it as a file. The image carries a
copy at `/etc/licenses/lua` beside `/bin/lua`, `tools/bundle.sh` puts
`LICENSE.lua` into the x86-64 archive, and a package that installs the
interpreter copies the same file beside the program. The text is not merged
into the MIT `LICENSE` of the tree.
