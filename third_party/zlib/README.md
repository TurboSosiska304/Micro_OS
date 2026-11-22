# zlib 1.3.1 — the first program library of somebody else

zlib is a general-purpose compression library by Jean-loup Gailly and Mark
Adler, released under the zlib license (a permissive license, compatible with
the MIT tree). This tree carries version 1.3.1 and links it into `/bin/zcat`
of the x86-64 port, the first program built on a library that is not ours.

* Upstream: <https://zlib.net/> and <https://github.com/madler/zlib>
* Release: 1.3.1 (22 January 2024), the tarball
  `zlib-1.3.1.tar.gz` of the GitHub release
* License: [`LICENSE`](LICENSE), zlib
* Used in: `ports/port_x86_64/programs/zcat.c`, the static archive
  `microos_user_zlib` of the port (`ports/port_x86_64/CMakeLists.txt`)
* Built by: `cmake/userland.cmake` with the compiler and the flags of the
  guest (`-nostdlib`, `-ffreestanding`, no PIC, no SSE), against
  `components/libc/user` and not the libc of the host

## What is here

The C sources of the release that make the library:

```
adler32.c  compress.c  crc32.c     deflate.c  gzclose.c  gzlib.c
gzread.c   gzwrite.c   infback.c   inffast.c  inflate.c  inftrees.c
trees.c    uncompr.c   zutil.c
crc32.h    deflate.h   gzguts.h    inffast.h  inffixed.h inflate.h
inftrees.h trees.h     zconf.h     zlib.h     zutil.h
```

`LICENSE` is the upstream license, unchanged. `README` is the upstream
release note, unchanged. The documentation of the release (`ChangeLog`,
`INDEX`, `FAQ`, `doc/`, `examples/`, `test/`, the build systems of the
release) is not vendored: it is not compiled and the tree does not need a
second copy of it, and `README` names the release.

## What is added (the port layer)

Two files and no change to the upstream source:

| File | What it is |
|---|---|
| `port/zconf.h` | the configuration of the build: the platform has `<unistd.h>`, `<stdarg.h>` and the two allocation hooks of `port/zlib_os.c`; it is force-included (`-include port/zconf.h`) before every source, so the upstream `zconf.h` stays byte for byte |
| `port/zlib_os.c` | the two allocation hooks (`zcalloc`, `zcfree`) over the arena of `components/libc/user`, and the four wrappers that give zlib the POSIX shape of a call (see below) |
| `.clang-format` | `DisableFormat: true`: the style of the release is the style of the release, and `make integrity` must not rewrite a vendored file |

The upstream files are **not** edited. `port/zconf.h` is force-included and
configures the build without a hunk in the release's `zconf.h`, so an update
to a new 1.3.x is a copy of the files of the release and a run of the tests.

## The shape of the port

The library of a program is not POSIX in two details that a portable source
notices:

* **The error is the negative number itself.** `open`, `read`, `write` and
  `lseek` of `components/libc/user` answer `-ENOENT`, `-EAGAIN` and so on;
  the utilities of the tree print `-fd`. zlib, like a program of a Unix-like
  system, expects `-1` and `errno`, and tests `open` and `lseek` against
  `-1`. `port/zlib_os.c` translates the four calls, and `port/zconf.h`
  renames them for the sources of zlib (`#define open zlib_os_open` and so
  on). The source of the port itself is compiled with `ZLIB_OS_NO_RENAME`,
  so its wrappers reach the real calls.
* **A pipe answers `EAGAIN`.** A read of a pipe whose producer is still there
  is not the end of the file (`ADR 0014`), and zlib would read a negative
  result as one. The wrappers wait the condition out, so `cat x.gz | zcat`
  works.

The allocator is the arena of the program, not the kernel: `KSYS_MALLOC`
answers memory of ring 0, which the page tables of a program do not map with
the user bit. `ULIBC_HEAP_SIZE` is 512 KiB for `zcat` (the state of an
inflate and the two buffers of `gz*`); the bss of an image is capped at 1 MiB
by the MOX1 format.

## The build

The port builds `libmicroos_user_zlib.a` with the flags of the guest and the
same compiler that builds the program, then links the archive into
`/bin/zcat`:

```cmake
microos_userland_library(microos_user_zlib
    SOURCES ${ZLIB_SOURCES}
    INCLUDES "${ZLIB_DIR}" "${ZLIB_DIR}/port"
    DEFINES MY_ZCALLOC HAVE_UNISTD_H HAVE_STDARG_H
    COMPILE_OPTIONS "-include" "${ZLIB_DIR}/port/zconf.h" -Wno-sign-compare -Os
)
add_libc_program(zcat LIBRARIES microos_user_zlib ...)
```

The archive is static and the program pulls the objects it names; no byte of
the host libc enters the image. `-Os` is not a taste: the kernel reads a
program image whole into a block of `KSYS_MAX_IO` (64 KiB), and the text and
data of `zcat` and its library have to fit that. The image of 1.3.1 is about
58 KiB; the same build at `-O2` is 80 KiB and the kernel answers
`not a MOX1 image of this system`.

## The tests

* `tests/cases/test_zlib.c` builds the same sources on the host with its
  headers and covers the checksums, the three stream wrappers and a `gz`
  round trip on a temporary file.
* `make x86-smoke` runs `/bin/zcat` on `/etc/greeting.gz`, a fixture the
  image tool writes with a fixed moment and a fixed text, and checks the
  count and the bytes of the inflated file.
* `make x86-disk-smoke` writes the inflated file in one boot and reads it in
  the next one.

## The license of the binary

The zlib license asks that the origin of the code not be misrepresented and
that a notice travel with a binary distribution. The MicroFS image carries
`/etc/licenses/zlib` (a copy of `LICENSE`), `tools/bundle.sh` puts
`LICENSE.zlib` into the x86-64 archive, and `docs/licensing.md` has the row.
A package that ships `zcat` copies the same file beside the program; the
`microos-progs` package of a release is not in this tree.
