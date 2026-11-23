# file 5.45 and libmagic — the type of a file

file is the command of a Unix-like system that says what a file is: an ELF
image, a gzip stream, a PNG picture, a tar archive, a text of a language. The
work is done by libmagic, which reads a database of rules (the `magic` files)
and matches the bytes of the file against them. This tree carries the release
5.45 and links the library into `/bin/file` of the x86-64 port.

* Upstream: <https://github.com/file/file>, tag `FILE5_45`
* Release: 5.45 (27 July 2023), the tarball `FILE5_45.tar.gz`
* License: [`LICENSE`](LICENSE), BSD-2-Clause (the copyright of the authors is
  in it)
* Used in: `/bin/file` of the x86-64 MicroFS image, with the database
  `/etc/magic.mgc` and its text `/etc/magic`
* Built by: `cmake/userland.cmake` with the compiler and the flags of the
  guest (`-ffreestanding`, no PIC, no SSE), against `components/libc/user`
  and not the libc of the host
* Recipe: [`docs/porting-programs.md`](../../docs/porting-programs.md)

## What is here

The C sources of the library and the Magdir files the subset is taken from:

```
src/          magic.c apprentice.c softmagic.c funcs.c fsmagic.c ascmagic.c
              encoding.c readelf.c der.c cdf.c readcdf.c cdf_time.c is_csv.c
              is_json.c is_simh.c is_tar.c print.c buffer.c and their headers
              the fallbacks of the release: asctime_r ctime_r gmtime_r
              localtime_r dprintf fmtcheck strlcat strlcpy pread
magic/Magdir/ elf compress archive zip images jpeg audio vorbis riff c-lang
              lua python sql msdos linux filesystems
magic/        microos.magic (ours, the MOX1 record) and subset.json (ours,
              the selection)
port/         config.h, magic.h, file_port.c, magic_compile.c
```

`LICENSE` is the upstream license, unchanged. `README` is the upstream note,
unchanged. The documentation of the release, its test suite, its fuzzers, its
build system and its own `file` command are not vendored: the program of this
tree is `ports/port_x86_64/programs/file.c`, and `README` names the release so
that a reader can fetch the rest.

`port/magic.h` is the public header the release generates from
`src/magic.h.in` (`X.YY` becomes `545`, as `Makefile.am` does); it is
generated here because the release's build is autoconf and this tree has
CMake.

## The database: a subset of Magdir

The release ships about ten megabytes of magic rules — every format of every
system of forty years. The image of a program of this system is a file of a
few hundred kilobytes and the RAM of the machine is what the boot leaves, so
the port carries a **compact database**:

* `tools/magic_subset.py` reads `magic/subset.json` and writes one text file;
  `microos_magic_compile` (the library and `port/magic_compile.c`) compiles it
  into `magic.mgc`. The build puts both in the image: `/etc/magic` (the text)
  and `/etc/magic.mgc` (the compiled form, 359 KiB).
* A `magic_load` of `/etc/magic` tries `/etc/magic.mgc` first and falls back
  to the text, which is the two levels of the port. A machine without the
  compiled file still has a working `file` (slower and with a bigger
  allocation), and a machine with neither gets `cannot open magic database`.
* The selection keeps a top-level rule and its continuations up to a `depth`
  of `>`; a rule whose description `desc` does not name is dropped with its
  subtree. The condition lines without a description stay, so the tree that
  reaches a description is whole. A `name` block is kept when a kept rule
  `use`s it, and the closure is followed; the `\^` form of a name is the same
  block.
