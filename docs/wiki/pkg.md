# `pkg`: programs from a repository

`pkg` is the installer of x86-64. It brings a program from a repository of
its own (`microos-progs`), checks its signature and puts it in `/usr/bin`.
It is the second half of the promise of `2.0` - a machine a person changes
from the machine itself; the settings of `/etc` were the first.

> **Status: built, over HTTP and HTTPS.** `pkg` is a command of the x86-64
> image with the network (`-DMICRO_OS_X86_NET=ON`, `make x86-net-build`). The
> board has none (ADR 0018). A repository may be an `https://` URL; the
> client of TLS and the trust of the store are described in
> [network.md](network.md). The package is still trusted by its signature and
> not by the wire: the two are independent, and a mirror with a valid
> certificate cannot install a program that is not signed (ADR 0024 §4).

## The shape of a package

A repository publishes an index and the files it names:

```text
index.json          the list: name, version, machine, abi, size, sha-256
index.json.sig      the Ed25519 signature of the index (64 raw bytes)
hello-1.0.0-x86_64.mox    the program
hello-1.0.0-x86_64.mox.sig  its signature (64 raw bytes)
```

The index is one object of the format of ADR 0024 §1:

```json
{
  "repository": "microos-progs",
  "machine": "x86_64",
  "abi": 2,
  "packages": [
    {
      "name": "hello",
      "version": "1.0.0",
      "machine": "x86_64",
      "abi": 2,
      "size": 4288,
      "sha256": "4f1c…",
      "file": "hello-1.0.0-x86_64.mox",
      "summary": "says hello"
    }
  ]
}
```

The ABI is the version of the header of MOX1 that the kernel reads
(`K_EXEC_VERSION`, 2 today). A field the parser does not know is skipped, so a
repository grows without breaking an older machine; a field that is needed and
missing, a digest that is not 64 hexadecimal digits, or a size above
`PKG_PACKAGE_MAX` is `PKG_EPARSE`.

`PKG_PACKAGE_MAX` is eight megabytes, the ceiling of an image of a program:
where `kernel/exec.h` names it (`K_EXEC_MAX_IMAGE`), the installer takes the
number from the header itself, so the two cannot drift apart. The number is the
memory of the machine and not the size of one transfer. A package is read into
**one block** of the allocator of that size before a byte of it is checked,
which is why the ceiling is also the answer to a hostile index: a document that
names a larger package asks for nothing.

The index and every program are signed with the same Ed25519 key as the images
of the system ([ADR 0020](../adr/0020-signed-image.md)). The tool of the
repository side is `microos_sign`:

```bash
build-x86-net/microos_sign sign-file --key signing.key --in hello.mox --sig hello.mox.sig
build-x86-net/microos_sign sign-file --key signing.key --in index.json --sig index.json.sig
build-x86-net/microos_sign verify-file --pub signing.pub --sig hello.mox.sig hello.mox
```

## The key of a repository

The key is `/etc/pkg/keys/<repo>.pub`, thirty-two bytes in hexadecimal (the
same text `microos_sign pubkey` writes). When the file is not there, the key
of the image is the key of the repository: the release signs the kernel and
the index with the same key, and a machine that boots an image of the project
trusts the programs of the project without a second file to copy. A host build
has no image and therefore no built-in key; its tests write theirs.

```text
microos> pkg repo add http://10.0.2.2:8000/
:: repository microos-progs is http://10.0.2.2:8000/
```

`pkg repo add` writes a line `<name> <url>` into `/etc/pkg.conf` (the name is
`microos-progs`, or `--repo NAME`).

## The commands

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
| `pkg clean` | `-Sc` | the cache |
| `pkg repo add URL` | | a repository and its key, in `/etc/pkg.conf` |
| `pkg log` | | what was installed, removed and when |

```text
microos> pkg -Syu
:: refreshing microos-progs
:: download http://10.0.2.2:8000/index.json.sig
:: download http://10.0.2.2:8000/index.json
:: 1 package(s) in the index
:: upgrade hello 1.0.0 -> 1.1.0
:: download http://10.0.2.2:8000/hello-1.1.0-x86_64.mox
:: download http://10.0.2.2:8000/hello-1.1.0-x86_64.mox.sig
:: signature ok
:: installed /usr/bin/hello (1.1.0)
microos> pkg -Q
hello 1.1.0
microos> exec hello
PROGRAM: hello from /bin
```

