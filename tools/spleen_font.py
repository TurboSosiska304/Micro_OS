#!/usr/bin/env python3
"""Micro OS - generate the 8x16 console font of x86-64 from Spleen.

The font of the x86-64 console is Spleen 8x16 (BSD-2-Clause). This tool
reads the BDF of the upstream release and writes the C table the port uses:
the 95 glyphs of the printable ASCII, one byte a row, the most significant
bit the leftmost pixel - the order of a VGA and a PSF font.

Usage:
    python3 tools/spleen_font.py third_party/spleen/spleen-8x16.bdf \
        ports/port_x86_64/src/font_spleen.c

The BDF is committed, so the table can be regenerated and compared; a new
version of Spleen is a new file in third_party/spleen and a new run of this
tool. The license and the attribution travel with the file (see
third_party/spleen/README.md); a copy of the license goes into the x86-64
bundle of tools/bundle.sh.
"""

from __future__ import annotations

import pathlib
import sys

FIRST = 0x20
LAST = 0x7E
HEIGHT = 16

HEADER = """/*
 * Micro OS - the 8x16 font of the x86-64 console.
 *
 * The glyphs of the printable ASCII, one byte a row and the most
 * significant bit the leftmost pixel, taken from Spleen 8x16 2.3.0.
 * Spleen is a font of Frederic Cambus, released under the BSD 2-Clause
 * license; the license is in third_party/spleen/LICENSE and the note in
 * third_party/spleen/README.md. The 5x7 font of the board is a different
 * font and stays in components/font.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "port_x86_64_font.h"

const k_u8_t k_font8x16_ascii[95][FONT8_HEIGHT] = {
"""

FOOTER = """};

const k_u8_t *font8x16_glyph(char character)
{
    unsigned char code = (unsigned char)character;

    if (code < 0x20u || code > 0x7eu) {
        code = (unsigned char)'?';
    }

    return k_font8x16_ascii[code - 0x20u];
}
"""


def read_bdf(path: pathlib.Path) -> dict[int, list[int]]:
    glyphs: dict[int, list[int]] = {}
    encoding = None
    rows: list[int] = []
    in_bitmap = False

    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("STARTCHAR"):
            encoding = None
            rows = []
            in_bitmap = False
        elif line.startswith("ENCODING"):
            encoding = int(line.split()[1])
        elif line.startswith("BITMAP"):
            in_bitmap = True
        elif line.startswith("ENDCHAR"):
            if encoding is not None:
                glyphs[encoding] = rows
            in_bitmap = False
        elif in_bitmap:
            rows.append(int(line.strip(), 16))

    return glyphs


def name_of(code: int) -> str:
    if code == 0x20:
        return "space"
    if chr(code).isalnum():
        return chr(code)
    return "0x%02x" % code


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    source = pathlib.Path(argv[1])
    target = pathlib.Path(argv[2])
    glyphs = read_bdf(source)

    for code in range(FIRST, LAST + 1):
        if code not in glyphs:
            print(f"the BDF has no glyph for 0x{code:02x}", file=sys.stderr)
            return 1
        if len(glyphs[code]) != HEIGHT:
            print(f"the glyph 0x{code:02x} has {len(glyphs[code])} rows", file=sys.stderr)
            return 1

    lines = [HEADER.rstrip("\n")]
    for code in range(FIRST, LAST + 1):
        body = ", ".join("0x%02x" % row for row in glyphs[code])
        lines.append("    {%s}, /* %s */" % (body, name_of(code)))
    lines.append(FOOTER.rstrip("\n"))
    lines.append("")

    target.write_text("\n".join(lines), encoding="utf-8")
    print(f"spleen: {target} ({LAST - FIRST + 1} glyphs of {HEIGHT} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
