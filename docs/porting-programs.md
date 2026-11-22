# Porting a program, and a library under it

A program of Micro OS is a file of a volume: `execve` reads it through the
VFS, checks it against the MOX1 format and hands it to the port, which copies
it into the window of the machine. `docs/wiki/programs.md` is the guide of a
program and its entry; this file is the list of the places a new program
touches, and the rules a program with a third-party library has to keep.

## The five places of a program of x86-64

1. `ports/port_x86_64/programs/<name>.c` — `main(argc, argv, envp)`, with
   `#include "ulibc.h"`.
2. `ports/port_x86_64/CMakeLists.txt` — `add_program_image()` for a program
   that talks to the gate through `programs/prog.h`, or `add_libc_program()`
   for one over the library. The function compiles for the window, links with
   `programs/program.ld`, and turns the file into a `.mox` with
   `tools/mkprog.sh`.
3. `tools/x86_image.py` — the `PROGRAMS` list, which is the `/bin` of the
   disk image.
4. `tools/x86_smoke.py` — a command of the shell, and the checks of its
   answer. A program without a line there is a program no machine has run.
5. Documentation: a row of the `/bin` table in `docs/wiki/programs.md`, and
   the count of the images in that page and in `docs/roadmap.md` when it
   changes.

The library of a program is `components/libc/user`: `_start`, the syscall
wrapper, `printf`/`puts`, the strings, and since the PCRE2 work `malloc` and
the ASCII `ctype`. `components/libc` (without `/user`) is the library of the
kernel and is not linked into a program.

## The one hard number: 64 KiB

A program image is read into one block of `kmm` and the edge of the syscall
refuses a file over `KSYS_MAX_IO` (65536 bytes, `kernel/ksys.h`; the debt is
in `docs/debts.md`). The window of x86-64 is 16 MiB, so the limit is the
transfer and not the address space: an image may not be larger until the
kernel streams it or the ABI grows a second call.

That number decides what a library can be. The plan for a vendored component
is:

* compile for size (`-Os`, `-ffunction-sections`, `-fdata-sections`,
  `-fmerge-constants`, `-fno-jump-tables`) and link with `--gc-sections`;
* drop every feature whose tables are bigger than the program: Unicode, the
  JIT, the 16/32-bit widths;
* leave out the optimization passes that do not change whether a pattern
  matches if the engine is still too large;
* measure the `.mox` and write the number down. A margin of a few hundred
  bytes is not a margin: two compilers disagree by more than that.

`/bin/pcre2grep` is the worked example: PCRE2 10.42, 8-bit, no JIT, no
Unicode, two optimization passes stubbed, about 60 KiB of the 64 KiB cap (61,336 bytes of 65,536).
The record is `third_party/pcre2/README.md`.

## A vendored library: the shape

A component lives in `third_party/<name>/` and has:

| File | What it is |
|---|---|
| `src/` | the upstream sources, unmodified |
| `config.h` | the release's own template, edited for this tree; the choices are written in the README |
| `compat/` | the standard headers the library includes, declared over `ulibc` (string, ctype, stdlib, stdint, limits, stdio) |
| `LICENSE` | the upstream text, unchanged |
| `README.md` | version, source URL, archive digest, configuration, every local addition |
| `pcre2_small.c` (for PCRE2) | the local file, if there is one, with a comment that says why |

The build of the program names the sources one by one instead of a glob, so
a file that upstream adds is a decision and not a surprise. The test of the
host compiles the same sources with the same `config.h` and without
`compat/`: the host libc is the platform there, and the test is the second
caller that keeps the configuration honest.

## The library of a program grows only with a caller

`components/libc/user` is not a standard C library. A function is added when
a program of this tree calls it, with a test of its own:

* the strings are in `src/string.c` and `ctype.c`;
* the allocation is `src/malloc.c`: a first-fit list over a static block of
  bss (256 KiB by default, `ULIBC_HEAP_SIZE`), because a program has no
  `brk` and no `mmap`;
* `src/stdio.c` is the formatter of a program: `%d`, `%i`, `%u`, `%x`, `%X`,
  `%c`, `%s`, `%p`, `%%`, a width, `-`, `0`, `l` and `z`; no precision and
  no `*`. A program that is close to the 64 KiB cap may write its numbers
  with `write` and skip the formatter, as `pcre2grep` does;
* the calls of the ABI stay in `src/syscall.c`; a new call is a change of
  `kernel/ksys.h` and of the ABI, not of a program.

## The test of a library

The test lives in `tests/cases/test_<name>.c` and covers the behaviour the
program leans on, not every function of the library:

* the choice of the configuration, read back through the library
  (`config` values, version);
* the result a program relies on (a match, a group, an anchor);
* the edge (a pattern that does not compile, a subject that does not match);
* one case for the size choice, so a later release cannot quietly undo it
  (for PCRE2: a UTF-8 subject is bytes, `PCRE2_UTF` is refused).

The four touches of a new case are the ones of every test: the file, the
declaration in `tests/include/tests/tests.h`, the call in
`tests/src/main.c`, and the file in `tests/CMakeLists.txt`. Third-party
sources in the test target are compiled with `-w`: their warnings are an
upstream matter.

## The license

A vendored component keeps its license in its directory, its name in
`docs/licensing.md`, a copy in the bundle of `tools/bundle.sh`, and — when
the machine carries the component at run time — a file under `/etc/licenses`
of the disk image. The MIT of the tree does not cover a byte of it.
