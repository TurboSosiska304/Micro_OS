# Licenses

Micro OS is released under the MIT license ([`LICENSE`](../LICENSE)). The
tree also carries components of other people, and each of them keeps its own
license. This page is the list and the rule that keeps the boundary clear.

This is an engineering policy, not legal advice. A binary distribution of a
third-party part should be reviewed by a lawyer before it ships
commercially; the note here is what the tree does.

## The components of other people

| Component | Version | License | Where | In the binaries |
|---|---|---|---|---|
| Spleen 8x16 | 2.3.0 | BSD-2-Clause | `third_party/spleen`, `ports/port_x86_64/src/font_spleen.c` | the x86-64 kernel image; `LICENSE.spleen` in its tar |
| zlib | 1.3.1 | zlib | `third_party/zlib`, `ports/port_x86_64/programs/zcat.c` | `/bin/zcat` of the x86-64 MicroFS image; `/etc/licenses/zlib` in the image and `LICENSE.zlib` in the x86-64 tar |
| sbase | c546c3a (tip) | MIT | `third_party/sbase`, ten tools of `/bin/sb-*` | `/bin/sb-cat`, `/bin/sb-grep`, `/bin/sb-head`, `/bin/sb-ls`, `/bin/sb-tail`, `/bin/sb-wc`, `/bin/sb-sort`, `/bin/sb-find`, `/bin/sb-xargs` and `/bin/sb-sed` of the x86-64 MicroFS image; `/etc/licenses/sbase` in the image and `LICENSE.sbase` in the x86-64 tar |
| file (libmagic) | 5.45 | BSD-2-Clause | `third_party/file`, `/bin/file` and the database `/etc/magic.mgc` of `/etc` | `/bin/file` of the x86-64 MicroFS image; `/etc/licenses/file` in the image and `LICENSE.file` in the x86-64 tar |

Spleen is a font by Frederic Cambus; the license and the note are in
[`third_party/spleen`](../third_party/spleen/README.md). The 8x16 table is
generated from the upstream BDF by `tools/spleen_font.py`, and the BSD
2-Clause license asks that its notice travel with a binary: the x86-64
bundle carries a copy.

zlib is a compression library by Jean-loup Gailly and Mark Adler; the
version, the files and the port layer are in
[`third_party/zlib`](../third_party/zlib/README.md). The zlib license asks
that the origin not be misrepresented and that a notice travel with a
binary, so the MicroFS image carries `/etc/licenses/zlib` beside `/bin/zcat`,
and `tools/bundle.sh` puts `LICENSE.zlib` into the x86-64 archive. A package
that installs `zcat` (`microos-progs` of a release) copies the same file
beside the program. The file stays zlib: it is not merged into the MIT
`LICENSE` of the tree.

sbase is a collection of Unix tools by suckless; the subset, the commit and
the two hunks of the port are in
[`third_party/sbase`](../third_party/sbase/README.md). Its license is the MIT
text of the collection, which names its authors. The MicroFS image carries
`/etc/licenses/sbase` beside the tools of `/bin/sb-*`, `tools/bundle.sh`
puts `LICENSE.sbase` into the x86-64 archive, and a package that installs a
tool (`microos-progs` of a release) copies the same file beside it. The
notice is not merged into the MIT `LICENSE` of the tree: the two are the same
license but not the same text.

file is the command of a Unix-like system that names the type of a file, and
libmagic is its library (Ian Darwin, Christos Zoulas and the contributors of
the release). The subset of the magic files, the port layer and the seven
hunks of the port are in [`third_party/file`](../third_party/file/README.md).
The BSD-2-Clause license asks that the copyright notice and the conditions
travel with a binary: the MicroFS image carries `/etc/licenses/file` beside
`/bin/file` and its database, `tools/bundle.sh` puts `LICENSE.file` into the
x86-64 archive, and a package that installs `file` copies the same file beside
the program. The magic database of the image is a *subset* of the rules of the
release and not the whole of it; the notice of the release covers it the same
way.

## The rule

1. **The MIT tree takes permissive code only.** MIT, BSD-2-Clause,
   BSD-3-Clause, ISC, Apache-2.0 (with its patent grant) and the zlib license
   may live in `components/`, `ports/`, `main/`, `tools/` and
   `third_party/`. A new one is a line in the table above, a license file in
   its directory and a note in this page.
2. **Copyleft does not go into the kernel image.** GPL, LGPL, AGPL and CDDL
   code is not linked into a binary of the MIT tree. A driver or a library
   under such a license lives in a repository of its own, or runs as a
   program of the system (a user-space driver), with a documented interface;
   the user chooses to install it. This is the boundary ADR 0025 records.
3. **No code from the Linux kernel.** Its `font_8x16.c`, its drivers and its
   headers are GPL-2.0-only; they are not copied into this tree. A driver
   ported from a permissive source (FreeBSD, OpenBSD, NetBSD) keeps its
   notice and is listed here.
4. **A binary carries the licenses of what it contains.** `tools/bundle.sh`
   puts `LICENSE.spleen` into the x86-64 archive; a new third-party part adds
   its license to the same bundle. The board image contains no third-party
   code today.
5. **The notice stays with the source.** A generated file names its origin
   and its SPDX identifier; a vendored file keeps its copyright lines.

## What is not in the tree, and why

The Linux kernel module ABI is not a stable ABI: a `.ko` is built for one
kernel and one configuration, and a GPU driver leans on DRM, DMA and power
management that this kernel does not have. The tree therefore does not load
`.ko`, and a future driver of that kind is a separate, optional component
(ADR 0025), not a file of `components/`.
