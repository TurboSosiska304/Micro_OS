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

Spleen is a font by Frederic Cambus; the license and the note are in
[`third_party/spleen`](../third_party/spleen/README.md). The 8x16 table is
generated from the upstream BDF by `tools/spleen_font.py`, and the BSD
2-Clause license asks that its notice travel with a binary: the x86-64
bundle carries a copy.

## The rule

1. **The MIT tree takes permissive code only.** MIT, BSD-2-Clause,
   BSD-3-Clause, ISC and Apache-2.0 (with its patent grant) may live in
   `components/`, `ports/`, `main/` and `tools/`. A new one is a line in the
   table above, a license file in its directory and a note in this page.
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
