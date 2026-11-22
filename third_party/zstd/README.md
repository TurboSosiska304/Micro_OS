# zstd 1.5.7 on Micro OS

zstd is the third library of somebody else in this tree, after zlib and Lua,
and the first whose command is the program of the release itself
(`programs/zstdcli.c`). It is the compression format of the system: `/bin/zstd`
packs and unpacks a file, and the frames it writes are the frames of every
other zstd of the world.

## The release

| | |
|---|---|
| version | 1.5.7 (26 Feb 2025) |
| source | <https://github.com/facebook/zstd>, the tarball `zstd-1.5.7.tar.gz` of the release (sha-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`) |
| license | BSD-3-Clause, Copyright (c) Meta Platforms, Inc. and affiliates (`LICENSE`) |
| upstream note | `README` (the release note of the tarball, unchanged) |
| upstream license choice | the files are dual licensed "BSD-3-Clause or GPL-2.0"; this tree takes the BSD-3-Clause and vendors no copy of the GPL text (`COPYING` of the tarball is not here) |

## What is here

The library and the command, as the release ships them:

```
lib/common/*.c  lib/compress/*.c  lib/decompress/*.c    the library
lib/zstd.h  lib/zstd_errors.h  lib/zdict.h              the headers
programs/zstdcli.c                                      the command
programs/fileio.c  fileio_asyncio.c  util.c  timefn.c   the files of the command
programs/*.h  (platform.h, util.h, fileio*.h, timefn.h) their headers
```

`LICENSE` is the upstream license, unchanged. What is not here is what this
port does not build, and `README` names the release for a reader who wants it:

* `lib/dictBuilder` (cover, fastCover, divsufsort, `zdict`) — the *builder* of
  a dictionary. `--train` answers "training mode not available" and the
  library still reads a dictionary somebody else trained (`-D`).
* `lib/deprecated` — the frames of a version before 1.0.
* `lib/decompress/huf_decompress_amd64.S` — the assembly loop of the Huffman
  decoder (see `ZSTD_DISABLE_ASM` below).
* `programs/benchzstd.c`, `benchfn.c`, `datagen.c`, `dibio.c`, `lorem.c` — the
  benchmark (`-b`) and the trainer of the CLI.
* the build systems, `tests/`, `contrib/`, `doc/` and `examples/` of the
  tarball.

## What is added (the port layer)

Four files and no change to the upstream source:

| File | What it is |
|---|---|
| `port/zstd_port.h` | the configuration of the library, force-included into every source of `lib/` |
| `port/zstd_cli_port.h` | the configuration of the CLI, force-included into every source of `programs/` |
| `port/zstd_cli_os.h` | the numbers and the prototypes the two halves of the port share |
| `port/zstd_cli_os.c` | the wrappers: the buffers of a stream, two parameters of a context, and the five POSIX calls of a process |
| `port/utime.h` | `struct utimbuf` and `utime`, which the CLI includes |

The upstream files are **not** edited. The configuration is a force-included
header (`-include`), so an update to a new 1.5.x is a copy of its files and a
run of the tests.

| Setting | Why |
|---|---|
| `ZSTD_NO_TRACE` | the trace hook of the library is a pointer the CLI never sets |
| `ZSTD_DISABLE_ASM` | the C path of the Huffman decoder; the assembly of 1.5.7 is not in the tree |
| `ZSTD_LEGACY_SUPPORT 0` | the frames before 1.0 are refused, not decoded |
| `ZSTD_MULTITHREAD` *not defined* | the library of a program has no `pthread`; a single-thread context is the one the CLI builds |
| `ZSTD_NOBENCH`, `ZSTD_NODICT`, `ZSTD_NOTRACE` | the CLI without the benchmark, the dictionary builder and the trace of a stream |
| `PLATFORM_POSIX_VERSION 0` | the C paths of the release for a machine that is Unix-like in shape and not in calls: the dictionary is read with `fread`, no `mmap`, no `nanosleep`, no `setpriority`, and `IS_CONSOLE` answers 0 |
| `ZSTD_SPARSE_DEFAULT 0` | MicroFS has no hole; a decompressed stream is written whole |
| `BACKTRACE_ENABLE 0` | the compiler of this machine is a glibc-targeted one and its `<limits.h>` defines `__GLIBC__`; this program has no `execinfo.h` |

### The memory policy

The arena of a program is its bss and the format caps the bss at a megabyte
(`K_EXEC_BSS_MAX`). The CLI of the release wants about three: a window of two
megabytes, tables of half a megabyte, four buffers of a stream of 128 KiB and
two more. The port answers with the machine's numbers, and they are the ones
`make x86-smoke` measures:

| Number | Value | Why |
|---|---|---|
| `ZSTD_PORT_STREAM_SIZE` | 32 KiB | the size of the four buffers of a stream (the read pool holds four of them and the write pool two): 192 KiB of the arena |
| `ZSTD_PORT_WINDOWLOG` | 16 | the window of a frame when the caller did not name one: 64 KiB. The format is unchanged, and every decoder of zstd reads such a frame |
| `ZSTD_PORT_TABLELOG`, `ZSTD_PORT_HASHLOG` | 14, 15 | the chain and hash tables of the compressor, clamped with the window |
| `ZSTD_PORT_SRC_SIZE_HINT` | 32 KiB | a stream of an unknown length (a pipe) is compressed for a source of 32 KiB; without the hint the library sizes the workspace for the window of the level, two megabytes at level 3 |
| `ZSTD_PORT_DECODE_WINDOWLOG` | 19 | the largest window this machine decodes: a frame that names more is refused with "requires too much memory", not with a failed allocation in the middle of a stream |

