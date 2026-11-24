# Porting a program to Micro OS

This is the recipe of a program that is somebody else's: a library and a
command of a Unix-like system that were written for a C library and a POSIX
kernel, built with the compiler and the library of this system and run in the
window of a program. The first one is zlib and `/bin/zcat`
([`third_party/zlib/README.md`](../third_party/zlib/README.md)); the next ones
are Lua (an interpreter), zstd (a library and the command of its own release),
bc/dc, `file` with libmagic, the tools of sbase and SQLite, and the recipe is
written so that they are copies of a shape and not new research.

It is not the recipe of a **port of Micro OS** to another machine: that one is
[`porting.md`](porting.md), and it is about the contract of `kernel/port/`.

## What a port of a program can be

A program of Micro OS is a static `MOX1` image linked for the window of the
machine, entered at `_start`, which reaches the kernel only through
`syscall` on x86-64 (ADR 0006, ADR 0016). There is no dynamic linker and no
`libc.so`: the third-party library is a **static archive** built by the same
compiler with the same flags as the program, and the linker takes the objects
the program names. A port is therefore three things:

1. a vendored copy of the upstream source in `third_party/<name>/`;
2. a port layer in `third_party/<name>/port/` for what the source expects of
   a platform and this system does not have;
3. a line in the CMake of the port that builds the archive and links it into
   a program of `/bin`.

The board (ESP32) is not a target of this recipe. A program of the board has
8 KiB of text and 8 KiB of data in two fixed windows and shares the address
space of the kernel (ADR 0018); a library of a hundred kilobytes does not
fit and a bad pointer is a reboot. The port of a program is the x86-64 port
(and, one day, a machine with an MMU). The host build is the second build of
the same source, for the tests.

## 1. Vendoring

```
third_party/<name>/
    <the files of the release, unchanged>
    LICENSE                 the license of the upstream, byte for byte
    README                  the upstream release note, if it has one
    README.md               our note: version, source, files, port layer
    port/                   what this system adds (see below)
```

The rules are the ones of [`licensing.md`](licensing.md):

* permissive only: MIT, BSD-2-Clause, BSD-3-Clause, ISC, Apache-2.0 or the
  zlib license. Copyleft does not enter a binary of the MIT tree;
* the upstream files are **not edited** where a port layer can answer. If a
  hunk is unavoidable, `README.md` names the file and the hunk, and the next
  update has a diff to read;
* `README.md` says the version, the exact upstream (a URL and a release),
  what is included and what is left out, which files are ours, and where the
  binary travels;
* the license of the binary travels with it: a file in the image or in the
  package, a copy in the bundle, a row in `licensing.md`. Never merge a
  third-party license into the MIT `LICENSE` of the tree.

The tree does not vendor documentation, examples, test suites or the build
systems of the release unless the build needs them. `README.md` names the
release so that a reader can fetch them.

## 2. The port layer

The layer answers the questions the source asks of a platform. zlib asks two
kinds, and they are the usual two:

### The headers of the C library

A portable source includes `<string.h>`, `<stdlib.h>`, `<stdio.h>`,
`<errno.h>`, `<unistd.h>`, `<fcntl.h>`, `<sys/types.h>`, and expects the
functions and the types behind them. The library of a program now carries
thin headers with those names (`components/libc/user/include/`), each one
including `ulibc.h`; a program that includes `<string.h>` gets the functions
of the system and not the ones of the host. The functions a portable library
tends to need and the tree has:

