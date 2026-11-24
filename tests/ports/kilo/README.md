# kilo — a full-screen editor built with nothing of this tree

The proof that this system has a terminal and not only a console: an editor of
somebody else, compiled by the SDK with **no line of its source edited**, and
run on the machine of `make ports-smoke`.

## What is here

```
kilo.c          the release, byte for byte
LICENSE         the license of the upstream, byte for byte (BSD-2-Clause)
README.md       this note
```

Nothing else: there is no port layer, because there is nothing to answer. The
editor asks for `isatty`, `tcgetattr`, `tcsetattr`, `cfmakeraw`, `ioctl` with
`TIOCGWINSZ`, `read`, `write`, `open`, `ftruncate`, `exit` and `atexit`, and
`components/libc/user` answers all of them (ADR 0051).

## The release

* upstream: <https://github.com/antirez/kilo>
* commit: `323d93b29bd89a2cb446de90c4ed4fea1764176e`
  ("Fix function declaration missing void.", 4 January 2025)
* license: BSD-2-Clause, the file `LICENSE` of the release

`tests/ports/upstream.txt` carries the sha256 of `kilo/kilo.c` and
`kilo/LICENSE`; `tools/ports_smoke.py` recomputes them before the machine
boots, so a hunk edited into the source to make it build or to make a test
pass is a failed gate and not a commit.

## The build

`tests/ports/CMakeLists.txt` names `kilo/kilo.c` and nothing else:

```cmake
add_executable(kilo kilo/kilo.c)
```

The CMake project is the one of a person with the SDK
(`share/microos-toolchain.cmake`, `-DMICROOS_WARNINGS=OFF`): the archive of the
library, `crt0.o`, the linker script of the window and `mkprog.sh`, and not a
file of the build of the tree. The image is `/bin/ports/kilo` of the volume of
`make ports-smoke` and `make tty-smoke`.

The warning of the release (`-Wunterminated-string-initialization` on the
`"//"` of its C syntax table) is the release's own and is not a failure of the
gate: a source of somebody else answers the warnings of its release.

## What the machine does with it

`/bin/ports/kilo /tmp/k.txt` opens a file that is not there, draws its screen
through the escape sequences the console of x86-64 reads (ADR 0051 §7), reads
the keys one at a time through the raw mode of the terminal, and writes the
file with Ctrl+S. Ctrl+Q leaves. `make tty-smoke` types a line, saves, quits
and reads the file back with `cat`, so the transcript of the machine is the
proof of the editor and of the terminal under it.

The arrows, Home and End are the keys the console of x86-64 does not give to a
character stream yet (`docs/debts.md`): the editor moves with the letters, the
Enter, the Backspace and its own Ctrl combinations.

## The license

BSD-2-Clause: permissive, so the file travels in this tree and in the volume
(ADR 0025). The text is beside the source, `/etc/licenses/kilo` in the image of
x86-64, `LICENSE.kilo` in a bundle, and the row of `docs/licensing.md`.