The consequence is a machine of a megabyte and not of a gigabyte, and it is
written down in `docs/debts.md`: a level above 3 needs its own room (level 19
is comfortable to about 16 KiB of source), a file of half a megabyte at level
3 costs about 700 KiB and fits, and a frame of the host with a window past
512 KiB is refused.

### The shape of the calls

The library of a program (`components/libc/user`) answers the negative number
of the ABI, and the release is written for a POSIX library: it tests a
descriptor against `-1` and hands it to `fdopen`. The build of the runtime of
`zstd` therefore renames `open` (`-Dopen=ulibc_open_call`) and
`port/zstd_cli_os.c` defines the POSIX one:

```c
int open(const char *path, int flags, ...)
{
    int fd = ulibc_open_call(path, flags);

    if (fd < 0) {
        errno = -fd;
        return -1;
    }
    return fd;
}
```

The other four wrappers answer the machine: `sysconf` is 1 (the second core is
not in the scheduler yet, ADR 0012), `chown`/`fchown` are the one owner of ADR
0005, `fchmod`/`utime` are the moment of the record the filesystem keeps (ADR
0013). `port/utime.h` carries the structure and the declaration the CLI
includes.

The CLI also asks the library of a program for `<sys/stat.h>`, `<dirent.h>` and
`<libgen.h>`: those three arrived with this port (from `feature/sbase`, which
wrote the POSIX families of the tree) and `rm` uses the POSIX `stat` of them
now. `docs/porting-programs.md` has the table.

## The command

`/bin/zstd` is `programs/zstdcli.c` of the release, built as the `main`:

* `-d`, `-c`, `-f`, `-q`, `-l`, `-1`..`-19`, `--rm`, `-o FILE`, `--stdout`,
  `-t`, `-r`, `-D FILE` (a dictionary somebody else trained), `--train` (the
  sentence of a build without the builder), a pipe in both directions, and a
  file of the VFS in place of either;
* `/bin/zstdcat` is the same image under a second name: the release reads the
  name it was entered with and takes the mode of a known one. `tools/x86_image.py`
  writes the image twice, and there is no second program in the tree.

The image is 441,888 bytes (401,376 of text, 40,480 of data, 983,200 of bss -
the arena is 960 KiB and the rest is the state of the library), well under
`K_EXEC_MAX_IMAGE` (8 MiB). It is **not** small: an `exec` of it reads 437 KiB
through the filesystem of the port, and that read is the quadratic cost of
`docs/debts.md` (a program of 164 KiB, Lua, costs about a second; zstd costs
about six, which is why the command list of `make x86-smoke` has one line of
each thing and not five).

## Tests

* host: `tests/cases/test_zstd.c` compiles lib/common, lib/compress and
  lib/decompress with the headers of the host and the port header, and covers
  the round trip of text and binary data at levels 1, 3 and 19 (0, 1, 64,
  4096 and 65536 bytes, four patterns), the streaming calls the CLI reads a
  file with, an empty input, the refusal of a stream that is not a frame, of a
  truncated one and of one with a changed byte under the checksum, and
  `ZSTD_getFrameContentSize` for a frame of a known and of an unknown source.
* guest: `make x86-smoke` unpacks `/etc/setting.zst` (a frame the `zstd` of
  the host wrote in `tools/x86_image.py`), packs the setting and unpacks its
  own frame, reads `-l` and `-t`, refuses `/etc/broken.zst` (a frame with its
  tail cut), runs `/bin/zstdcat`, and packs and unpacks through a pipe. One
  boot of its own reads a frame **the guest wrote** out of the machine (the
  bytes in hexadecimal, printed by `/etc/hex.lua`) and unpacks it with the
  `zstd` of the host, which is the other half of the test of the format.
* `make x86-disk-smoke` packs a file into the volume in the first boot and
  unpacks it in the second one, after the reset.

A dictionary trained by the `zstd` of the host and loaded with `-D` was checked
by hand (the guest read a frame of `/etc/hello.lua` of 31 bytes with it, and
`zstd -t -D` agreed), and `--train` answers "training mode not available"; the
image carries no dictionary, so no fixture of the command list covers `-D`
(`docs/debts.md` says why the builder is not here).

## The license

`LICENSE` is the BSD-3-Clause text of the release, unchanged. The image carries
a copy at `/etc/licenses/zstd` beside `/bin/zstd`, `tools/bundle.sh` puts
`LICENSE.zstd` into the x86-64 archive, and a package that installs the command
copies the same file beside the program. The text is not merged into the MIT
`LICENSE` of the tree, and the GPL-2.0 half of the dual license of the release
is not vendored: this tree takes the BSD-3-Clause.
