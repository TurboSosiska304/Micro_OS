# ADR 0024 - The stream and the installer: programs from a repository

* Status: accepted, and built in part - see "What was built" at the end
* Date: 2026-11-22
* Supersedes: nothing
* Related: ADR 0004 (the port), ADR 0011 (the versions), ADR 0018 (the
  programs of the board), ADR 0020 (the signed image), ADR 0021 (the settings
  of `/etc`), ADR 0023 (the screen of x86-64), `docs/releases/2.0.md`

## Context

The programs of the system are a project of their own: the tree builds
`hello` and `cat` for the tests, and the applications a person wants live in
a repository of programs. `1.1.0` already made a program a file of the
machine - `/bin` is a partition of the board and a volume of x86-64 - so a
new program no longer needs a new firmware. What is missing is the way a
program arrives: the network of the system is a console client, and a person
copies a file by hand.

The network of today is DNS, TCP, an HTTP server and an HTTP client that
reads an answer into a buffer (`components/net/src/http.c`: HTTP/1.0, no
chunked body, no redirect, no TLS, no file). It answers a line of a console;
it cannot carry a program.

The destination is the one of `docs/releases/2.0.md`: a machine a person
changes from the machine itself. A program installed at the console is the
first half of that promise; the settings of `1.1.0` were the other.

## Decision

### 1. The repository is its own tree

The applications live in `microos-progs` (a repository of their own, as
`AGENTS.md` says), not in the kernel tree. Its CI builds a program for every
machine it names, signs it and publishes it with an index:

```
index.json          the list: name, version, machine, abi, size, sha-256
hello-x86_64.mox    the program and its signature
hello-x86_64.sig
```

The index is signed with the same Ed25519 key as the images of ADR 0020, and
the kernel tree does not grow an application it does not test.

### 2. First a stream, then a file

The HTTP client learns to stream: `Content-Length` and a chunked body, a
redirect that the client follows (a small bounded number), a timeout, and a
sink that is a `struct vfs_file` or the inactive slot of an update, not a
buffer. `wget URL FILE` is the console face of it, and the update over the
network of ADR 0022 §7 is the same stream into `esp_ota_write()`.

A resume (`Range`) is part of the stream when the sink is a file: a download
of a megabyte over a link that drops is not started again from zero.

### 3. TLS is mbedTLS, one stack on both machines

The public internet is HTTPS; GitHub Pages answers nothing else. The stack is
**mbedTLS**:

* the board already carries it in ESP-IDF and needs no new dependency;
* x86-64 has memory and no size budget of a partition, and mbedTLS is the
  stack with TLS 1.3 and a maintained X.509 implementation;
* one stack means one set of certificate rules, one set of cipher suites,
  and one place to read when a handshake fails.

On x86-64 the library is vendored and configured freestanding: the platform
layer gives it the allocator of the kernel (`kmm`), entropy from `RDRAND`
(falling back to the timer), the clock from NTP, and the roots from
`/etc/ssl/certs.pem` read through the VFS - no `malloc`, no `stdio`, no
`gettimeofday` of a libc. A clock that NTP has not set is a warning at the
handshake: a certificate has dates, and a machine of 1970 cannot check them.

### 4. A package is trusted by its signature, not by its transport

TLS protects the wire; the signature protects the program. Every `.mox` and
the index carry an Ed25519 signature, and the installer verifies the digest
and the signature before a byte reaches `/usr`. A hostile mirror can refuse
to serve a package; it cannot put another program in its place. The record
of the installed version is the floor of a downgrade, in the spirit of the
anti-rollback of ADR 0020, and a package names the machine and the ABI it
needs (`MOX1` header and the version of the kernel).

### 5. `pkg` is the installer, and it is a program of x86-64

`pkg sync`, `pkg install NAME`, `pkg remove NAME`, `pkg list`, `pkg info
NAME` and `pkg upgrade` live in `components/pkg` (portable, host-tested with a
fake transport and a temporary volume). The database is under `/var/pkg`, the
programs under `/usr/bin`, and `/bin` stays the programs of the system.