| Header | What is there |
|---|---|
| `<string.h>` | `strlen`, `strcmp`, `strncmp`, `strcoll`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strcpy`, `strncpy`, `strcat`, `strchr`, `strrchr`, `strdup` (a file of its own: it allocates), `memcpy`, `memmove`, `memset`, `memcmp`, `memchr` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `exit`, `abort`, `system` (1.8: a fork, the program of the first word of the line and its status; there is no shell), `getenv`/`environ`, `setenv`, `unsetenv`, `putenv`, `strtod`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `atoi`, `atol`, `atof`, `abs`, `labs`, `qsort`, `EXIT_SUCCESS`, `EXIT_FAILURE` |
| `<stdio.h>` | `printf`, `vprintf`, `fprintf`, `vfprintf`, `sprintf`, `snprintf`, `vsnprintf`, `puts`, `putchar`, `perror`, `getchar`, `sscanf`, `vsscanf`, `scanf`, `vscanf`, `fscanf`, `vfscanf`, `popen`/`pclose` (1.8: a pipe, a fork and the `execve` of the first word of the command, because there is no `/bin/sh`), and the streams `fopen`, `fdopen`, `freopen`, `tmpfile` (a failure: no `/tmp`), `fclose`, `fread`, `fwrite`, `fgetc`, `getc`, `ungetc`, `fputc`, `fgets`, `fputs`, `feof`, `ferror`, `clearerr`, `fflush`, `fseek`, `ftell`, `rewind`, `fileno`, `setvbuf` (accepted, no buffer), `stdin`, `stdout`, `stderr` |
| `<sys/stat.h>` | `struct stat` with the `S_IS*` macros, `stat`, `lstat` (the same call: no links), `fstat` (the position of the end), `chmod`, `umask`. The record of the ABI itself is `ulibc_stat_call`, which fills `struct vfs_stat` |
| `<dirent.h>` | `DIR`, `struct dirent`, `opendir`, `readdir`, `closedir`, `rewinddir`, `dirfd` over a batch of eight entries of the call `ulibc_readdir` |
| `<libgen.h>` | `basename`, `dirname` |
| `<errno.h>` | the POSIX numbers, `EDOM`/`ERANGE`, and `errno`, which is a plain global of the program |
| `<unistd.h>` | `read`, `write`, `lseek`, `close`, `unlink`, `isatty` (0: no terminal), `realpath`, the three `STD*_FILENO` and `off_t`/`ssize_t` |
| `<fcntl.h>` | `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY`; `O_EXCL` with `O_CREAT` is `EEXIST` for a name that is already there |
| `<sys/types.h>` | `off_t`, `ssize_t`, `off64_t` |
| `<math.h>` | `sqrt`, `pow`, `fmod`, `floor`, `ceil`, `trunc`, `round`, `fabs`, `exp`, `exp2`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `frexp`, `ldexp`, `modf` |
| `<time.h>` | `time` (the wall clock of the call 49, the seconds of Unix), `clock` (the processor time of the call 50), `difftime`, `gmtime`, `localtime`, `mktime`, `strftime`, `strptime`, `struct tm` |
| `<glob.h>` | `glob`/`globfree` over `opendir`/`readdir` and `stat`: `*`, `?`, `[`...`]`, `GLOB_NOSORT`, `GLOB_NOCHECK`, `GLOB_MARK`, `GLOB_DOOFFS`, `GLOB_ERR`, `GLOB_APPEND` |
| `<getopt.h>` | `struct option`, `getopt_long`, `getopt_long_only` beside `getopt`: a long name, `--name=value`, an unambiguous prefix, the `flag`/`val` of a table |
| `<setjmp.h>` | `setjmp`/`longjmp` (the registers of x86-64) |
| `<locale.h>` | the C locale: `setlocale`, `localeconv` |
| `<ctype.h>` | the classes of a byte and `tolower`/`toupper` |
| `<signal.h>` | the four signals of the ABI, `signal`, `raise`, `sig_atomic_t` |
| `<assert.h>` | `assert` |

The allocation is a **heap whose first block is an arena in the bss of the
program**, not `KSYS_MALLOC`: the block of the kernel is memory of ring 0 and
the page tables of a program do not map it with the user bit. The first block
is `-DULIBC_HEAP_SIZE=<bytes>` (256 KiB by default), and a request the arena
cannot hold grows the heap with `brk` (call 47), so a program that allocates
two megabytes does not need a constant of its build. The growth is charged to
the memory limit of the task; on a machine without an address space (the
board) `brk` answers `ENOSYS` and the heap stays the arena, which is the size
the bss of the format allows (1 MiB).

The formatter has `%d %i %u %o %x %X %c %s %p %%`, the flags `- + space # 0`
and `*`, a precision, and the length `l`/`ll`/`z`/`j`/`h`. A build that
defines `ULIBC_WITH_FLOAT` adds `%f %F %e %E %g %G`; a program of the tree
that does not defines it is built with `-mgeneral-regs-only` and prints such
a conversion as the two characters it was written with. `%a` is not there
(Lua prints a hexadecimal float only if a script asks for it; `%a` is a
debt).

### The shape of the calls

The ABI of this system answers the negative error itself (`-2` is `ENOENT`)
and sets `errno` in the library as a convenience. A program of a Unix-like
system expects `-1` and `errno`, and writes `if (fd == -1)`, not `if (fd <
0)`. The port layer translates the calls the source uses:

