# Porting a program to Micro OS

This is the recipe of a program that is somebody else's: a library and a
command of a Unix-like system that were written for a C library and a POSIX
kernel, built with the compiler and the library of this system and run in the
window of a program. The first one is zlib and `/bin/zcat`
([`third_party/zlib/README.md`](../third_party/zlib/README.md)); the next
ones are Lua, the tools of sbase and SQLite, and the recipe is written so that
they are copies of a shape and not new research.

It is not the recipe of a **port of Micro OS** to another machine: that one is
[`porting.md`](porting.md), and it is about the contract of `kernel/port/`.

## What a port of a program can be

A program of Micro OS is a static `MOX1` image linked for the window of the
machine, entered at `_start`, which reaches the kernel only through
`int $0x80` on x86-64 (ADR 0006, ADR 0016). There is no dynamic linker and no
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
| `<string.h>` | `strlen`, `strcmp`, `strncmp`, `strstr`, `strcpy`, `strncpy`, `strcat`, `strchr`, `strrchr`, `memcpy`, `memmove`, `memset`, `memcmp`, `memchr` |
| `<stdlib.h>` | `malloc`, `calloc`, `realloc`, `free`, `exit`, `EXIT_SUCCESS`, `EXIT_FAILURE` |
| `<stdio.h>` | `printf`, `puts`, `snprintf`, `vsnprintf`, and the streams `fopen`, `fclose`, `fread`, `fwrite`, `fgetc`, `fputc`, `fgets`, `fputs`, `feof`, `ferror`, `clearerr`, `fflush`, `fseek`, `ftell`, `rewind`, `stdin`, `stdout`, `stderr` |
| `<errno.h>` | the POSIX numbers and `errno`, which is a plain global of the program |
| `<unistd.h>` | `read`, `write`, `lseek`, `close`, `unlink`, the three `STD*_FILENO` and `off_t`/`ssize_t` |
| `<fcntl.h>` | `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_DIRECTORY` |
| `<sys/types.h>` | `off_t`, `ssize_t`, `off64_t` |

The allocation is an **arena in the bss of the program**, not
`KSYS_MALLOC`: the block of the kernel is memory of ring 0 and the page
tables of a program do not map it with the user bit. The size is
`-DULIBC_HEAP_SIZE=<bytes>` (256 KiB by default, 512 KiB for `zcat`), and
the bss of an image is capped at 1 MiB by the format. A library that
allocates more needs a program with a larger arena, or a `brk`/`mmap` of the
program, which the kernel does not have yet (`docs/debts.md`).

The formatter has `%d %i %u %o %x %X %c %s %p %%`, the flags `-` and `0`
and the length `l`/`z`. It has **no floating point** and no precision: a
library that prints a float, or a `%.3s`, needs a formatter of its own or a
change of `components/libc/user/src/stdio.c` (a debt).

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

* **A pipe answers `EAGAIN`** while its producer is still there (ADR 0014).
  zlib would read a negative result as the end of the file; the wrapper of
  `read` waits the condition out. A library with a `select` loop needs the
  descriptor of the pipe and a `sleep`/`yield`; this system has `sleep` in
  the ABI and no `poll` yet.
* **The image has a size budget.** The kernel reads a program whole into one
  block of `KSYS_MAX_IO` (64 KiB) before the port copies it to the window. The
  text and data of the program and its libraries have to fit that; `-Os`
  took `zcat` from 80 KiB to 58 KiB. A larger program needs the streaming or
  mapping of the image, which is a debt.

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
window, links them with `-nostdlib -static -no-pie` and `programs/program.ld`
at `0x02000000 + 32`, and runs `tools/mkprog.sh --base 0x02000000 --arch
x86-64`. The resulting `.mox` is a file of `/bin` in the image; the program
is added to `tools/x86_image.py` (the `PROGRAMS` list) and to the checks of
the smoke tests.

The guest flags are the ones of the kernel and of the existing programs:
`-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone
-mgeneral-regs-only -march=x86-64 -fno-asynchronous-unwind-tables
-fno-merge-constants -Wall -Wextra -Werror`. No `-mcmodel=kernel` (that one is
the kernel's), and no libc of the host: a symbol that the archive cannot
resolve is a link error and not a silent call into glibc.

`-mgeneral-regs-only` means no floating point and no SSE in the guest. A
library that computes in `double` cannot be built this way until the port
saves the XMM state; that is a debt, not a flag to remove.

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
   under `KSYS_MAX_IO`.
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
* **Floating point.** `-mgeneral-regs-only` is the rule of the port.
* **A real `fork` of a library's child process.** The system has `fork` on
  x86-64, but no `execve` of a path that a library resolves in `PATH` and no
  `waitpid` in the library yet; a library that starts a helper is a later
  step.
* **A package manager of its own.** `pkg` exists for the signed image
  (ADR 0024); a port of a program does not have to use it, but a package
  that ships one carries its license.
