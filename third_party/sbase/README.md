# sbase — ten Unix tools of somebody else

sbase is a collection of small Unix tools by suckless, released under the MIT
license. This tree carries ten of them, built as `/bin/sb-<tool>` of the
x86-64 port: `cat`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`,
`xargs` and `sed`. They are the first commands of the tree that were not
written for it, after `/bin/zcat` and its zlib.

* Upstream: <https://git.suckless.org/sbase>, `git clone` of the tip
* Commit: `c546c3a` ("rm: Don't ask for confirmation to remove symlinks")
* License: [`LICENSE`](LICENSE), MIT (the copyright of the authors is in it)
* Used in: `/bin/sb-cat`, `/bin/sb-grep`, `/bin/sb-head`, `/bin/sb-ls`,
  `/bin/sb-tail`, `/bin/sb-wc`, `/bin/sb-sort`, `/bin/sb-find`,
  `/bin/sb-xargs` and `/bin/sb-sed` of the x86-64 MicroFS image
* Built by: `cmake/userland.cmake` with the compiler and the flags of the
  guest (`-nostdlib`, `-ffreestanding`, no PIC, no SSE), against
  `components/libc/user` and not the libc of the host
* Recipe: [`docs/porting-programs.md`](../../docs/porting-programs.md)

## What is here

The tools of the subset and the two libraries of the collection they use:

```
cat.c grep.c head.c ls.c tail.c wc.c sort.c find.c xargs.c sed.c
arg.h compat.h util.h utf.h queue.h text.h fs.h
libutf/        the runes of wc, sort and sed (19 sources, runetype.h)
libutil/       the helpers of every tool (38 sources; a tool compiles the
               ones it calls)
port/          our port layer: numcmp.c, humansize.c, sbase_port.h
```

The manual pages of the ten tools (`*.1`) travel with them. The tools that
are not in the subset (`awk`, `tar`, `ed`, `cp`, `mv`, `sh` and the rest of
the collection) are not vendored: a source that is not compiled is a build
and a warning and buys nothing. `README` is the upstream note, unchanged; its
status table names the whole collection and not this subset.

## What is added (the port layer)

| File | What it is |
|---|---|
| `port/sbase_port.h` | the one declaration of the port (`sbase_numcmp`); force-included in every tool (`-include port/sbase_port.h`) |
| `port/numcmp.c` | `sort -n` without a `double`: the decimal order of two numbers |
| `port/humansize.c` | `ls -h` without a `double`: the size in the units of the collection |
| `.clang-format` | `DisableFormat: true`: the vendored files keep the style of the upstream, and `make integrity` does not rewrite them |

`aspell`-style tabs and 80 columns of the collection stay as they are; the
hygiene check of the tree skips `third_party/` for that reason (see
`tools/integrity_check.py`).

## The two hunks of the upstream source

The upstream files are **not** edited except for two places, and each is
recorded here so that an update is a diff to read:

1. **`sort.c`** — the comparison of `-n` used `strtod` and two `double`s. The
   guest of this system is built with `-mgeneral-regs-only` (the kernel saves
   no XMM state) and a `double` cannot cross a function of the guest. The two
   lines of `slinecmp()` now call `sbase_numcmp()` of the port layer; the
   `double x, y` declaration is gone.
2. **`xargs.c`** — the `spawn()` of upstream forks and executes in the child.
   The ABI of this kernel runs `execve` as a task of its own and returns when
   the program has ended, so the command is run in the process of `xargs`
   itself and the next batch follows it. The jobs are one at a time and `-P`
   has nothing to do; the port of a real fork is a debt of K4.

`grep.c` also has a hunk of ours: upstream has no `-r`, and the port adds it
(a directory is read with `opendir`/`readdir`, the names are collected and
the directory is closed before a child is visited). The reason of the close
is the four descriptors of a program: the descriptor 1 of the console is a
console only while it is not an open file.

## The library the tools expect

The two libraries of the collection ask for a POSIX C library: `opendir`,
`readdir` and `closedir`, `stat`/`lstat` in `struct stat`, `getpwuid` and
`getgrgid`, `localtime`/`strftime`, `fnmatch`, `regcomp`/`regexec`,
`getopt`, `getline`, `fmemopen`, `qsort` and the strings of POSIX. Those are
in `components/libc/user` now, and `docs/libc.md` lists them beside the
calls they build on.