An install is atomic: the bytes go to `NAME.mox.new`, the signature is
checked, the file is flushed, and a **rename** makes it visible. The VFS has
no rename today (ADR 0021 said so about a settings file), so the installer
adds `vfs_rename` as an operation of the filesystem, implemented by MicroFS
and FAT; a volume that cannot rename still installs with a warning that the
window is not atomic.

The board has no installer: its `progs` partition belongs to the firmware
(ADR 0018, compatibility port), and `pkg` is not built for it. A program of
the board is still a new `progs.bin` and a flash.

### 6. The interface is pacman's; the build is not

The convenience of `yay` and `paru` is two things: a package manager with
muscle memory, and an AUR build on the machine. The first is a contract of
the console and this ADR fixes it; the second is not possible here and is not
wanted (a program of `MOX1` is static, the machine has no compiler, and an
install script is code of a stranger run as root).

The commands and their pacman-style short forms:

| The console | Short | What it does |
|---|---|---|
| `pkg sync` | `-Sy` | refresh the index of every repository |
| `pkg upgrade` | `-Su` | a newer version of every installed package |
| `pkg sync --upgrade` | `-Syu` | both, in that order |
| `pkg install NAME...` | `-S` | fetch, verify and install |
| `pkg install ./x.mox` | `-U` | the same for a file on the machine |
| `pkg remove NAME...` | `-R` | remove; `-Rn` also the files it owns |
| `pkg search TERM` | `-Ss` | the name and the summary of a match |
| `pkg info NAME` | `-Si` / `-Qi` | the package in a repository / installed |
| `pkg list` | `-Q` | the installed packages; `-Qe` only the explicit ones |
| `pkg files NAME` | `-Ql` | the files a package installed |
| `pkg check` | `-Qk` | the digest of every installed file |
| `pkg clean` | `-Sc` | the cache, keeping what an install needs |
| `pkg repo add URL` | | a repository and its key, in `/etc/pkg.conf` |
| `pkg log` | | what was installed, removed and when |

The flags of an install: `--noconfirm`, `--needed` (a version already there
is a no-op), `--download-only`, `--dry-run` (the plan, no byte written),
`--allow-downgrade` (a warning, the default refuses an older version),
`--ignore NAME`, `--force` (reinstall), `--offline` (the cache only),
`--root DIR` (a volume for the tests), `--repo NAME`, `--cachedir DIR`,
`--color auto|always|never`, `--quiet`, `--verbose`, and `NAME=VERSION` for a
pin. `--help` prints the table, and `pkg --version` the version of the tool.
There is no `--nosignature`: a package without a valid signature does not
install, and no flag is a door around that.

The console face is a command of the shell (the first step, like `config`) and
becomes a program of `/usr/bin` when the calls it needs - `open`, `write`,
`rename`, `unlink` - are in the system-call table. A terminal with a pointer
(after ADR 0023) may grow a picker; the plain `pkg search` stays.

### 7. What is not in this decision

* a dependency solver: a program of `MOX1` is static, and its dependencies
  are the machine and the ABI, named in the index;
* shared libraries, a script engine in the installer, or a package that runs
  code at install time;
* a GUI package manager (it follows the screen of ADR 0023, not this ADR);
* a delta update: a package is replaced whole.

## Consequences

* The HTTP client grows a streaming API and keeps `http_get` for the console;
  the fake server of the tests covers both.
* mbedTLS adds a few hundred kilobytes to the kernel of x86-64 and a port
  layer that every bare-metal user of the library needs: allocator, entropy,
  clock, roots. The board uses the copy of ESP-IDF as it is.
* The VFS gains `rename`, the first metadata operation after the read/write
  set of 1.0; MicroFS and FAT implement it, and an install on a volume
  without it says that it is not atomic.