## The flags of an install

`--noconfirm`, `--needed` (a version already there is a no-op),
`--download-only`, `--dry-run` (the plan, not a byte written),
`--allow-downgrade` (a warning; the default refuses an older version),
`--ignore NAME`, `--force` (reinstall), `--offline` (the cache only),
`--root DIR` (a volume, for the tests), `--repo NAME`, `--cachedir DIR`,
`--color auto|always|never`, `--quiet`, `--verbose`, and `NAME=VERSION` for a
pin. `--help` prints the table and `pkg --version` the version of the tool.

There is **no `--nosignature`**. A package without a valid signature does not
install, and no flag is a door around that.

The installer never asks a question, so `--noconfirm` is accepted for the
scripts that pass it and changes nothing. `--color auto` is the plain console
(the escape sequences of `--color always` are for a terminal that reads them),
and `--quiet` keeps the errors and the answer of a query and drops the `::`
lines of the steps.

`--offline` uses what the cache and the stored index have: a machine with no
link installs a package that was fetched before, and `-Sy --offline` re-reads
the stored index (and its signature) without a socket.

## Where it lives

| what | where |
|---|---|
| the database | `/var/pkg/local/<name>`, one text file per package |
| the index of a repository | `/var/pkg/index/<repo>.json` and `.sig` |
| the cache | `/var/cache/pkg/<file>.mox` and `.sig` |
| the programs | `/usr/bin/<name>` (mode 0755) |
| the repositories | `/etc/pkg.conf`, one `<name> <url>` per line |
| the keys | `/etc/pkg/keys/<repo>.pub` |
| the log | `/var/pkg/log`, one line per install and remove |

The database is one file per package and not one table: a table is a file that
every install rewrites whole, and a power cut in the middle of it would lose
the names of the programs that are still in `/usr/bin`.

## An install is atomic

The bytes go to `/usr/bin/<name>.new`, the size, the digest, the signature and
the header of MOX1 are checked, the file is made executable, and a
**rename** makes it visible (`vfs_rename`, an operation of the filesystem that
MicroFS and FAT implement; the system call is `rename`, number 34). The package
travels in one block of `kmm` of its size (up to `PKG_PACKAGE_MAX`, 8 MiB) and
the block is freed once the file is in `/usr/bin`; a file past the ceiling is
refused before the block is asked for. A rename
replaces a file that is already there, as POSIX does, and a volume without the
operation still installs: the program is written in place and the command says
that the window is not atomic.

A local file (`-U`) is named as a repository names it,
`NAME-VERSION-MACHINE.mox`, with its `.sig` beside it; the installer reads the
name, the version and the machine from the file name, and the digest from the
bytes. The signature is checked against the key of the default repository
(`microos-progs`), which is the key of the image unless
`/etc/pkg/keys/microos-progs.pub` says otherwise. A file that is already
installed at the same version is left alone (`--force` reinstalls it).

## What it does not do, and why

* **It does not build.** The convenience of `yay` and `paru` is two things: a
  package manager with muscle memory, and an AUR build on the machine. The
  first is the contract above; the second is not possible here. A program is
  static `MOX1`, the machine has no compiler, and an install script is the
  code of a stranger run as root.
* **No dependency solver.** A program is static; its dependencies are the
  machine and the ABI, and the index names them.
* **No scripts, no shared libraries, no delta update.** A package is a file
  replaced whole.
* **No revocation of a certificate.** The store of `https` is one file,
  `/etc/ssl/certs.pem`, and a certificate that is withdrawn stays trusted
  until the file is replaced ([debts.md](../debts.md)).
* **No installer on the board.** The `progs` partition of the ESP32 belongs to
  the firmware (ADR 0018); a program of the board is a new `progs.bin` and a
  flash.

A third-party repository is the answer to "I want more than the official
index": `pkg repo add` takes a URL and `--repo NAME` names it, and the
signature rule is the same. That is the freedom of the AUR without the build.
