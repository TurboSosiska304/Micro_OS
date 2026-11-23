# Porting a copyleft program

A program under GPL, LGPL or AGPL is not a file of this tree (ADR 0025). It is
a **package** of `microos-progs`: an image of `MOX1`, the exact source it was
built from, the patch the port takes and the license texts. The kernel stays
MIT, the program keeps its license, and the person who installs it chooses.

This page is the recipe. It assumes the SDK of 1.3 (`make sdk`: a sysroot with
the headers, `libmicroos_user.a`, the crt and the linker script, and a CMake
toolchain file); without it the program is built inside a checkout of this
tree, which is the same recipe with a shorter path.

## 1. Take the source and keep it

```text
sources/NAME-VERSION/              the upstream tree as it was taken
sources/NAME-VERSION.tar.gz        the same bytes, named by source_sha256
sources/NAME-VERSION.patch         every change the port takes, in order
sources/NAME-VERSION/BUILD         the command that produced the image
```

The tarball is the bytes the package was built from, not a link that may move.
`make check` of the repository verifies `source_sha256` against it. A patch is
one commit of upstream, described in the patch header in a sentence: *why* the
change is needed on this machine, not what it changes.

## 2. Build it

A program is a flat image linked for the window of the port (ADR 0016), built
against the library of a program, not the kernel:

```bash
cmake -B build -S . -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=<sdk>/toolchain-microos.cmake
cmake --build build --target NAME
```

The two things upstream almost always needs and this machine has not:

* **no `fork` of a process with its own image**: `fork` is a task with a copy
  of the address space (ADR 0005) and `execve` replaces the image;
* **a `system()` and a `popen()` that do not exist**: there is no `/bin/sh` by
  default, and `popen` is `ENOSYS`. A program that shells out says so, or the
  port gives it a `posix_spawn` of its own (a task, a pipe, `execve`).

Everything else is the library: `open/read/write/lseek`, `stat`, `dirent`,
`getopt`, `regex`, `fnmatch`, `strtod`, the formatter with `%f`.

## 3. The license, in four places

| Where | What |
|---|---|
| the package | `license` in the index: the SPDX identifier, `GPL-3.0-or-later` |
| the repository | `licenses/NAME/` with every text the binary contains |
| the meta | `license`, `source`, `source_sha256` in `meta/NAME.mox.json` |
| the index | the same, written by `tools/mkindex.py` |

`pkg` refuses a package whose `license` starts with `GPL-`, `LGPL-`, `AGPL-`
or `CDDL-` and whose `source` or `source_sha256` is missing or not 64
hexadecimal digits (`PKG_ELICENSE`, ADR 0025). The installer cannot verify a
license - it verifies that the claim is complete, and the repository's CI and
its history carry the rest.

## 4. LGPL and the static link

There is no dynamic linker: a library is linked into the image. LGPL §6 then
asks that the user be able to relink the program against a modified library,
which the repository satisfies with the objects:

```text
sources/NAME-VERSION/objects/*.o       the objects the image was built from
sources/NAME-VERSION/BUILD             the link command, one line
```

The patch and the objects are the difference between "we shipped a GPL binary
and a URL" and a license that is met.

## 5. Publish it

```bash
make index          # index.json and index.html from the files
make check          # every digest, both signatures, the license rule
git push            # Pages publishes; pkg sync sees it
```

The CI of `microos-progs` does the same on a tag, with the key of the release
in a secret: `check.yml` for every push (no key needed), `publish.yml` for the
manual run and the tag.

## 6. Prove it on the machine

The smoke of a package is one line of the shell:

```text
microos> pkg repo add https://turbososiska304.github.io/microos-progs/
microos> pkg -Syu
microos> pkg install diff
microos> diff /etc/setting /etc/greeting
```

`make x86-net-smoke` runs that against a repository of the test; the package
of a release is installed once by hand from the live one, and the transcript
goes into the release notes.

## 7. The first five

| Program | License | What it proves |
|---|---|---|
| `diffutils` (`diff`) | GPL-3.0-or-later | a program that reads two files and exits with a status |
| `patch` | GPL-3.0-or-later | writes a file from a diff: the `rename` and the modes |
| `sed` | GPL-3.0-or-later | the regex of a library, in a program of its own |
| `gzip` | GPL-3.0-or-later | a stream through a compressor, and the file modes |
| `make` | GPL-3.0-or-later | `fork`, `execve`, a pipe and a wait: the shell's half |

Each is one package with its sources and its patches. The order is the order
of the difficulty, and `make` is the last because it is the first program that
wants a machine around it.
