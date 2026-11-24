# The gate of a program nobody rewrote

This directory is the proof of lane M3 of 1.8: a program of POSIX builds with
the SDK of this system **without a line of its source being edited**, runs on
the machine and answers what the release answers. `make ports-smoke` is the
gate; the numbers of it are in [`docs/ports-table.md`](../../docs/ports-table.md).

```
tests/ports/
    sbase/           the release of sbase, byte for byte (MIT)
      LICENSE          the license of the release, unchanged
      true.c echo.c cmp.c cksum.c ...   the thirteen tools of the gate
      util.h arg.h compat.h             the headers the tools include
      libutil/         the helpers the tools call (eprintf, fshut, putword,
                       ealloc, strtonum, writeall), unchanged as well
    kilo/            the release of kilo, byte for byte (BSD-2-Clause)
      LICENSE          the license of the release, unchanged
      kilo.c           the editor of 1 308 lines, and the gate of the terminal
      README.md        our note: the version, the commit and the binary
    upstream.txt     the sha256 of every file above against its commit
    CMakeLists.txt   one project, one target per program: a person with the
                     SDK writes this file, and it names no source of the tree
    command.c        the one program here that is ours: the gate of `popen`,
                     `pclose` and `system` on the machine
    ttyprobe.c       the other one: the terminal of a program asked in the
                     words of a transcript (ADR 0051)
    README.md        this file
```

## The upstream

* `https://git.suckless.org/sbase`
* commit `c546c3a5724c81cee9a11d816a38ccdf17472129` — "rm: Don't ask for
  confirmation to remove symlinks", the tip of the day of the copy
* license: MIT ([`sbase/LICENSE`](sbase/LICENSE), the file of the release);
  sbase is permissive at every byte and enters the tree under the rule of
  [`docs/licensing.md`](../../docs/licensing.md) and ADR 0025

The files are the ones of the commit, copied and never touched: the whole
point of the gate is that a program of somebody else needs **no port layer**
of this system, so a hunk here would be a lie. `upstream.txt` carries the
sha256 of every file and `tools/ports_smoke.py` recomputes it before the
machine boots — a formatter, a quick fix or a tab turned into spaces fails the
gate. `tools/integrity_check.py` treats `tests/ports/sbase/` as a vendored
tree for the same reason (sbase indents with tabs and its lines are its own).

## The build

```sh
sh tools/ports_build.sh --build build-ports     # make sdk + the programs
python3 tools/ports_smoke.py --build build-ports --x86 build-x86
```

`tools/ports_build.sh` builds the SDK of the release
(`build-ports/sdk/microos-sdk/<version>/`) and configures this directory with
the toolchain file of that SDK. Nothing of the CMake of the tree is involved:
the library is `lib/libmicroos_user.a`, the entry is `lib/crt0.o` and the link
of the image is `bin/link_program.sh`. A program that asked for a header or a
function the library does not have would fail here — which is the number the
lane is about, and `docs/ports-table.md` has the table of how many did not.

## The programs

`true`, `false`, `echo`, `basename`, `dirname`, `pwd`, `printenv`, `sleep`,
`cksum`, `tee`, `cmp`, `whoami` and `sha256sum` — the console set of the
collection, one target each
in `CMakeLists.txt`. They are written into the volume of the run under
`/bin/ports/`, so the image of the machine keeps its own names (`/bin/echo`
and `/bin/pwd` of the tree are the programs of ADR 0016 and not these), and
every one of them is typed at the shell by `tools/ports_smoke.py` with the
answer of the release as the check.

The twelfth program, `command.c`, is ours: it is the gate of the three calls
of a command that 1.8 added to the library (`popen`, `pclose`, `system`), and
it starts the tools of the same directory — the pipes and the statuses are the
answers of programs built by the SDK, not of a stub.

## The screen

`kilo/kilo.c` is the editor of Salvatore Sanfilippo (antirez) and the second
gate of the directory: a program that owns the screen and reads one key at a
time, built by the same SDK with **no line of its source edited** (ADR 0051).
It is the reason `components/libc/user` has `<termios.h>`, `ioctl` and an
`isatty` that asks, and the reason the console of x86-64 reads the cursor and
erase sequences of a terminal.

A transcript can type at a shell and not at an editor, so the gate has two
halves: `make ports-smoke` writes kilo into `/bin/ports` and runs `ttyprobe.c`
(ours) which asks the terminal in the words of a transcript — `isatty`, the
mode, the size of the screen, the raw mode and the mode that comes back — and
`make tty-smoke` drives the keyboard itself: it types a line into
`/bin/ports/kilo /tmp/k.txt`, sends Ctrl+S and Ctrl+Q and reads the file back
with `cat`. The two are the proof of the same thing, and neither can be
replaced by the other.

`upstream.txt` carries the sha256 of `kilo/kilo.c` and `kilo/LICENSE` beside
the sources of sbase, and `tools/integrity_check.py` treats `tests/ports/kilo/`
as a vendored tree: a formatter never touches a byte of it.