```c
/* third_party/<name>/port/<name>_os.c */
int name_os_open(const char *path, int flags, ...)
{
    int fd = open(path, flags);

    if (fd < 0) {
        errno = -fd;
        return -1;
    }
    return fd;
}
```

and the configuration header renames the call for the sources of the library
(`#define open name_os_open`), while the port source itself is compiled with
`<NAME>_OS_NO_RENAME` so that its wrapper reaches the real call. zlib is the
worked example (`third_party/zlib/port/zconf.h`, `port/zlib_os.c`).

Two more shapes are worth knowing:

* **A pipe blocks while its producer is still there** (ADR 0035, which
  changed the `EAGAIN` of ADR 0014): a read of an empty pipe with a writer
  sleeps until the bytes arrive, and a write to a full one sleeps until the
  reader makes room. A wrapper of `read` that waits `EAGAIN` out is still
  correct and no longer needed; a library with a `select` loop needs the
  descriptor of the pipe and a `sleep`/`yield`, because this system has
  `sleep` in the ABI and no `poll` yet.
* **The image has a size budget.** The kernel reads a program into one block
  of the allocator, in pieces of `KSYS_MAX_IO` (64 KiB), before the port
  copies it to the window; the ceiling of the file is `K_EXEC_MAX_IMAGE`
  (8 MiB, the memory of the machine) and the window of the port is 16 MiB.
  `-Os` took `zcat` from 80 KiB to 58 KiB and keeps `/bin/sqlite3` at
  846,080 bytes (826 KiB), a sixth of the ceiling. A larger program needs the
  streaming of the image into the window,
  which is a debt.

## 3. The build

The two functions are in [`cmake/userland.cmake`](../cmake/userland.cmake) and
are included by `ports/port_x86_64/CMakeLists.txt`:

```cmake
set(NAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/<name>")
set(NAME_SOURCES ...)          # the .c files of the release

microos_userland_library(microos_user_<name>
    SOURCES ${NAME_SOURCES} "${NAME_DIR}/port/<name>_os.c"
    INCLUDES "${NAME_DIR}" "${NAME_DIR}/port"
    DEFINES MY_<NAME>_PORT        # what the port layer needs
    COMPILE_OPTIONS "-include" "${NAME_DIR}/port/zconf.h" -Wno-sign-compare -Os
)

add_libc_program(<command>
    LIBRARIES microos_user_<name>
    INCLUDES "${NAME_DIR}"
    DEFINES ULIBC_HEAP_SIZE=524288u
    SOURCES "${LIBC_USER_DIR}/src/malloc.c"   # when the library allocates
            "${LIBC_USER_DIR}/src/file.c"     # when it uses a stream
    COMPILE_OPTIONS -Os
)
```

`microos_userland_library` builds a static archive with `MICROOS_USERLAND_CFLAGS`
and `microos_add_userland_program` compiles the runtime of
`components/libc/user`, the file of the program and the archives for the
window, and hands them to `tools/link_program.sh`, which links them with
`-nostdlib -static -no-pie` and `programs/program.ld` at `0x02000000 + 32`,
reads the address of the text back and links again if the linker moved it,
and then runs `tools/mkprog.sh --base 0x02000000 --arch x86-64`. The resulting
`.mox` is a file of `/bin` in the image; the program
is added to `tools/x86_image.py` (the `PROGRAMS` list) and to the checks of
the smoke tests.

