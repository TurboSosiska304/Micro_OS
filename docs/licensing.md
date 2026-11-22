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
| Mbed TLS | 3.6.2 | Apache-2.0 (of the dual Apache-2.0 / GPL-2.0-or-later) | `third_party/mbedtls` | the x86-64 network image, and the host tests; `LICENSE.mbedtls` in the x86-64 tar |
| PCRE2 | 10.42 | BSD-3-Clause WITH PCRE2-exception | `third_party/pcre2`, `ports/port_x86_64/programs/pcre2grep.c` | `/bin/pcre2grep` of the x86-64 MicroFS image; `/etc/licenses/pcre2` in the image and `LICENSE.pcre2` in the x86-64 tar |
| Lua | 5.4.7 | MIT | `third_party/lua`, `ports/port_x86_64/programs` (the `main` is the release's `lua.c`) | `/bin/lua` of the x86-64 MicroFS image; `/etc/licenses/lua` in the image and `LICENSE.lua` in the x86-64 tar |
| sbase | c546c3a (tip) | MIT | `third_party/sbase`, the ten commands `cat grep head ls tail wc sort find xargs sed` | `/bin/cat`, `/bin/grep`, `/bin/head`, `/bin/ls`, `/bin/tail`, `/bin/wc`, `/bin/sort`, `/bin/find`, `/bin/xargs` and `/bin/sed` of the x86-64 MicroFS image; `/etc/licenses/sbase` in the image and `LICENSE.sbase` in the x86-64 tar |

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
Mbed TLS is the TLS stack of the x86-64 port (ADR 0024 §3), vendored from the
3.6.2 release with only a configuration and a CMake file of this tree beside
it; the note and the sum of the tarball are in
[`third_party/mbedtls`](../third_party/mbedtls/README.md). The library is
dual-licensed and Micro OS takes the **Apache-2.0** half (its patent grant is
compatible with the MIT tree; the rule 1 below names it). The board does not
link this copy: ESP-IDF carries its own, under the configuration of the
firmware. `tools/bundle.sh` puts the upstream license into every x86-64
archive as `LICENSE.mbedtls`.

PCRE2 is a regular expression library by Philip Hazel and the PCRE2
contributors; the release, the archive sum, the configuration and the two
optimization passes the image leaves out are in
[`third_party/pcre2`](../third_party/pcre2/README.md). The license is the
BSD-3-Clause of the release with the PCRE2 exception for a binary that uses
the library without shipping its source: the three conditions stay (the
notice, the list of the conditions and the disclaimer), and the exception
only settles the form of the binary. The MicroFS image carries
`/etc/licenses/pcre2` beside `/bin/pcre2grep`, and `tools/bundle.sh` puts
`LICENSE.pcre2` into the x86-64 archive. The text stays PCRE2's: it is not
merged into the MIT `LICENSE` of the tree.

Lua is a language by the team of PUC-Rio; the version, the files, the port
layer and the one configuration file of the port are in
[`third_party/lua`](../third_party/lua/README.md). Its MIT license asks for
the copyright notice and the permission text to travel with a copy of the
software, so the image carries `/etc/licenses/lua` beside `/bin/lua` and
`tools/bundle.sh` puts `LICENSE.lua` into the x86-64 archive. The text is the
MIT of the release and not a second copy of the MIT of this tree: the two
notice lines are different and the tree keeps both.

sbase is a collection of Unix tools by suckless; the subset, the commit and
the two hunks of the port are in
[`third_party/sbase`](../third_party/sbase/README.md). Its license is the MIT
text of the collection, which names its authors. The MicroFS image carries
`/etc/licenses/sbase` beside the ten commands, `tools/bundle.sh`
puts `LICENSE.sbase` into the x86-64 archive, and a package that installs a
tool (`microos-progs` of a release) copies the same file beside it. The
notice is not merged into the MIT `LICENSE` of the tree: the two are the same
license but not the same text.

## The rule

1. **The MIT tree takes permissive code only.** MIT, BSD-2-Clause,
   BSD-3-Clause (with or without an exception such as PCRE2's), ISC,
   Apache-2.0 (with its patent grant) and the zlib license may live in `components/`, `ports/`, `main/`, `tools/` and
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
   puts `LICENSE.spleen`, `LICENSE.zlib`, `LICENSE.pcre2`, `LICENSE.lua`,
   `LICENSE.sbase` and `LICENSE.mbedtls` into the x86-64 archive; a new
   third-party part adds its license to the same bundle. The board image
   contains the mbedTLS that ESP-IDF links into it (Apache-2.0), which is not
   a copy of this tree.
5. **The notice stays with the source.** A generated file names its origin
   and its SPDX identifier; a vendored file keeps its copyright lines.

## What is not in the tree, and why

The Linux kernel module ABI is not a stable ABI: a `.ko` is built for one
kernel and one configuration, and a GPU driver leans on DRM, DMA and power
management that this kernel does not have. The tree therefore does not load
`.ko`, and a future driver of that kind is a separate, optional component
(ADR 0025), not a file of `components/`.
