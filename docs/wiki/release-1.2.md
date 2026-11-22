# The 1.2 release

`1.2.0` is the release of the machine a person can see and feed. The x86-64
port gets a real screen, the system learns to download a file, to speak HTTPS
and to install a signed program, the port boots from an ISO and from a tar
that is the whole machine, and the programs of other people - zlib, sbase,
PCRE2 and Lua - become files of `/bin`. The volume, the syscall ABI and
`MOX1` do not change. The notes are in
[the release](../releases/1.2.md); the decisions are ADR 0023 (the screen),
ADR 0024 (the stream and the installer) and ADR 0026 (the ISO).

`1.1.2` is the patch of the shell inside this release: `ls` and `tree` read a
directory without a limit, `.` and `..` are a path, and the console says what
it means. A machine of `1.1.2` takes the image as it is.

## A real screen on x86-64

The port programs the Bochs VBE interface of the VGA of QEMU to 1280x800, 16
bits per pixel in RGB565 - the format the renderer of the kernel already
draws in - and maps the window above the kernel. The console is a grid of
**160 columns by 50 rows** of the 8x16 font of the PC (Spleen 8x16), and it
has a colour per cell:

```text
microos> config set font.line_gap 2
font: line_gap 2, the cell is 8x16 at 18 px, 44 rows of 160 columns
microos> config set console.ink red
microos> config set console.paper 0x001F
```

`font.line_gap` 0, 2 or 4 pixels gives 16, 18 or 20 pixels a line, so the
picture holds 50, 44 or 40 rows. A command colours a word with the SGR
sequences of a terminal (`ESC[31m` is red, `ESC[1m` is bold, `ESC[7m` is
reverse), the log marks a warning amber and an error red, and `ls --color`
paints a directory blue and an executable green. The serial console stays a
plain transcript. A machine whose card has no interface keeps the text mode
of 80x25, and the boot says which screen it chose. The whole table is in
[screen.md](screen.md) and [settings.md](settings.md).

## Download, install, verify

`wget URL FILE` downloads a file and survives a dropped link: a second run of
the same command continues it from the bytes that arrived. `pkg` installs a
program from the repository of the project:

```text
microos> ntp 10.0.2.2:123
ntp: 2026-11-23 12:00:00 UTC
microos> pkg repo add https://mirror.example/
microos> pkg -Sy
microos> pkg -S hello
:: signature ok
:: installed /usr/bin/hello (1.0.0)
microos> exec hello
```

A repository may be `https://`. The client is mbedTLS 3.6.2 with TLS 1.2 and
1.3, the roots of `/etc/ssl/certs.pem` read through the VFS and the clock of
NTP for the dates of a certificate: a machine whose clock is 1970 refuses the
connection and says to run `ntp`. TLS is the wire; the **signature** of the
index and of every `.mox` is the trust - a mirror with a valid certificate
cannot install a program that is not signed. The details are in
[network.md](network.md) and [pkg.md](pkg.md).

The board has no TLS and no installer: it is the compatibility port of ADR
0018, and a program of the board is still a new `progs.bin` and a flash.

## Programs from other people

The ports of programs that are not ours are the commands of the machine, with
their licenses in `/etc/licenses`:

| Program | What it is |
|---|---|
| `/bin/zcat` | zlib 1.3.1: a `.gz` of the volume inflated to the console or a file |
| `/bin/cat`, `grep`, `head`, `ls`, `tail`, `wc`, `sort`, `find`, `xargs`, `sed` | sbase (MIT), the commands under their canonical names |
| `/bin/pcre2grep` | PCRE2 10.42 (BSD-3-Clause WITH PCRE2-exception): a regular expression of a library |
| `/bin/lua` | the interpreter of Lua 5.4.7, with the script `/etc/hello.lua` |
| `/bin/bc`, `/bin/dc` | bc 7.2.0 (BSD-2-Clause): an arbitrary-precision calculator and its RPN |

The ten tools of sbase replace the small `ls`, `cat`, `grep`, `wc` and `head`
of ADR 0016; what the shell keeps is the state and the special commands (`cd`,
`path`, `df`, `mount`, `fsck`, `stat`, `write`, `mkdir`, `rm`, `tree`, `kill`,
`config`). The shell looks a bare name in `PATH` first, so `ls /etc` is the
program of the disk; `ls --color` paints a directory blue and an executable
green.

`bc` and `dc` are the same 115,440-byte image entered by its name: `bc -e
2^100` is the whole 31-digit power, and `scale=10; 4*a(1)` under `bc -l` is
`3.1415926532`, the answer of GNU bc. A program may now be an image of up to
**8 MiB**, read in pieces of 64 KiB: the interpreter of Lua is the first one
past the old 64 KiB edge, and a file past the ceiling is `E2BIG` before a byte
of it is read. The recipe for the next port is
[`docs/porting-programs.md`](../porting-programs.md), and
[programs.md](programs.md) has the program of `/bin` and the library of a
program.

The programs are published in **microos-progs**, a public repository with a
signed index at <https://turbososiska304.github.io/microos-progs/>. Its key is
`microos-progs.pub` (`/etc/pkg/keys/microos-progs.pub`), and a machine adds it
with `pkg repo add https://turbososiska304.github.io/microos-progs/` and
installs from it with `pkg -S`; [pkg.md](pkg.md) has the flags.

Not in this release, and the first of `1.3.0`: SQLite (its SDK, and a package
above the 1 MiB `PKG_PACKAGE_MAX`), zstd, `file`/libmagic and awk (branches
still in progress).

## An ISO and a tar

`make x86-iso` writes a BIOS ISO (El Torito) with the loader, the signed
kernel and the root image; the root of the CD is the second module of the
boot, mounted read-only, so a write answers `input/output error` and a person
who needs files boots a disk. `make x86-iso-smoke` boots it with `-cdrom` and
nothing else.

`make x86-user` builds the tar of the release, unpacks it and starts `run.sh`
from the tar: the artifact a person downloads is the machine that runs, with
the disk of `/bin` and `/etc` and the network of QEMU. The commands are in
[build.md](build.md).

## What 1.2 does not do

Many cores are **1.3.0**: the application processors of x86-64 are still not
started (ADR 0012), and so are SQLite, zstd, `file`/libmagic and awk (the
first needs its SDK and a package above `PKG_PACKAGE_MAX`, the others wait
for their merge). There is no TLS on the board, no UEFI and no hybrid or
persistent ISO, no 80x50 text mode, no directory beyond the 4 KiB of names of
MicroFS, no `xargs -P`, and `time()` of a program is still the uptime and not
the wall clock. The list, with its reasons, is in
[the release notes](../releases/1.2.md#what-this-release-does-not-do).
