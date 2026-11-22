# bc and dc 7.2.0 on Micro OS

`bc` is the POSIX calculator of arbitrary precision, and `dc` is the same
engine in reverse Polish notation. This is Gavin D. Howard's implementation
of the two, the third library of somebody else in this tree, after zlib and
Lua. It is the first port whose program does its own arithmetic on numbers
larger than a machine word, and the first whose input is a language of its
own (a script, a function, an array, a string and a comment).

## The release

| | |
|---|---|
| version | 7.2.0 (`VERSION.txt`) |
| source | <https://github.com/gavinhoward/bc>, branch `master`, commit `1a8e2e6` of 19 Oct 2026 |
| license | BSD-2-Clause, Copyright (c) 2018-2026 Gavin D. Howard and contributors (`LICENSE`) |
| upstream note | `README.md` and `NEWS.md` of the repository, not vendored |

`LICENSE` is the text of the release's `LICENSE.md`, byte for byte. The
release's `README.md`, `NEWS.md`, `manuals/`, `tests/`, `benchmarks/`,
`locales/`, `scripts/`, `vs/` and the two build systems (`configure.sh`,
`Makefile.in`) are not here: the build of this port is
`cmake/userland.cmake`, and a reader who wants the manual or the test suite
fetches the release.

What is included:

| Path | What |
|---|---|
| `include/` | the headers of the release, unchanged |
| `src/` | the calculator, unchanged: the two parsers, the two lexers, the VM, the numbers, the vectors, the options, the reader and `main.c` |
| `gen/lib.bc`, `gen/lib2.bc` | the two scripts of the math library `-l`, unchanged |
| `gen/bc_help.txt`, `gen/dc_help.txt` | the help texts of `-h`, unchanged |
| `port/` | the layer of this system, below |

`src/bc_fuzzer.c`, `src/dc_fuzzer.c`, `src/history.c`, `src/library.c` and
`src/bcl.c` are left out: there is no fuzzer, no line editor and no `bcl`
library in this port, and their `#if` guards would make them empty files.

## The port layer

The release has no configuration header: `configure.sh` writes a `Makefile`
and the settings travel as `-D` flags on the command line of the compiler.
`port/bc_port.h` is force-included into every source and names the same
settings, so the release's sources are untouched and the next update is a
copy of its files:

| Setting | Value and why |
|---|---|
| `BC_ENABLED`, `DC_ENABLED` | 1 and 1: one engine and two names of `/bin`; `main` reads the name it was entered with |
| `BC_ENABLE_EXTRA_MATH` | 1: the GNU extensions of the release are part of the language |
| `BC_ENABLE_HISTORY`, `BC_ENABLE_EDITLINE`, `BC_ENABLE_READLINE` | 0: this machine has no terminal behind descriptor 0 and the port does not carry a line editor |
| `BC_ENABLE_NLS` | 0: no `gettext`, no catalog, the messages are the strings of the release |
| `BC_ENABLE_LIBRARY` | 0: `bcl` is not built |
| `LONG_BIT`, `BC_LONG_BIT` | 64: the limb is the 64-bit `long` of x86-64. GCC's freestanding `<limits.h>` does not define `LONG_BIT`, and the release would silently fall back to 32-bit limbs |
| `NDEBUG` | 1: the release's non-debug build (`-DNDEBUG` of `configure.sh -O2`) |
| `BC_DEFAULT_*` | the defaults of that build: no banner, a prompt that an interactive run never reaches, an expression that ends the program |

`port/bc_os.c` and `port/sys/stat.h` answer the two calls the release takes
from POSIX and this machine does not have:

* `fstat` of a descriptor: the ABI has `stat` of a path and no call of an
  open descriptor. The port takes the size from `lseek(fd, 0, SEEK_END)` and
  names the object a regular file. A directory given to `-f` is read as an
  empty file and not refused; `docs/debts.md` has the row;
* `sigaction`: the kernel has `signal` and four signals. The port keeps the
  POSIX structure for the release and calls `signal`; a signal the system
  does not know is refused, and the release ignores that answer.

`port/libgen.h` is an empty header: `src/main.c` includes `<libgen.h>` on
every system that is not Windows and calls nothing of it (the name of the
calculator is cut from `argv[0]` by the release itself).

`port/generated/` holds the four C files the release's `Makefile` makes from
`gen/` with its own `gen/strgen.c`: `bc_help.c`, `dc_help.c`, `lib.c` and
`lib2.c`. They are committed because both builds of this tree (the host tests
and the x86-64 image) need them and neither has the release's build system;
the command that regenerates them, from this directory, is

```sh
cc -O2 -I include -o /tmp/strgen ../path/to/bc/gen/strgen.c
cd gen
/tmp/strgen lib.bc    ../port/generated/lib.c    0 bc_lib  bc_lib_name BC_ENABLED 1 "" "" 1
/tmp/strgen lib2.bc   ../port/generated/lib2.c   0 bc_lib2 bc_lib2_name "BC_ENABLED && BC_ENABLE_EXTRA_MATH" 1 "" "" 1
/tmp/strgen bc_help.txt ../port/generated/bc_help.c 0 bc_help "" BC_ENABLED 0
/tmp/strgen dc_help.txt ../port/generated/dc_help.c 0 dc_help "" DC_ENABLED 0
```

