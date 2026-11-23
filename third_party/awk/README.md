# The One True Awk 20240728 on Micro OS

awk is the third program of somebody else in this tree, after zlib and Lua,
and the first one that is a language with a grammar: a lexer, a parser
generated from a yacc grammar, a regular expression engine of its own (it
does not use the `regex` of a C library), an evaluator that computes in a
`double`, and the library of the built-in functions. It is the `/bin/awk` of
the x86-64 image.

## The release

| | |
|---|---|
| version | 20240728 (28 Jul 2024), `version 20240728` in `main.c` |
| source | <https://github.com/onetrueawk/awk>, tag `20240728` |
| license | the Lucent permissive license, Copyright (C) Lucent Technologies 1997 (`LICENSE`) |
| upstream note | `README` (the release note of the repository, `README.md` there) |

The sources of the release are copied unchanged: `main.c`, `lex.c`, `b.c`
(the regular expression engine), `lib.c`, `run.c`, `tran.c`, `parse.c`,
`awk.h`, `proto.h`, and the grammar `awkgram.y`. What is not here is what the
build of this tree does not use: `maketab.c` (it runs once, below), the
manual page `awk.1`, `FIXES`, `REGRESS`, `testdir/`, `makefile` and
`bugs-fixed`. The upstream `README.md` is kept as `README` so that our note
can be `README.md`.

The release of 2024 handles UTF-8 and CSV input. This system is
byte-oriented and has one locale, the C one: the port sets `MB_CUR_MAX` to 1,
so `length`, `substr`, `match`, `split`, `sub`, `gsub` and the case of a
letter work on bytes, which is what POSIX says of awk in the C locale. `--csv`
is part of the release and works on the bytes.

One hunk of the release is edited, and it is whitespace only: nine lines of
`lex.c`, `lib.c` and `run.c` end with a space or a tab, and the hygiene check
of this tree refuses trailing whitespace in every file, the vendored ones
included (`make integrity`). The bytes are otherwise those of the release, and
the next update has a diff of nine lines to read.

## The generated parser

`awkgram.tab.c` and `awkgram.tab.h` are the output of **byacc 2.0
(20260126)** on `awkgram.y`, and `proctab.c` is the output of the
`maketab.c` of the release on that header. They are committed because the
build machine has no `yacc` and no `bison` (the requirements of this tree are
cmake, ninja, gcc and python3), and because the parser is a part of the
program that a reader may want to look at.

byacc is used and not GNU bison on purpose: the skeleton bison writes into
its output carries the GPL with a special exception, and this tree takes
permissive code only (`docs/licensing.md`). The output of byacc carries no
license of its own. The grammar has 44 shift/reduce and 85 reduce/reduce
conflicts; byacc resolves them the way bison does, and the host tests and the
smoke test run the parser the port ships.

To generate them again:

    byacc -d -b awkgram awkgram.y
    cc -o maketab maketab.c && ./maketab awkgram.tab.h > proctab.c

`awkgram.y` is the input and the only file a new release changes here.

## The port layer

| File | What it answers |
|---|---|
| `port/awk_port.h` | the configuration, force-included into every source: `MB_CUR_MAX` 1, `FOPEN_MAX` 8, the number of `SIGFPE` (the ABI has four signals and none of them is this one), the flag of `fcntl` that has no meaning here, and the declarations of the C library functions the port implements |
| `port/awk_os.c` | `strdup`, `strncasecmp`, `putc`, `bsearch`, `random`/`srandom`, `fcntl` (a no-op), `mbtowc`/`wctomb` (a byte is a character), `towupper`/`towlower` (ASCII), and the two calls of a shell: `system` and `popen` answer the failure with a sentence |
| `port/wctype.h` | `wint_t` and the two wide functions for `run.c`, which includes `<wctype.h>` |
| `port/strings.h` | `strncasecmp` for `lib.c`, which includes `<strings.h>` |
| `port/sys/wait.h` | `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`, `WTERMSIG`; `system` never returns a status on this machine, and the macros are compiled, not taken |

The functions of `awk_os.c` are declared in `awk_port.h` and not in
`components/libc/user`, because awk is the only source of the tree that asks
for them today and the layer of a port is the place for a function of one
program: the library would keep four names for one caller, and a branch that
adds `strdup` for another port would leave two definitions of one symbol in
an image. The row of the move into the library is in `docs/debts.md`.

Nothing of the release is edited. The two warnings of the call of the
library are the ones upstream C answers in every build (`-Wno-unused-parameter`,
`-Wno-unused-but-set-variable`); the note of the port in
`ports/port_x86_64/CMakeLists.txt` says so.

## The library of the program

`/bin/awk` is built with `-DULIBC_WITH_FLOAT` and without
`-mgeneral-regs-only`: an `Awkfloat` is a `double`, and the formatter of the
program writes `%e`, `%f` and `%g` through `components/libc/user/src/format_float.c`.
The runtime of the program carries `malloc`, `file`, `stdlib`, `math`,
`setjmp`, `time`, `locale`, `ctype`, `assert` and `format_float`; the arena
is `ULIBC_HEAP_SIZE=524288` (512 KiB of the bss; the format caps the bss at a
megabyte). The image is `-Os`: about 72 KiB of text, 99 KiB of data (the
tables of the parser and of the regular expression engine) and 515 KiB of
bss, 171 KiB in the file.

## The calls that are not there

This machine has no shell and no second process a program can wait for: the
library of a program has no `system`, no `popen` and no `waitpid`. The two
forms of awk that ask for one answer the failure:

| Form | Answer |
|---|---|
| `system("...")` | the line `awk: system: this machine has no shell` on the error descriptor, and the value `-1` |
| `print \| "..."`, `"..." \| getline` | the line `awk: popen: this machine has no shell and no second process`, and `-1` for the `getline`, `FATAL` for the redirect of `print` |

A script that reads a file with `getline < "name"` and writes one with
`print > "name"` works: that is `fopen` of the VFS. The debt is the row in
`docs/debts.md` and not a silent success.

## Tests

* host: `tests/cases/test_awk.c` builds the whole release for the host with
  the headers of the host and runs the `main` of the release in a child
  process for each case: the fields and `-F`, the arithmetic and the
  formatter, the arrays and `for (k in a)`, `sub`/`gsub`/`match`, `getline`,
  `-v`/`-f`, BEGIN/END, `next`/`nextfile`/`exit`, and the comparison of a
  numeric string with a number and with a string;
* guest: `make x86-smoke` runs `/bin/awk` on `/etc/columns` and
  `/etc/setting`, the script `/etc/hello.awk`, the refusals of `system` and
  of a pipe, and a script that is not there; `make x86-disk-smoke` runs the
  script out of the volume in the first boot and again after the reset.

## The license

`LICENSE` is the license of the release, unchanged: the Lucent permissive
text of 1997 (it is not the MIT of this tree and not the BSD-3-Clause of a
newer reading of the file). The image carries a copy at `/etc/licenses/awk`
beside `/bin/awk`, `tools/bundle.sh` puts `LICENSE.awk` into the x86-64
archive, and a package that installs the interpreter copies the same file
beside the program.
