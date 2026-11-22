# `pkg`: programs from a repository

`pkg` is the installer of x86-64. It brings a program from a repository of
its own (`microos-progs`), checks its signature and puts it in `/usr/bin`.
It is the second half of the promise of `2.0` - a machine a person changes
from the machine itself; the settings of `/etc` were the first.

> **Status: planned for 1.2.** `1.1.0` has no `pkg`; a program is a file a
> person writes or copies. The contract of the command is
> [ADR 0024](../adr/0024-stream-and-installer.md) §6.

## The shape of a package

A repository publishes an index and the files it names:

```text
index.json          name, version, machine, abi, size, sha-256
hello-x86_64.mox    the program
hello-x86_64.sig    its Ed25519 signature
```

The index is signed with the same key as the images of the system
([ADR 0020](../adr/0020-signed-image.md)). A package names the machine (the
`MOX1` header) and the ABI of the kernel it needs; a package for another
machine, or for a newer ABI, does not install.

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
| `pkg clean` | `-Sc` | the cache, keeping what an install needs |
| `pkg repo add URL` | | a repository and its key, in `/etc/pkg.conf` |
| `pkg log` | | what was installed, removed and when |

```text
microos> pkg -Syu
:: refreshing the index of microos-progs
:: 3 package(s) in the index
:: upgrade  hello 1.0.0 -> 1.1.0
:: download hello-1.1.0-x86_64.mox: 8 192 bytes, done
:: signature ok, installing /usr/bin/hello
microos> pkg -Ss editor
microos-progs/vi 1.2.0    a small screen editor
microos> pkg -S vi
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

An install is atomic: the bytes go to `NAME.mox.new`, the digest and the
signature are checked, the file is flushed, and a rename makes it visible
(`vfs_rename`, a new operation of the filesystem). A volume that cannot rename
still installs, with a warning that the window is not atomic.

## Where it lives

The database is `/var/pkg`, the programs are `/usr/bin`, the cache is
`/var/cache/pkg`, and the repositories and their keys are `/etc/pkg.conf` and
`/etc/pkg/keys`. The console face is a command of the shell at first (like
`config`) and a program of `/usr/bin` when the calls it needs - `open`,
`write`, `rename`, `unlink` - are in the table of the system calls.

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
* **No installer on the board.** The `progs` partition of the ESP32 belongs to
  the firmware (ADR 0018); a program of the board is a new `progs.bin` and a
  flash.

A third-party repository is the answer to "I want more than the official
index": `pkg repo add` takes a URL and its key, and the signature rule is the
same. That is the freedom of the AUR without the build.
