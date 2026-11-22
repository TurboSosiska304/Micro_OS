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
   puts `LICENSE.spleen` and `LICENSE.mbedtls` into the x86-64 archive; a new
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
