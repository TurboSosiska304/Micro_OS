# PCRE2 10.42 for Micro OS

A vendored copy of PCRE2, the Perl-compatible regular expression library of
the University of Cambridge, for the programs of this tree. The library is
not part of the kernel and not part of `components/libc`: `pcre2grep` links
it into its own MOX1 image (`components/libc/user` supplies the allocation,
the strings and the `ctype` the library asks for).

## What it is

| | |
|---|---|
| Version | 10.42 (12 February 2024) |
| Source | <https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.42/pcre2-10.42.tar.gz> |
| Archive SHA-256 | `c33b418e3b936ee3153de2c61cc638e7e4fe3156022a5c77d0711bcbb9d64f1f` |
| License | BSD-3-Clause WITH PCRE2-exception, see `LICENSE` |

The archive is unpacked and `src/pcre2_*.c`, `src/pcre2_*.h`,
`src/pcre2.h.generic` and `src/config.h.generic` are copied as they are. The
files of `src/` are third-party code: `make format` and the hygiene checks of
`make integrity` skip `third_party/` on purpose, so a diff against an
upstream release stays readable.

## The configuration

`config.h` is `src/config.h.generic` with these choices, and no other:

* `SUPPORT_PCRE2_8` - the 8-bit library only. The 16-bit and 32-bit widths
  and the POSIX wrapper are not built.
* no `SUPPORT_JIT` - there is no executable memory of that kind under a
  program of this system, and the interpreter is the shape the machine
  supports.
* no `SUPPORT_UNICODE` - see "The size of 10.42" below. Unicode with the
  fixed tables of `pcre2_ucd.c` costs about 120 KiB of tables alone, and a
  program image may be at most `KSYS_MAX_IO` (64 KiB, `kernel/ksys.h`). The
  option is a documented debt, not a decision against Unicode.
* `LINK_SIZE 2`, `NEWLINE_DEFAULT 2` (LF), `PARENS_NEST_LIMIT 250`.
* `MATCH_LIMIT 1000000` and `MATCH_LIMIT_DEPTH 1000` - a tenth and a
  thousandth of the upstream defaults. The heap of a program is a few
  hundred kilobytes, and a pattern that would take more backtracking than
  this is a refusal, not a machine that appears to hang.
* `HAVE_MEMMOVE` - `components/libc/user/src/string.c` has it.
* `PCRE2_EXPORT` is empty, as in the generic file: the symbols are ordinary
  ones of the image.

`pcre2.h` is `src/pcre2.h.generic` (the public header of the release).
`pcre2_chartables.c` is `src/pcre2_chartables.c.dist`: the character tables
of the C locale that the library uses when the caller passes none.

## The size of 10.42, and the two passes that are left out

Ten-forty-two is the release that fits. The compile and match engine of
10.43 and later is about 73 KiB before the program itself, above the 64 KiB
a program image may be; 10.42 with the optimization passes below is about 60 KiB
(61,336 bytes) with `pcre2grep` and the library of a program in it, which leaves about 4 KiB
of the cap.

`pcre2_small.c` is a local addition and not an upstream file. It provides the
two analysis passes of the compiler with an empty answer:

* `auto_possessify()` rewrites a greedy repeat followed by a character that
  cannot match the repeat into an atomic one;
* `study()` computes the bitmap of the first code units and the minimum
  length of a subject.

Both only make a match faster; neither changes whether a pattern matches.
`pcre2_compile.c` treats a zero answer as success and carries on with the
unoptimized opcodes, and the matcher has the same behaviour when the bitmap
is absent. The unit test compiles the same sources with the same stubs, so
the engine the image carries is the engine the test checks.

The build also passes `-fno-jump-tables`: the switch tables of the compiler
are read-only data of the image, and code that tests a value with a branch
costs a few hundred bytes of text and saves about 3 KiB of data. The total is
what matters, and the cap counts both.

## The platform layer

`compat/` is not part of PCRE2. The library includes the standard headers by
name (`<string.h>`, `<ctype.h>`, `<stdlib.h>`, ...), and a program of this
system has no libc: `compat/` declares exactly the functions the library
calls, and the compiler finds them before the headers of the build machine.
The implementations live in `components/libc/user/src`:

| Header | Functions |
|---|---|
| `compat/string.h` | `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr` |
| `compat/ctype.h` | the ASCII `is*` classes and `tolower`/`toupper` |
| `compat/stdlib.h` | `malloc`, `calloc`, `realloc`, `free` |
| `compat/stdint.h`, `compat/inttypes.h`, `compat/limits.h` | the widths and the limits the header needs |
| `compat/stdio.h` | the debug formatter; no caller of a release build uses it |

A caller that compiles the library for the host (the unit test
`tests/cases/test_pcre2.c` does) does not put `compat/` on the include path:
the host libc is the platform there, and `config.h` has the same choices, so
the test and the image run the same engine with the same tables.

## What was changed

Nothing in the source of 10.42. The edits are `config.h`, which the release
itself says to copy and edit for a build without `configure`, and the
additions of this directory: `compat/`, `pcre2_small.c` and this README.

## Building it

The x86-64 port compiles the sources of `src/` for the window of a program
(`ports/port_x86_64/CMakeLists.txt`, `add_pcre2_program`):

```text
-ffreestanding -nostdlib -fno-pic -mno-red-zone -mgeneral-regs-only -Os
-ffunction-sections -fdata-sections -fmerge-constants -fno-jump-tables
-DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
-I third_party/pcre2 -I third_party/pcre2/src -I third_party/pcre2/compat
```

The tests of the host compile the same sources with the same `config.h` and
without `compat/`.