The names of the two scripts come out as `lib.bc` and `lib2.bc`, not as the
path of the machine that built the image, so two builds of the tree write the
same bytes.

## The library of the program

The port found two functions missing from `components/libc/user` and added
each one as a file of its own, so that a program that does not call it does
not carry it:

* `src/strdup.c`: a copy of a text in the arena (`vm.c` copies a name);
* `src/rand.c`: `srand`/`rand`, the fallback of the release's generator of
  random digits when `/dev/urandom` cannot be read (there is no `/dev/urandom`
  in the `devfs` of this machine, so `irand()` and `$` use the sequence).

The two headers that use `__attribute__((noreturn))` now spell it
`__attribute__((__noreturn__))`: the release includes `<stdnoreturn.h>`,
which defines `noreturn` as `_Noreturn`, and the plain spelling becomes an
attribute name the compiler does not know.

The program links `src/malloc.c`, `src/stdlib.c`, `src/setjmp.c`,
`src/ctype.c`, `src/time.c`, `src/strdup.c` and `src/rand.c` of the library.
`stdlib.c` is the whole file (it also holds `strtod` and `qsort`), and a
`double` in it is why the image is built with the `FLOAT` option of the two
functions of `cmake/userland.cmake` and without `-mgeneral-regs-only`; the
port of this machine saves the XMM state for no task and runs one program at
a time (`docs/debts.md`). The arithmetic of bc itself is in its integer limbs.

## The image

`/bin/bc` and `/bin/dc` are the same image bytes: the name of the program is
`argv[0]` and the engine is picked by it. At `-Os`:

| | text | data | bss | file |
|---|---|---|---|---|
| `bc.mox`, `dc.mox` | 81,888 | 33,520 | 797,312 | 115,440 |

The bss is the arena of the program (`ULIBC_HEAP_SIZE=786432`) and the state
of the calculator; it is under the 1 MiB cap of MOX1, and the file is under
`K_EXEC_MAX_IMAGE` by two orders of magnitude. The arena holds the numbers of
a script, the bytecode of the math library of `-l` (about 9 KiB of source)
and the strings.

## What works, and what the machine does not give it

The release works: arbitrary precision, `scale`, `ibase`/`obase`, `sqrt`,
`^`, the math library of `-l`, `if`, `while`, `for`, `define`, arrays,
strings, both comments, `-e`, `-f`, a script named on the command line and a
script on descriptor 0; `dc` is the same engine with its stack and its RPN
commands. The POSIX mode `-s` is the release's.

What the machine does not give it:

* **descriptor 0 at the prompt.** The shell does not pass a console to a
  program yet (`docs/debts.md`), so `bc` with no redirect reads `EBADF` after
  its files and exits with a fatal I/O error. A script on the command line or
  of `-f` needs a redirect of descriptor 0 (`< /dev/null` or `< script`);
  `bc -e` does not, because it ends before the read of stdin. This is the
  shell's debt, not bc's;
* **a terminal.** `isatty` is 0 for every descriptor, so the release runs in
  its non-interactive mode: no prompt, no line editing, no history. The
  interactive loop on the console would work the same way, line by line,
  without the editing;
* **a directory as an argument.** `fstat` cannot see the type of an open
  descriptor; `-f /etc` reads an empty file instead of refusing it.

## Tests

* host: `tests/cases/test_bc.c` builds the release for the host
  (`microos_bc_host`) and runs it as a process: the arithmetic, `scale`, the
  two bases, `sqrt`, `-l`, the language (`define`, `for`, `if`, an array, a
  string, both comments), the POSIX `-s`, a run-time error and its status, a
  script of descriptor 0, a script of `-f`, a script named as an operand, and
  `dc`;
* guest: `make x86-smoke` runs `bc -e 2^100`, `bc -l -e scale=10 -e 4*a(1)`,
  the two bases, the script `/etc/bc.bc` through `-f` and through descriptor
  0, `/etc/dc.dc`, and `dc -e 2 -e 3+p`; `make x86-disk-smoke` runs the same
  scripts in the first boot and after the reset.

The line `scale=10; 4*a(1)` prints `3.1415926532`, not the `3.1415926535` of
a value rounded at the last place: the math library of the release (as the
one of GNU bc) truncates its constant of pi/4 at the scale of the call, and
`4` times ten digits of that constant is `3.1415926532`. That is the answer
of the release and of GNU bc on the same line; a port that printed another
number would have changed the language.

## The license

`LICENSE` is the BSD-2-Clause text of the release, unchanged. The image
carries a copy at `/etc/licenses/bc` beside `/bin/bc` and `/bin/dc`,
`tools/bundle.sh` puts `LICENSE.bc` into the x86-64 archive, and a package
that installs the two calculators copies the same file beside them. The text
is not merged into the MIT `LICENSE` of the tree.