* The families are the ones `subset.json` names: ELF, the compressed streams
  (gzip, bzip2, xz, LZMA, lz4, zstd, compress, 7-zip), the archives (tar,
  cpio, ar, zip, RAR), the pictures (PNG, JPEG, GIF, BMP, TIFF), the audio
  (WAVE/RIFF, FLAC, Ogg, MIDI, Sun/NeXT, Monkey's, Musepack), C and its
  relatives, Lua, Python, SQL (SQLite, MySQL, PostgreSQL), MS-DOS/PE, Linux
  kernels and swap, and the filesystems (ext2/3/4, NTFS, squashfs, BTRFS, XFS,
  F2FS, ReiserFS, romfs, ISO 9660). The text of a plain file is not in Magdir
  at all: the encoding and `ascmagic` of the library name it.
* The rules of a **float** or **double** type are dropped: a program of this
  system is built with `-mgeneral-regs-only` (no XMM state is saved) and the
  library is compiled with `MICRO_OS_NO_FLOAT`. A rule of the database that
  needs a floating point register would ask the library for a number it
  cannot hold.

`magic/microos.magic` is the port's own record and the first entry of the
database: an image of this system is `MOX1` at offset 0, and
`file /bin/file` (or any program of `/bin`) answers
`Micro OS MOX1 executable, version 2, x86-64`.

## The port layer

| File | What it is |
|---|---|
| `port/config.h` | the `config.h` of autoconf, written by hand for this machine: no fork, no mmap, no zlib/bzlib/lzma/zstd of the release, no floating point, no `sscanf`, no `*` precision |
| `port/file_port.c` | the two calls of a process the machine cannot make: `sread` (a read that waits out the `EAGAIN` of a pipe) and `file_pipe2file` (a refusal: there is no temporary file) |
| `port/magic_compile.c` | the compiler of the database, the `file -C` of the release as a tool of the build |
| `port/magic.h` | the generated public header |
| `.clang-format` | `DisableFormat: true`: the release keeps its own style |

There is no `mmap` on this machine. The library is built **without `QUICK`**,
so `apprentice_map` allocates the compiled database and reads it into the
block (in pieces of 64 KiB, the transfer edge of one call), and
`magic_buffer()` is handed the bytes the program already read: the mmap path
of the release is not compiled and the descriptor path (`magic_file`) is not
used by the program.

The stream of the program now has a block (`components/libc/user/src/file.c`):
without it a line reader over a few hundred kilobytes — the `grep` of sbase
over the database of this port — is one system call per byte.

## The hunks of the upstream source

The upstream files are **not** edited where the port layer can answer. Seven
hunks are unavoidable, and each is recorded here so that an update is a diff
to read:

1. **`softmagic.c`, `apprentice.c`, `print.c`, `readcdf.c`** — the magic types
   `float` and `double` are inside `#ifndef MICRO_OS_NO_FLOAT`. The guest has
   no floating point register and the formatter of the library of a program
   has no `%g`; a database of the port has no rule of those types.
2. **`apprentice.c`** — `mkdbname` builds the name of the `.mgc` beside the
   text with a copy under `MICRO_OS_NO_STAR_PRECISION` instead of
   `asprintf("%.*s%s")`: the formatter of a program prints a precision but not
   the `*` width that names it in an argument.
3. **`apprentice.c`** — the compiled database is read in pieces of 64 KiB
   (the edge of one transfer of the system calls) and not with one `read` of
   the whole file.
4. **`apprentice.c`** — the encoding test looks at
   `MICRO_OS_ENCODING_MAX` bytes (16 KiB) and not at 64 KiB: the release
   allocates one `unsigned long` per byte of the window, which is a megabyte
   of the arena the program does not have.
5. **`funcs.c`** — `file_parse_guid` is a refusal under `MICRO_OS_NO_SCANF`:
   the library of a program has no `sscanf`, and a `guid` magic is not in the
   subset.
6. **`compress.c` is not compiled at all** — it is behind `HAVE_FORK` and the
   machine has no process. `port/file_port.c` answers its two symbols that the
   rest of the library names.
7. **`file.c`, `getopt_long.c`, `apptype.c`, `seccomp.c` and the test halves**
   are not vendored: the command of the tree is our own, and a program
   has no seccomp.

## The program

`ports/port_x86_64/programs/file.c` is the command:

```
file [-b] [-i] [-L] [-z] [-f list] [--] FILE...
file -                      the bytes of descriptor 0
```

`-b` prints no name, `-i` the MIME type and the encoding, `-L` is accepted
(the VFS has no symbolic link to follow), `-f` reads the names of a list and
`-z` looks inside a compressed file: a gzip stream with the zlib of the image
and a zstd frame with the decompressor of `microos_user_zstd` (the library of
the port of zstd; xz and bzip2 are not in the image). A directory is
`directory` (or `inode/directory`), a name that cannot be opened is the
sentence `file: cannot open `x' (No such file or directory)` and the status of
the program is 1. The file is read to 128 KiB: the rules of the subset look
at the start and at a few offsets near the end, and the arena is the rest.

## The tests

* `tests/cases/test_file.c` is the library for the host: an ELF header, a gzip
  stream, the magic of a Zstandard frame and a whole frame of the same, a PNG
  image, a tar archive, a MOX1 image, the empty file and a text, through
  `magic_buffer` and `file_buffer`, with the compiled database and with the
  text (`magic_load` and the fallback).
* `make x86-smoke` runs `/bin/file` on a program of `/bin`, on
  `/etc/greeting.gz` and `/etc/setting.zst`, on `/etc/hello.lua`, on a
  directory and on a name that is not there, with `-i`, `-b`, `-z` and `-f`.
* `make x86-disk-smoke` asks the same after a reset of the machine, on the
  volume the first boot left.

## The license of the binary

BSD-2-Clause asks that the copyright notice and the list of conditions travel
with a binary distribution. The MicroFS image carries `/etc/licenses/file` (a
copy of `LICENSE`), `tools/bundle.sh` puts `LICENSE.file` into the x86-64
archive, and `docs/licensing.md` has the row. A package that ships `file`
copies the same file beside the program; the `microos-progs` package of a
release is not in this tree.