* The programs repository becomes a thing that is built and published, not a
  directory of examples; the kernel tree keeps only the two programs its
  tests need.
* The network of a machine with a real clock is a machine that can install:
  NTP is no longer a convenience of the console but a part of the trust.

## What was built

The first pass of K12 is in the tree. It is honest about its two halves: the
stream, the index, the signature and the installer are here and are tested;
TLS is not, and the note says why.

### The stream, and `wget`

`components/net/src/http.c` has a second client beside `http_get`, which
stays for the console:

```c
struct http_stream {
    const struct http_transport *transport;
    const char *url;
    struct http_sink sink;   /* write(), and reset() for a 200 after a Range */
    k_u64_t resume_from;     /* the Range */
    int max_redirects;
    k_u32_t timeout_ms;
    ...                      /* status, length, total, redirects, chunked */
};
int http_stream(struct http_stream *stream);
```

It reads a body of `Content-Length`, a chunked body (with the extensions of a
chunk size and the trailers at the end), follows at most `HTTP_REDIRECT_MAX`
(5) redirects - an absolute URL or a path on the same host - and writes the
bytes into the sink as they arrive, so a program of 4 KB costs a 256-byte
buffer and not a buffer of its size. A `Range: bytes=N-` is sent when the
caller names a `resume_from`; a `206` continues the sink, a `200` calls
`sink.reset()` because the server ignored the range, and a sink without a
reset makes that answer `HTTP_ERROR` instead of writing half of an old file
in front of a whole new one. A body that ends before its length is
`HTTP_SHORT`; a body that is not a success (a 404 of a mirror) is read and
thrown away, and the sink stays empty.

The console face is `wget URL FILE`: the destination is the sink, the file
that is already there is the `Range`, and a download that was cut keeps the
bytes it has - the second run of the same command continues it. The same
`net_http_download_to()` is what `pkg` uses, with the transport of the port or
a fake one in the tests.

A bug of the TCP client of x86-64 came out of this work: the receive window
was only advertised with a segment the stack sent, so a body larger than the
4 KB buffer of a session stopped in the middle. The reader now sends the
acknowledgement with the new window as it drains, and only the bytes it kept
are acknowledged.

### The index and the signature

The index is JSON, and `pkg_index_parse` reads the object of §1 while skipping
a field it does not know (so a repository grows without breaking an older
machine):

```json
{
  "repository": "microos-progs",
  "machine": "x86_64",
  "abi": 2,
  "packages": [
    { "name": "hello", "version": "1.0.0", "machine": "x86_64", "abi": 2,
      "size": 4288, "sha256": "…64 hex…",
      "file": "hello-1.0.0-x86_64.mox", "summary": "says hello" }
  ]
}
```

The signature of the index and of every `.mox` is Ed25519 over the bytes of
the file, detached, in a `.sig` file of 64 raw bytes; the primitives are the
ones of the signed image of ADR 0020 (`ports/port_x86_64/loader/sha256.c` and
`ed25519.c`), and `tools/microos_sign.c` grew two commands for the
repository side: `sign-file --key K --in FILE --sig FILE` and
`verify-file --pub P --sig FILE FILE`.

The key of a repository is `/etc/pkg/keys/<repo>.pub`, thirty-two bytes in
hexadecimal. When the file is not there, the key of the image is the key of
the repository: the release signs the kernel and the index with the same key,
and a machine that boots an image of the project trusts the programs of the
project without a second file to copy. The x86-64 build compiles the generated
`sign_public_key.h` into `pkg`; a host build has no image and therefore no
built-in key (the tests write theirs).

A package is checked in this order: the size against the index, the SHA-256
against the index, the signature against the key, and the header of MOX1
(`k_exec_parse`) against the machine. A package of another machine, of another
ABI, with a digest or a signature that does not match, does not reach `/usr`.

### The installer, and `vfs_rename`

