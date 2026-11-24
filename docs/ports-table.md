# Patched lines per program

The table of lane M3 of 1.8 ([`plan-1.8.md`](plan-1.8.md)): what a program of
somebody else costs in this tree, in three numbers — the lines of its own
source, the lines of a port layer this system adds, and the lines of the
upstream source that had to be **edited**. The lane promises zero for the
console set, and `make ports-smoke` is the gate that says so on the machine
([`tests/ports/`](../tests/ports/)).

The recipe of a port is [`porting-programs.md`](porting-programs.md); the
programs of that recipe that are in the image are zlib, sbase, Lua, zstd, bc,
file/libmagic, SQLite and awk, and each of them has a port layer of its own.
This file is about the ones that have **none**.

## The method

A program is in the gate when all four of these are true:

1. its source is a release of a permissive project (sbase, MIT —
   [`licensing.md`](licensing.md), ADR 0025), copied byte for byte into
   `tests/ports/sbase/` and checked against the sha256 of the upstream commit
   (`tests/ports/upstream.txt`) before the machine boots;
2. it is built by **the SDK** — `make sdk`, then
   `tools/ports_build.sh`, then the CMake project of `tests/ports` — and by
   no CMake of this tree, so the port layer of the tree cannot reach it;
3. it links against `libmicroos_user.a` and the helpers of its own release
   (`libutil`), with no file of this tree on the link line;
4. it runs on the machine: the program is a file of `/bin/ports` of the
   volume and `tools/ports_smoke.py` types it at the shell and checks the
   answer of the release.

The lines of the port layer are therefore zero by construction, and the
patched lines are the number that the sha256 check keeps at zero. What the
table measures is the **library**: a program needs a port layer when
`components/libc/user` cannot answer a header or a call it includes.

## The console set: thirteen programs, zero patches

| Program | Source | Lines | Port layer | Patches | Image | What the machine answers |
|---|---|---:|---:|---:|---:|---|
| `true` | `true.c` | 6 | 0 | 0 | 4 096 | the status 0 and nothing else |
| `false` | `false.c` | 6 | 0 | 0 | 4 096 | the status 1 |
| `echo` | `echo.c` | 24 | 0 | 0 | 14 432 | its words, `-n` without a newline |
| `basename` | `basename.c` | 39 | 0 | 0 | 14 464 | `etc/setting` → `setting`, and `b` for `/a/b.txt .txt` |
| `dirname` | `dirname.c` | 27 | 0 | 0 | 14 432 | `/a/b/c` → `/a/b` |
| `pwd` | `pwd.c` | 50 | 0 | 0 | 14 592 | `/` through `getcwd` and a `stat` of `PWD` |
| `printenv` | `printenv.c` | 39 | 0 | 0 | 14 432 | `PATH` → `/bin`, and the whole environment |
| `sleep` | `sleep.c` | 30 | 0 | 0 | 14 400 | a sleep of no seconds and the status 0 |
| `cksum` | `cksum.c` | 132 | 0 | 0 | 16 704 | `1273462717 34 /etc/setting` (the CRC-32 of POSIX) |
| `tee` | `tee.c` | 60 | 0 | 0 | 14 592 | the stream to a file **and** the console |
| `cmp` | `cmp.c` | 82 | 0 | 0 | 14 656 | nothing for a file with itself; `differ: byte 1, line 1` and status 1 for two |
| `whoami` | `whoami.c` | 37 | 0 | 0 | 14 528 | `root` through `getpwuid` and `/etc/passwd` |
| `sha256sum` | `sha256sum.c` | 45 | 0 | 0 | 19 168 | the SHA-256 of `/etc/setting`, with `sha256.c` of the release beside it |
| **total** | 13 files | **577** | **0** | **0** | 171 KiB | 73 checks in `make ports-smoke` |

The helpers they call are the release's own: `libutil/eprintf.c`,
`fshut.c`, `putword.c`, `ealloc.c`, `strtonum.c`, `writeall.c`,
`sha256.c` and `crypt.c` (652 lines of sources and 214 of headers, unchanged,
and only the members a program names enter its image). Nothing in the column "port
layer" is a file of this tree: `CMakeLists.txt` of `tests/ports` names the
sources and the link, and that is the whole of what the SDK does not do
itself.

Why zero holds for these thirteen, in one sentence each:

* **Their numbers are integers.** A program of the guest is built with
  `-mgeneral-regs-only` (no XMM state is saved at a switch, `docs/debts.md`
  K9), so a `double` in the source is a compile error — `seq`, `printf` and
  `sort -n` are the ones that ask for one, and these thirteen do not.
* **They are one program and no child.** `xargs` of the collection forks and
  executes; these thirteen call `fork` only through `popen` of the library,
  and a library that forks is not a patch of the upstream (it is the port of
  `popen` itself).
* **They ask the calls the library has.** `open`/`read`/`write`/`lseek`/
  `close`, `stat`, `getcwd`, `getenv`, `getpwuid`, `sleep`, `signal`, the
  streams of `<stdio.h>` and the CRC of a table — every one of them is in
  [`libc.md`](libc.md), and the table there is the one this table pays for.
* **Their paths are the paths of this system.** A name is short, a file has
  an owner and a mode, `.` and `..` are folded by the resolver of the
  library: nothing of the thirteen walks a tree or links a file.

## The terminal set: a program with a screen, zero patches

The console set is a program that answers a line with a line. The second set
of the same gate is a program that owns the screen, and it is where a system
without a terminal gives itself away: every editor of a Unix starts with
`isatty`, `tcgetattr`, `cfmakeraw` and `ioctl(TIOCGWINSZ)`, and a machine that
answers "no" to the first one never sees the program at all.

| Program | Source | Lines | Port layer | Patches | Image | What the machine answers |
|---|---|---:|---:|---:|---:|---|
| `kilo` | `kilo.c` | 1 308 | 0 | 0 | 245 KiB of image | the screen of the console, a line typed, Ctrl+S, Ctrl+Q, and `cat` reads the file |
| `ttyprobe` (ours) | `ttyprobe.c` | 100 | — | — | 14 KiB | `isatty` 0/1 yes and 9 no, canonical, the size, raw, and back |
| **total** | 1 file of the release | **1 308** | **0** | **0** | 259 KiB | 12 checks of `make tty-smoke`, 8 of `make ports-smoke` |

`kilo` is the editor of Salvatore Sanfilippo (`antirez/kilo`, commit
`323d93b`), copied byte for byte into `tests/ports/kilo` with its `LICENSE`
and the sha256 of both in `tests/ports/upstream.txt`; the note is
[`tests/ports/kilo/README.md`](../tests/ports/kilo/README.md). It is built by
the SDK and by no CMake of the tree, like the thirteen tools of sbase, and the
one warning of its release (`-Wunterminated-string-initialization` on the
`"//"` of its C syntax table) is the release's and not a failure of the gate
(`-DMICROOS_WARNINGS=OFF`).

The gate has two halves because a transcript cannot type at an editor:

* **`make ports-smoke`** builds kilo, writes it into `/bin/ports` of the
  volume, hashes its source like the sources of sbase, and checks that the
  file is there. It also runs `ttyprobe`, which asks the terminal in words a
  transcript can read: the four questions of a full-screen program, the size
  of the screen, the raw mode and the mode that comes back.
* **`make tty-smoke`** drives the keyboard of the machine: it types
  `exec /bin/ports/kilo /tmp/k.txt`, waits for the screen, types a line, sends
  Ctrl+S (save) and Ctrl+Q (quit), waits for the shell, and reads the file
  back with `cat`. The line of the file is the proof of the editor and of the
  terminal under it.

The second program of the table is ours and is the other half of the same
proof: `tests/ports/ttyprobe.c` is a file of this tree (as `command.c` is) and
it is the terminal asked with the calls of the library, in the words of a
transcript. It is not a port of anything and it is not in the count of zero
patches.

Why zero holds for an editor, in one sentence each:

* **Its numbers are integers.** `-mgeneral-regs-only` is the rule of a program
  of `/bin`, and the editor of 1 308 lines never computes in a `double`.
* **It asks the calls the library has.** `isatty`, `tcgetattr`, `tcsetattr`,
  `cfmakeraw`, `ioctl`, `read`, `write`, `open`, `ftruncate`, `exit`,
  `atexit`, `time`, `snprintf`, `sscanf` and `malloc` are in
  [`libc.md`](libc.md) with ADR 0051.
* **It asks for a terminal and the machine has one.** The mode of the console,
  the echo, the signal of Ctrl+C, the raw read and the size of the screen are
  the subject of ADR 0051; the escape sequences it draws with are read by the
  console of x86-64.