The guest flags are the ones of the kernel and of the existing programs:
`-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
-mgeneral-regs-only -march=x86-64 -fno-asynchronous-unwind-tables
-fno-merge-constants -Wall -Wextra -Werror`. No `-mcmodel=kernel` (that one is
the kernel's), and no libc of the host: a symbol that the archive cannot
resolve is a link error and not a silent call into glibc.

A library that does not compute in floating point links `src/stdlib.c` and
`src/time.c` without `ULIBC_WITH_FLOAT`: the conversion of a double
(`strtod`, `atof`) and `difftime` are in the float build alone, the way the
formatter is, so `zstd` pays neither their code nor the SSE register they
return in.

`-mgeneral-regs-only` is the rule of a program that computes in integers.
A library whose numbers are doubles (Lua) is built with the `FLOAT` option of
the two functions instead: the port's loader sets `CR4.OSFXSR`,
`CR4.OSXMMEXCPT` and `CR0.NE`, the runtime of the program is compiled with
`-DULIBC_WITH_FLOAT`, and `src/math.c`, `src/stdlib.c` and `src/setjmp.c` are
linked into that image alone. The scheduler saves the XMM and x87 state of a
task at every switch (`docs/debts.md`, K9), so a program that computes in a
double keeps its registers when another one runs beside it - a child of a
`fork`, a program started by `execve`, or `xargs -P`.

### Lua: the second worked example

Lua 5.4.7 is the port that asks for everything the first one did not: a
`double`, a parser that long-jumps out of an error, a clock, a file it reads
as a script and a formatter with a precision. Its recipe is the one above with
three additions:

```cmake
microos_userland_library(microos_user_lua FLOAT
    SOURCES ${LUA_SOURCES}
    INCLUDES "${LUA_DIR}" "${LUA_DIR}/port"
    COMPILE_OPTIONS "-include" "${LUA_DIR}/port/luaconf_port.h" -Os)

add_libc_program(lua
    FLOAT                              # SSE and x87, no -mgeneral-regs-only
    LIBRARIES microos_user_lua
    MAIN "${LUA_DIR}/lua.c"            # the main of the release, not a copy
    DEFINES ULIBC_HEAP_SIZE=786432u
    SOURCES "${LIBC_USER_DIR}/src/math.c"
            "${LIBC_USER_DIR}/src/stdlib.c"
            "${LIBC_USER_DIR}/src/setjmp.c"
            "${LIBC_USER_DIR}/src/time.c"
            "${LIBC_USER_DIR}/src/locale.c"
            "${LIBC_USER_DIR}/src/ctype.c"
            "${LIBC_USER_DIR}/src/assert.c"
            "${LIBC_USER_DIR}/src/format_float.c"
    COMPILE_OPTIONS -DULIBC_WITH_FLOAT -include "${LUA_DIR}/port/luaconf_port.h" -Os)
```

`MAIN` is for a port whose program is a file of the release (the standalone
interpreter of Lua) and not a small file of `ports/port_x86_64/programs`.
`FLOAT` replaces the guest flags of a program with the ones without
`-mgeneral-regs-only`, and the same option is on the library: the archive of
a source with a `double` in it cannot be built with the integers only. The
configuration of the release is a force-included header and not an edit of
`luaconf.h`, so the next update of the tarball is a copy of its files and
nothing else.

The port runs: `make x86-smoke` prints `1024.0` and the lines of
`/etc/hello.lua`, and the image of 164 KiB is under `K_EXEC_MAX_IMAGE`
(8 MiB), which `sys_execve` reads in pieces of `KSYS_MAX_IO`.

### awk: a grammar that the build must not generate

The One True Awk is the third worked example and the first port that is an
interpreter with a grammar of its own: `awkgram.y` is yacc source, and the
build machine of this tree has no `yacc` and no `bison` (the requirements are
cmake, ninja, gcc and python3). The parser is therefore **generated once and
committed**: `awkgram.tab.c`, `awkgram.tab.h` and the `proctab.c` of
`maketab.c`, with the version of the generator and the two commands in
`third_party/awk/README.md`.

The generator is byacc and not GNU bison on purpose. The skeleton bison
writes into its output is GPL-3 with the special exception for the larger
work; the tree takes permissive code only and does not rely on an exception
to read a rule. byacc's output carries no license, and the program is a
permissive one at every byte.

The port layer answers two different questions:

* the C library of the source: `random`/`srandom` of POSIX, a `fcntl` that
  is a no-op, and the wide characters of the C locale (`MB_CUR_MAX` 1,
  `mbtowc`/`wctomb`, `towupper`/`towlower`). They live in `port/awk_os.c` and
  not in `components/libc/user`, because only this source asks for them
  today; the debt row says when they move. `strdup`, `strncasecmp`, `putc`,
  `bsearch`, `popen` and `pclose` are the library of a program's files and
  the program links them, because a name has one implementation in the tree;
* the platform: `system` says on the error descriptor that the machine has no
  shell and answers `-1` (the port of the release owns the name; the library
  has its own `system` now, and a port that drops the stub gets it); since 1.8
  `"cmd" | getline` and `print | "cmd"` reach the `popen` of the library,
  which is a pipe, a `fork` and the `execve` of the first word of the command,
  so the command runs. A script that reads a file with `getline < "name"` and
  writes one with `print > "name"` works through the VFS, as before: the smoke
  test checks the three sides (`docs/ports-table.md`).

`port/sys/wait.h` and `port/wctype.h` exist because the source includes
those headers and the library of a program has neither; the path of the
header is the port layer's, and the host build of the test takes the headers
of the host, so the two builds of the same sources are the two machines.


## 4. The program and the tests

The program is a small C file in `ports/port_x86_64/programs/` with
`main(argc, argv, envp)`, the library's include and the calls of `ulibc.h`.
It uses `ULIBC_STDOUT` (1) and `ULIBC_STDERR` (2); descriptor 0 is the input
of a redirect or a pipe, and a program at the prompt has none yet.

The tests of a port have two machines:

* **host** — `tests/cases/test_<name>.c`, the same upstream sources compiled
  with the headers of the host and the port layer, covering the library
  itself: the check values, the round trip, the edge of an empty input, a
  file of the host. Add the case to `tests/CMakeLists.txt`,
  `tests/include/tests/tests.h` and `run_tests()` in `tests/src/main.c`.
* **guest** — a fixture in the image (a fixed file, generated with
  `SOURCE_DATE_EPOCH` and no name of the host in it), the command in
  `tools/x86_smoke.py` and `tools/x86_disk_smoke.py`, and a check of the
  bytes. `zcat` is the example: `/etc/greeting.gz`, `wc -c` for the count and
  `cat` for the text, in `x86-smoke`, and the same file written in one boot
  and read in the next one in `x86-disk-smoke`.

A library of the guest is not tested by the host build of the library only:
the two builds are different compilers and different headers, and the
`open`/`errno`/`EAGAIN` shape is only exercised by the machine. A port is
done when `make test`, `make x86-smoke` and `make x86-disk-smoke` pass, and
`make integrity` is clean.

## 5. The license and the package

* `third_party/<name>/LICENSE` is the license of the upstream, unchanged;
* `README.md` says where the binary travels;
* `docs/licensing.md` has the row and the note;
* the image carries the license (`/etc/licenses/<name>` for the disk of
  x86-64), `tools/bundle.sh` copies it into a bundle (`LICENSE.<name>`), and a
  package that installs the command (`microos-progs` of a release) copies it
  beside the program. The license is not merged with the MIT file of the
  tree.

## 6. The SDK: a program built outside the tree

A program of this tree is built by this tree: CMake of the port, the archive
of `components/libc/user`, `tools/link_program.sh`. A program of somebody else
- the copyleft packages of `microos-progs` above all - is built **outside**
it, because the tree is MIT and GPL code never enters a link line of it (ADR
0025 §1, §6). That is what `make sdk` is for:

```bash
make sdk                       # build-sdk/microos-sdk/<version>/
make sdk DESTDIR=/tmp/sdk      # somewhere else
```

The directory is the version of `kernel/config.h` (the version of
`kernel_version()`), so two releases do not overwrite each other:

```text
<version>/include/                 ulibc.h, the POSIX headers, kernel/ksys.h
                                   and the closure of the ABI (exec.h, errno.h,
                                   types.h, vfs.h, task.h, signal.h), prog.h
<version>/lib/libmicroos_user.a    the library of a program (35 objects)
<version>/lib/crt0.o               `_start` of the program (src/start.c)
<version>/lib/program.ld           the addresses of the window
<version>/bin/mkprog.sh            the sections -> a MOX1 image
<version>/bin/link_program.sh      the link of the window, which calls mkprog
<version>/bin/microos_sign         the host tool of the signature (ADR 0020)
<version>/share/microos-toolchain.cmake   the CMake toolchain of a program
<version>/share/build-libc.sh      the recipe that built the archive
<version>/share/src/libc/          the sources of the archive (MIT)
<version>/share/examples/hello/    a program that builds with nothing else
<version>/share/README.md          the same recipe, for a person with the SDK
```

Nothing of the directory is a second copy of the tree that can drift: the
library is built during `make sdk` from the copy of the sources inside it, by
the same flags as a guest object of the port (`tools/sdk/build_libc.sh`),
with `-Werror`, so a warning in a header of a release stops the SDK of that
release.

A program is then a `CMakeLists.txt` with `add_executable(name file.c)` and
two commands:

```bash
cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/microos-toolchain.cmake
cmake --build build                  # build/name.mox
```

The toolchain sets `CMAKE_SYSTEM_NAME Generic`, takes the headers of the SDK
instead of the ones of the host (`-nostdinc`, with the compiler's own
directory for `stdarg.h`), compiles with the flags of a guest object
(`-Os -ffreestanding -mno-red-zone -march=x86-64 -fno-pic -fno-pie
-fno-stack-protector -ffunction-sections -fdata-sections`), and links the
objects with `crt0.o`, the archive and `lib/program.ld` through
`bin/link_program.sh` - which is the same script the port uses, with the same
retry of the address and the same `mkprog.sh`. `main(argc, argv, envp)` is the
program, the entry is `_start`, and there is no include path or library to
name.

Three options of the toolchain matter:

| Option | Effect |
|---|---|
| `-DMICROOS_FLOAT=ON` | the program's own numbers are doubles: no `-mgeneral-regs-only`, and the XMM and x87 units are the ones the port turns on for ring 3 and saves at every switch (`docs/debts.md`, K9) |
| `-DMICROOS_GC_SECTIONS=OFF` | keep every function of the archive the program named (the default drops what no call reaches) |
| `-DMICROOS_WARNINGS=OFF` | the sources of somebody else answer the warnings of their release |

The example is `examples/hello/`, and the SDK copies it to
`share/examples/hello/`; the image of it is 10 688 bytes - text 8 160, data
2 496, bss 263 376 (the 256 KiB arena of the library) - and the machine runs
it: `/bin/sdkhello one two` prints the count, the environment, the pid and the
lines of `/etc/setting` read through the streams of the library.

The limits are the ones of every image of the port and not of the SDK:

| Limit | Number | Why |
|---|---|---|
| the image | 8 MiB (`K_EXEC_MAX_IMAGE`) | the kernel reads the file into one block of the allocator |
| the bss | 1 MiB (`K_EXEC_BSS_MAX`) | the arena of `malloc` is a block of the bss |
| the arena | 256 KiB (`make sdk SDK_HEAP=`) | the default of `src/malloc.c`; a bigger one is `share/build-libc.sh --heap` |
| one read or write | 64 KiB (`KSYS_MAX_IO`) | the block of one system call |
| the window | 16 MiB | `0x02000000`-`0x03000000`, with a 64 KiB stack at the top |

A program that needs another arena, or no float path at all, rebuilds the
archive from the sources the SDK carries and links that instead:

```bash
<sdk>/share/build-libc.sh --src <sdk>/share/src/libc --include <sdk>/include \
    --out <sdk>/lib/libmicroos_user.a --crt0 <sdk>/lib/crt0.o --heap 786432
```

`--no-float` leaves the SSE and x87 sources out (no `%f`, no `strtod` of a
double) for a program of integers and a smaller image; `sscanf` still reads
the conversions that are not a double.

The SDK is the **library and the link**; it is not an image of a disk. The
disk of QEMU is written by this tree, which is where the image tool lives:

```bash
python3 tools/x86_image.py --image disk.img --build build-x86 \
    --mkfs build-host/tools/microos_mkfs --program /bin/hello=build/hello.mox
```

A package of `microos-progs` signs its file with `bin/microos_sign sign-file`
and the key of the repository, and the machine installs it with `pkg`
(ADR 0024, ADR 0025 §3). The SDK does not build for the board: a program of
the ESP32 has 8 KiB windows and shares the address space of the kernel (ADR
0018), and its images belong to the firmware build.

## The second port: ten tools of sbase

The second port (after zlib) is a set of commands: `third_party/sbase` with
`cat`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`, `xargs` and `sed`.
It is the case the recipe was written for - a **command**, not a library -
and the one that made the library of a program grow the POSIX families of
[`libc.md`](libc.md).

What the port took, and what a next command can copy:

* **The same recipe, the canonical name.** The tool is built by
  `microos_add_userland_program()` with `MAIN` naming the upstream file and
  the image named after the tool: `/bin/ls` is the `ls` of sbase. The small
  `ls`, `cat`, `grep`, `wc` and `head` of ADR 0016 are not built for the
  x86-64 port any more; `echo`, `mkdir`, `pwd` and `rm` stay because sbase
  does not replace them. A port that replaces a program updates the smoke
  test in the same commit, because the sentences of an error are a contract.
* **The library is the program's own.** The C library a tool expects is
  `components/libc/user` (family by family, above), and the two libraries of
  the collection (`libutf`, `libutil`) are a static archive built with the
  flags of the guest. The arena is `ULIBC_HEAP_SIZE`: 128 KiB for a filter,
  512 KiB for `sort` and `find`, which hold a file.
* **The image fits one read.** A program is read whole by the kernel in a
  block of `KSYS_MAX_IO` (64 KiB). The tools are between 28 and 48 KiB with
  the library (`-Os`); a port that grows past the block is `E2BIG` at the
  boot and the build has to shrink it, not the contract.
* **No floating point.** `-mgeneral-regs-only` is the rule of these tools, so
  upstream `sort -n` (two `double`s) does not compile. The port layer
  (`third_party/sbase/port/numcmp.c`) compares the two numbers as decimals;
  `ls -h` has the same story in `port/humansize.c`. This is the third hunk of
  a vendored source the tree has, and `README.md` of the directory names it.
* **The descriptors are four, and 1 and 2 are the console only while they
  are not open.** A recursive `grep` that held a directory open while it
  opened a child would take the place of the console; the port reads the
  names first and closes the directory before it visits them.
* **`execve` is synchronous.** `xargs` of upstream forks and executes in the
  child; here the call runs the program and returns when it has ended, so the
  port runs the command in its own process and reads the next batch after it
  (`-P` has nothing to do). A command that needs a real child process waits
  for the POSIX `fork` of K4.
* **The license travels.** `/etc/licenses/sbase` in the image,
  `LICENSE.sbase` in the x86-64 bundle, a row in `licensing.md`.

The tests: `tests/cases/test_userland.c` for the pure library (compiled for
the host, a `userland` test of its own), and in `x86-smoke` and
`x86-disk-smoke` the ten commands over the files of the image - `ls -la`,
`find -name`, `sort`, `grep -r`, `wc`, `tail`, `head`, `sed`, `cat` and
`ls | xargs`. The shell looks a bare name in `PATH` before its own table for
these ten, so the checks are the command a person types and not only the
path of the file.

## The third port: file and libmagic

The third port is a command **and** a library that reads a database of its
own: file 5.45 with libmagic (`third_party/file`, BSD-2-Clause). It is the
first one whose data does not fit, and the first one that asked for a kernel
change of its own (an image of up to 8 MiB, read in pieces of the transfer
edge, `K_EXEC_MAX_IMAGE`).

What it added to the recipe:

* **The data is a build step.** The release's database is about 10 MiB.
  `tools/magic_subset.py` chooses a subset of the Magdir files (the list is
  `third_party/file/magic/subset.json`) and `microos_magic_compile` compiles
  it with the library itself into `/etc/magic.mgc`; the text travels as
  `/etc/magic` and is the fallback when the compiled file is gone. A command
  whose data is somebody else's is a **selection** and a tool, not a copy of
  the whole.
* **No `mmap` is a configuration, not a port.** The library has a path that
  reads the compiled database into a block (`QUICK` off); the port leaves it
  off, and `magic_buffer()` is handed the bytes the program read. The one
  `read` of the database became a loop of 64 KiB pieces, because the ABI of
  this system refuses a larger count.
* **No floating point is a hunk of the library, not a flag.** The release
  matches magic types in `float` and `double`; a program of this port has no
  XMM state (`-mgeneral-regs-only`) and the formatter has no `%g`. The port
  compiles those paths out with `MICRO_OS_NO_FLOAT` and drops such rules from
  the database; the hunks are named in the README of the directory.
* **The arena is the budget.** The database is 359 KiB and the file buffer
  128 KiB of a 960 KiB arena; the encoding test of the release wanted 512 KiB
  for a 64 KiB window and the port gives it 16 KiB. A port whose library
  allocates is a port whose numbers are chosen against `ULIBC_HEAP_SIZE`.
* **The streams needed a block.** `grep` of sbase over the database was one
  system call per byte and the shell gave up after ten seconds; the read side
  of `FILE` now has a block of 512 bytes.

The tests are `tests/cases/test_file.c` for the host (the compiled database
and the text) and `/bin/file` in `x86-smoke` and `x86-disk-smoke`.

## The third port: SQLite

The third port is a library with an operating system of its own: SQLite
3.54.1 ([`third_party/sqlite`](../third_party/sqlite/README.md)) as a static
archive and `/bin/sqlite3` of the x86-64 image. It is the first case where the
source asks for a **file system** and not the calls of a program, and the
answer is the VFS of the port: `SQLITE_OS_OTHER=1` leaves SQLite without its
`unix` layer, and `port/sqlite_vfs.c` is the whole `sqlite3_vfs` and
`sqlite3_io_methods` on `open`, `read`, `write`, `lseek`, `unlink`, `stat`,
`getcwd` and `uptime`.

What the next port can copy:

* **A VFS, not a wrapper of `open`.** SQLite wants the twelve methods of the
  interface and answers with its own codes; the port is one file with two
  builds (the guest and the host test, `SQLITE_PORT_HOST`), and the only
  differences are the names of the calls. The host test is what proves the
  journal and the truncation, because the guest's table of descriptors is
  smaller.
* **No mmap and no shared memory.** `SQLITE_OMIT_WAL` is the flag; the
  rollback journal is the only one. The source compiles without a line of the
  mapping of a file.
* **`double` is the `FLOAT` option of the recipe.** SQLite's `REAL` is a
  `double`, so the program and its archive leave `-mgeneral-regs-only` out,
  the loader turns the two units on, and the library of the program gains
  `format_float.c`, `strtod.c` and `math.c`. The rest of `/bin` still never
  touches the registers.
* **The table of descriptors is the design.** Four of them: the console on 1
  and 2 (opened as `/dev/tty0` and `dup2`'d there, because the table is
  inherited from the stage of the shell and the numbers may be taken), the
  database on 0 (the script of stdin is read into the arena first and `stdin`
  becomes `fmemopen`), and 3 for a `.read`. The journal of a transaction is
  in memory because there is no fifth descriptor; the VFS still implements a
  journal file and the host test uses it (`docs/debts.md`).
* **The shell is the release and the port is a shim.** `shell.c` is compiled
  whole with its `main` renamed (`port/sqlite_shell.c`), and the program of
  the image is `programs/sqlite3.c`: the console, the input and the one
  `-cmd` of the port. The upstream file is not edited.
* **The license is a statement, not a text.** SQLite is public domain; the
  statement of the upstream travels as `LICENSE`, `/etc/licenses/sqlite` and
  `LICENSE.sqlite` in the bundle.

The tests: `tests/cases/test_sqlite.c` for the library and the VFS on a file
of the host, `make x86-smoke` for the program and `/etc/sql-*.sql`,
`make x86-disk-smoke` for a database written in one boot and read in the next.

## The checklist of the next port

1. Read the license; stop if it is copyleft.
2. Fetch the release, copy the sources, `LICENSE` and `README`.
3. Write `README.md`: version, source, files, port layer, binary, tests.
4. Add the shims the source needs to `components/libc/user/include` and the
   functions to `components/libc/user/src` — additively, with a comment that
   says why, and a test.
5. Write `port/<name>_os.c` and `port/zconf.h` (or the equivalent): the
   allocator, the `-1`/`errno` shape, the `EAGAIN` of a pipe, the headers the
   source asks for.
6. Add `microos_userland_library` and `add_libc_program` to
   `ports/port_x86_64/CMakeLists.txt`; build with `-Os`; check the image is
   under `K_EXEC_MAX_IMAGE` (8 MiB) and the bss under the megabyte of MOX1.
7. Add the program to `tools/x86_image.py`, with a fixture and its license.
8. Write the host test; add the smoke commands and the checks; run
   `make test`, `make x86-smoke`, `make x86-disk-smoke`, `make test-asan`,
   `make test-tsan`, `make test-smp`, `make smoke`, `make ci`,
   `make x86-all`, `make integrity`.
9. Update `docs/licensing.md`, `docs/wiki/programs.md`, `docs/roadmap.md`
   and `docs/debts.md`; the debt of a missing piece is a row, not a note in a
   commit.

## What this recipe does not cover

* **The board.** 8 KiB windows, no MMU, the kernel's address space; a
  program library for the board is a different problem (ADR 0018).
* **A dynamic linker and shared objects.** Every image is static.
* **An image larger than 8 MiB.** `sys_execve` reads it in pieces of
  `KSYS_MAX_IO` into one block of the allocator, and `K_EXEC_MAX_IMAGE` is the
  ceiling. A larger program needs the streaming of the image into the window.
* **`mmap`, `brk` and a range that grows.** A library that maps its data
  (`QUICK` of libmagic) takes the block path; the arena of the program is the
  ceiling and a library with a bigger database needs a smaller selection or a
  `brk` of the program.
* **A real `fork` of a library's child process.** It exists on x86-64: the
  child is a task with a copy-on-write image and returns 0 from the call, an
  `execve` of the child replaces that child, and `waitpid` (a pid or `-1`)
  reaps it and answers the status. `xargs -P` starts its commands that way.
  The board keeps the placeholder of a machine without an MMU, so a port that
  forks many children is a port of x86-64.
* **A package manager of its own.** `pkg` exists for the signed image
  (ADR 0024); a port of a program does not have to use it, but a package
  that ships one carries its license.