`components/pkg` is the installer, a command of the shell (`pkg`) that does
not exist on the board: the ESP-IDF component is an empty interface and the
sources are not compiled there (ADR 0018).

* The database is one text file per package under `/var/pkg/local`, the cache
  is `/var/cache/pkg`, the index of a repository is `/var/pkg/index`, the
  programs go to `/usr/bin`, the repositories are lines of `/etc/pkg.conf`
  (`<name> <url>`) and the log is `/var/pkg/log`.
* The commands and the flags are the table of §6: `-S`, `-U`, `-R`, `-Q`,
  `-Qe`, `-Qi`, `-Ql`, `-Qk`, `-Sy`, `-Su`, `-Syu`, `-Ss`, `-Si`, `-Sc`,
  `repo add`, `log`; `--noconfirm`, `--needed`, `--download-only`,
  `--dry-run`, `--allow-downgrade`, `--force`, `--offline`, `--root`,
  `--repo`, `--cachedir`, `--color`, `--quiet`, `--verbose`, `--ignore`,
  `NAME=VERSION`, `--help`, `--version`. There is no `--nosignature` and
  there is no path that installs an unverified byte.
* An install writes `NAME.new` beside the program, verifies the bytes, makes
  the file executable (0755, because the shell runs only what has an execute
  bit), and calls `vfs_rename`. The VFS gained the operation (and the system
  call `KSYS_RENAME 34`, the next free number): it refuses a rename between
  two mounts with `KERR_XDEV`, and MicroFS and FAT implement it. A rename
  replaces an existing *file* at the destination, as POSIX does, and refuses
  a directory (`KERR_ISDIR`); a directory is renamed only inside its parent
  (`KERR_NOTSUP`), because moving the tree below it is a walk no filesystem
  of this system makes yet. A volume without the operation installs `in
  place` with a warning that the window is not atomic.
* The local file of `-U` is named as a repository names it,
  `NAME-VERSION-MACHINE.mox`, with its `.sig` beside it; the name is what
  lets the installer know what it has without an index.

### Tests

* `tests/cases/test_net.c`: the fake transport serves a body of a length, a
  chunked body, a redirect (relative and absolute), a chain that does not
  end, a body that ends early, a 404 (which must not reach the sink) and a
  Range that is a 206 or a 200.
* `tests/cases/test_vfs.c` and `test_fs_fat.c`: rename moves a name, replaces
  a file, refuses a directory that moves and a rename between two mounts.
* `tests/cases/test_pkg.c`: the index, a signed local file installed through
  the rename, a broken signature, a package of another machine (by name and
  by index), a downgrade without and with `--allow-downgrade`, a dry run that
  writes nothing, `--download-only`, the cache with `--offline`, an index
  without its signature, `-Qk` and `-R` - all on a temporary MicroFS volume
  under `--root`.
* `make x86-net-smoke` serves a repository signed with the key of the build:
  `wget` of a file, of a redirect, of a chunked body, of a cut body and of
  its resume, `pkg repo add`, `pkg -Sy`, `pkg -S hello`, `pkg -Q` and `exec
  hello` of the program that arrived.

### TLS: the honest blocker

The handshake is not here. mbedTLS is not vendored, and nothing in this tree
pretends to be TLS: `http_parse_url` refuses `https://` and says so. What the
stream gives a package is already enough for the trust of §4 - a signature
over the bytes, not a confident wire - and a mirror that lies about a body is
caught by the digest before a byte of it reaches `/usr`; what TLS adds is
privacy and the authentication of the server, and that is the next pass.

The work that remains is the one the ADR names: vendor mbedTLS, wire the
platform layer (`kmm` for the allocator, `RDRAND` with the timer as the
fallback, the clock of NTP, the roots of `/etc/ssl/certs.pem` read through the
VFS), the `https` branch of `http_parse_url`, and a test that a handshake
fails on a machine whose clock is 1970. It is recorded in
[debts.md](../debts.md), not passed over in silence.