* **Its source is the release.** The 1 308 lines are the upstream bytes; a hunk
  that somebody edited to make the editor compile would fail the sha256 check
  before the machine boots.

## The ten tools of the tree: where zero does not hold, and why

The ten sbase tools that 1.2 vendored ($2 of the recipe) are the same
collection, and they are built by the CMake of the tree with a port layer.
This is the other side of the number, and it is measured against the same
upstream commit:

| Program | Lines | Port layer | Patched lines | Why it is not zero |
|---|---:|---:|---:|---|
| `cat` | 52 | 0 | 0 | — |
| `head` | 77 | 0 | 0 | — |
| `tail` | 229 | 0 | 0 | — |
| `wc` | 122 | 0 | 0 | — (asks `libutf` for the runes) |
| `sed` | 1 738 | 0 | 0 | — (asks `libutf`) |
| `find` | 1 103 | 0 | 0 | — (asks `libutil/recurse.c`) |
| `sort` | 435 | `port/numcmp.c` 173 | 5 | `-n` compares two `double`s upstream; the guest has no XMM, so the order of two decimals is a comparator of ours |
| `ls` | 549 | `port/humansize.c` 65 | 60 | `-h` is the same `double` rule; and `--color` is an option the release does not have (a directory blue, an executable green) |
| `grep` | 361 | 0 | 99 | upstream has no `-r`: the recursive walk is a feature the port adds, not a defect it works around |
| `xargs` | 374 | 0 | 15 | upstream forks and executes in the child, and when the port was written `execve` was synchronous and `fork` did not exist. Since K4 (1.5–1.7) it does: **these 15 lines are a debt that 1.8 can pay** |

Six of the ten are at zero, and the four that are not are two kinds of thing:
two hunks of the **machine** (a `double` in a program that has no XMM) and two
hunks of **an option the release never had** (`grep -r`, `ls --color`). The
first kind is a debt of the library (a `strtod`-shaped comparator cannot be
shared without the XMM path), the second is a feature and not a port — a
`grep -r` that the collection does not ship is this system's own command and
belongs to it.

## The programs that cannot join yet

Measured with the same compiler and the same SDK, and every one of them is a
row in [`debts.md`](debts.md):

| Program | What it asks | Where it fails |
|---|---|---|
| `uname` | `<sys/utsname.h>` and `uname()` | the library has no such header, and the ABI has no call for the name of the machine; the version is `/proc/version` |
| `seq` | `double` | `SSE register return with SSE disabled`: `-mgeneral-regs-only` (the same debt as `sort -n`) |
| `printf` of the collection | `double` for `%f` | the same line, the same debt |
| `date` | `clock_settime()` | `ENOSYS`: the wall clock of the ABI is read-only (and the time zone is a file of `/etc`) |
| `sync` | `sync()` | `ENOSYS`: the VFS has no flush of the whole tree and no call |
| `ln` | `AT_SYMLINK_FOLLOW` and a symbolic link | `symlink()` is `ENOSYS`: a link is not an object of MicroFS |
| `mkdir`, `rm`, `cp`, `mv` | `libutil/recurse.c`, `rm.c`, `cp.c` | they compile unmodified, and the link needs the helpers of the collection that this project does not carry yet: the next four names of the gate |
| `fold`, `cut`, `tr`, `expand`, `unexpand`, `comm`, `tsort`, `md5sum` | `libutf`/`libutil` and nothing else | they compile unmodified: the second batch of the gate |

The first six are the four additions of the library that the lane named
(`malloc` from `brk`, `chdir`, the wall clock, buffered streams) plus the
`double`: `malloc`, `chdir` and the wall clock are **done** in 1.8 and the
`double` and `uname` are the ones that are not.

## The three calls of a command

`popen`, `pclose` and `system` are not a patch of anything: they are the
library (`components/libc/user/src/popen.c`, `system.c`,
`words.c`, and the move of a descriptor off the console in `syscall.c`), and the port of the tree that uses them is awk —
`"cmd" | getline` runs a program of `/bin` since 1.8 and `make x86-smoke`
checks the line. The gate checks them on the machine with a program of this
directory (`tests/ports/command.c`), which starts the tools above: the status
of `false` is 1, of a name that is not there 127, `popen("…", "r")` reads what
the command wrote and `popen("…", "w")` feeds `tee` the bytes it writes into a
file of the volume.
