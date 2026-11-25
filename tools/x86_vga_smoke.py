#!/usr/bin/env python3
"""Micro OS — the screen of the x86-64 port: the framebuffer, the text mode and
the keyboard.

The smoke test of the port (tools/x86_smoke.py) drives the machine through
COM1. This one looks at the screen: the framebuffer of the VGA of QEMU
(ADR 0023, 1280x800, 16 bits per pixel) and the text mode of 80x25 that is the
fallback of a machine whose Bochs VBE interface does not answer. The keyboard
is the other half of the window: it is the only way a person who runs the
machine in a window can type at all.

There is no display here, and none is needed: QEMU with -display none still
has its video device, and its monitor reads the picture (screendump) and
presses keys (sendkey). The test boots the machine seven times and checks the
two screens of ADR 0023 and the programs of the interface (ADR 0028, ADR
0030, ADR 0054):

  1. the framebuffer (-vga std, the default): the boot says the screen is the
     framebuffer of 1280x800 by 128x57 cells, the screenshot is a PPM of
     1280x800, and the cells of the first line are the glyphs of the 5x7 font
     at twice their size - the letter 'M' of "MicroOS" is compared with the
     bitmap of the font, pixel by pixel, so the test reads the text and not
     only the ink. The cursor is the one cell that is entirely ink. A command
     typed through the keyboard reaches the shell (IRQ1, the scancode map and
     the editing keys), its answer is on the serial console and the picture
     grows below the prompt, and `config set console.theme light` repaints the
     cells in the colours of /etc/console: the paper of the picture becomes
     the light grey of the VGA and the glyphs become black;
  2. the text mode (-vga cirrus, a card without the Bochs VBE interface): the
     boot says it kept the text mode, the screen is read out of the memory at
     0xB8000 with pmemsave as it always was, the picture of the device is
     720x400, the cursor of the CRTC is where the prompt ends, and the same
     typed command and theme reach the text cells and their attribute;
  3. the interface of ADR 0028: the machine boots the disk of `make
     x86-disk`, `exec /bin/ui` takes the screen (`FBIO_TAKE`), the screenshot
     of the panel and of the window of the log is not the console any more,
     Tab moves the focus of the list and `q` gives the screen back
     (`FBIO_GIVE`), after which the picture is the grid of the shell again;
  4. the editor of ADR 0030: a copy of the disk, `exec /bin/edit /edit.txt`,
     two lines typed through the keyboard, Enter, Ctrl+S and Ctrl+Q, then a
     second boot of the same disk and `cat /edit.txt`, which has to print the
     two lines; the screenshot is read for the title, the status line and the
     cell of the cursor;
  5. the file manager of ADR 0030: `/bin/fm /` on a file the shell made, its
     title, names and selected row, the arrows and End, the modal window of
     `d` answered with `n` and then `y`, and the list and the status line
     after the delete;
  6. the desk of a person (ADR 0054): a machine told by `/etc/session` to run
     `/bin/wm` at the boot starts on the desktop and not at the prompt; the
     panel shows the hour of the machine and the second of the desk is the
     rectangle of the clock; Alt+T opens `/bin/term`, a line typed into the
     window is a command of a child process and its answer is the text of the
     window, the meter of the server (F12) says a key costs a row and not a
     screen, two screenshots around a key differ only in the row of the key,
     Ctrl+C stops the command, Ctrl+T is the second size of the font, and the
     last window gives the prompt of the shell back. The pictures of the
     windows are kept in the directory of `--shots`;
  7. the same desk started by a person at the prompt (the laptop that died on
     `exec /bin/wm`): the machine is given SMEP and SMAP, the shell types the
     line, and a window and a living shell after the desk prove that every
     copy the kernel made of the memory of the program was inside the window
     of ADR 0052.

    tools/x86_vga_smoke.py [--timeout SECONDS] [--display none|gtk] [--verbose]
                           [--image PATH] [--shots DIR]

The exit status is 0 when the screens held and 1 otherwise; 2 when the image
or QEMU is missing. The test needs no X and no window, so a machine of a build
farm can run it; it is kept out of `make ci` because the monitor of QEMU is a
tool of its own and the serial smoke test already guards the boot.

With `--display gtk` the same cards are checked against the real window of
QEMU instead of its memory: the screendump is then the picture of the window
of the machine, drawn by the display backend, and a person can watch the keys
arrive as they are typed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# The prompt of the shell: it is written in one call, so it is never split by
# the log of another task, and the boot is over when it is on the screen.
PROMPT = "microos> "

# The text screen of the mode 3 of the VGA: 80 columns, 25 rows, two bytes a
# cell (the character and the colour), and that is the whole of the memory
# the fallback test reads.
VGA_BASE = 0xB8000
VGA_COLUMNS = 80
VGA_ROWS = 25
VGA_BYTES = VGA_COLUMNS * VGA_ROWS * 2

# The picture of ADR 0023 and the cell of its font: the 8x16 font of the PC
# (Spleen), one glyph a cell and the step of a line being 16 plus the gap of
# /etc/font (`font.line_gap`, 0, 2 or 4 pixels).
VBE_WIDTH = 1280
VBE_HEIGHT = 800
CELL_WIDTH = 8
CELL_HEIGHT = 16
LINE_PITCH = 16
LINE_PITCH_GAP2 = 18
ROWS_GAP0 = VBE_HEIGHT // LINE_PITCH  # 50
ROWS_GAP2 = (VBE_HEIGHT - CELL_HEIGHT) // LINE_PITCH_GAP2 + 1  # 44
COLUMNS = VBE_WIDTH // CELL_WIDTH  # 160

# The two colours of the console, as the display makes them of RGB565 0xAD55
# (the light grey of the VGA) and 0x0000. The theme of /etc picks which is the
# ink and which is the paper; the inversion swaps the two.
GREY = (173, 170, 173)
BLACK = (0, 0, 0)


def rgb565(value: int) -> tuple:
    """A pixel of the format of ADR 0023, widened by the display.

    The device keeps sixteen bits: five of red, six of green and five of blue,
    and the display of QEMU widens each of them to a byte (the low bits repeat
    the high ones, which is what 0x0010 -> (0, 0, 132) shows). The tests of the
    programs of ADR 0030 name their colours as the sources do.
    """
    red = (value >> 11) & 0x1F
    green = (value >> 5) & 0x3F
    blue = value & 0x1F
    return ((red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2))


def brightness(rgb: tuple) -> int:
    """The brightness of a colour of the screen, the way a person sees it.

    The checks of the palette ask that one tone is darker than another - the
    shadow under a window, the ink of a title against the ink of its body -
    and a tone is a number of the three channels and not one of them.
    """
    red, green, blue = rgb
    return (2126 * red + 7152 * green + 722 * blue) // 10000


def desk_at(picture: "Picture", x: int, y: int) -> bool:
    """The pixel is the wallpaper of its row, not a window, a panel or a shadow.

    The desk at (1250, y) is the reference: no window of the default desktop,
    and no window the test drags, reaches that column at the rows the checks
    ask about, and the grid of the wallpaper has no line there (1250 is not
    the offset of a line of 32).
    """
    return picture.pixel(x, y) == picture.pixel(1250, y)


# The colours of the sixteen attributes of the palette (ports/port_x86_64/
# src/fb.c) that the test reads: a command colours a name or a line with one
# of them, and the value is the five-six-five number widened by the display.
# Green is 0x0540, blue 0x0015, amber (the brown of a CGA) 0xAD40 and red
# 0xA800.
GREEN = (0, 170, 0)
BLUE = (0, 0, 173)
AMBER = (173, 170, 0)
RED = (173, 0, 0)

# The answer of the command the keyboard types: it is the checker of the
# filesystem, and the volume it checks is the one the boot mounted.
ANSWER = "the volume is consistent"

# The interface of ADR 0028 (`/bin/ui`): the layout and the colours of the
# program in ports/port_x86_64/programs/ui.c. The test reads a rectangle of
# the screenshot and not a copy of the drawing code: the panel is the dark
# blue of the status line (RGB565 0x0010), the focused row of the list is the
# light grey of the VGA and the window of the log is black with a dark grey
# border.
UI_PANEL_RGB = (0, 0, 132)
UI_PANEL_HEIGHT = 16
UI_LEFT_RGB = BLACK
UI_TEXT_RGB = (255, 255, 255)
UI_LEFT_WIDTH = 176
UI_ITEM_X = 4
UI_ITEM_WIDTH = 168
UI_ITEM_Y0 = 24
UI_ITEM_STEP = 22
UI_CONSOLE_X = UI_LEFT_WIDTH + 8
UI_CONSOLE_Y = 16 + 8

# The palette of the desktop (components/ui/include/ui/palette.h): a colour is
# written with eight bits a channel, as a person writes one, and the device of
# ADR 0023 narrows it to five-six-five. The roles are named and not numbered,
# so a screenshot is read in the language of the sources: the surface of a
# window, the ink of a body, the accent of the focus.
def palette_rgb(red: int, green: int, blue: int) -> tuple:
    """A colour of the palette, the way `UI_RGB` narrows it and the display
    widens it: the low bits of a channel are dropped, not rounded."""
    return rgb565(
        ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | ((blue & 0xF8) >> 3)
    )


WALL_TOP_RGB = palette_rgb(16, 19, 27)
WALL_BOTTOM_RGB = palette_rgb(28, 37, 54)
WALL_GRID_RGB = palette_rgb(20, 26, 36)
SURFACE_RGB = palette_rgb(24, 29, 39)
SURFACE_HIGH_RGB = palette_rgb(36, 44, 59)
PANEL_RGB = palette_rgb(20, 25, 35)
INK_RGB = palette_rgb(216, 223, 235)
INK_DIM_RGB = palette_rgb(146, 160, 181)
TITLE_INK_RGB = palette_rgb(244, 248, 255)
ACCENT_RGB = palette_rgb(76, 141, 255)
ACCENT_INK_RGB = palette_rgb(255, 255, 255)
BORDER_RGB = palette_rgb(58, 70, 92)
SELECTION_RGB = palette_rgb(46, 63, 92)
SELECTION_INK_RGB = palette_rgb(255, 255, 255)
CLOSE_RGB = palette_rgb(208, 80, 75)
CLOSE_INK_RGB = palette_rgb(255, 255, 255)
CURSOR_INK_RGB = palette_rgb(255, 255, 255)
CURSOR_EDGE_RGB = palette_rgb(0, 0, 0)

# The light theme of the same roles: the `theme=light` of /etc/console.
WALL_TOP_LIGHT_RGB = palette_rgb(238, 242, 248)
WALL_BOTTOM_LIGHT_RGB = palette_rgb(203, 213, 228)
PANEL_LIGHT_RGB = palette_rgb(220, 227, 238)

# The programs of ADR 0030 (/bin/edit and /bin/fm) and the frame of
# components/ui: four pixels of air above the row of the title, six at either
# side of it and of the body, a pixel of separator, so the body of a window
# starts at the pixel 21 of its own rectangle.
UI_FRAME_TITLE_HEIGHT = 21
UI_TITLE_Y = 4
UI_FRAME_MARGIN = 4
UI_FRAME_PAD = 6
UI_STATUS_Y = VBE_HEIGHT - CELL_HEIGHT

# The colours of the two programs, the roles of the palette widened by the
# display: the surface of a window, its bar and the line of status, the three
# inks of the hierarchy, the accent of the focus and the selection of a list.
EDIT_PAPER_RGB = SURFACE_RGB
EDIT_INK_RGB = INK_RGB
EDIT_TITLE_INK_RGB = TITLE_INK_RGB
EDIT_DIM_INK_RGB = INK_DIM_RGB
EDIT_BAR_RGB = SURFACE_HIGH_RGB
EDIT_FOCUS_RGB = ACCENT_RGB
FM_PAPER_RGB = SURFACE_RGB
FM_INK_RGB = INK_RGB
FM_TITLE_INK_RGB = TITLE_INK_RGB
FM_DIM_INK_RGB = INK_DIM_RGB
FM_BAR_RGB = SURFACE_HIGH_RGB
FM_FOCUS_RGB = ACCENT_RGB
FM_SELECT_RGB = SELECTION_RGB
FM_SELECT_INK_RGB = SELECTION_INK_RGB
FM_MODAL_X = 416
FM_MODAL_Y = 371

# The desktop of 1.7 (`/bin/wm`, docs/desktop.md) and the numbers of its
# source: the gradient and the grid of the wallpaper, the panel, the accent of
# the button of the window with the keys and of the ring of the focus, the red
# square that closes a window and the pointer of white with a black outline.
# The rectangles are the two windows the server starts.
WM_WALL_GRID = 32
WM_WALL_GRID_OFFSET = 16
WM_PANEL_RGB = PANEL_RGB
WM_PANEL_BUTTON_RGB = SURFACE_RGB
WM_PANEL_INK_RGB = INK_DIM_RGB
WM_PANEL_FOCUS_RGB = ACCENT_RGB
WM_PANEL_ACCENT_INK_RGB = ACCENT_INK_RGB
WM_FOCUS_RGB = ACCENT_RGB
WM_CLOSE_RGB = CLOSE_RGB
WM_CURSOR_RGB = CURSOR_INK_RGB
WM_CURSOR_EDGE_RGB = CURSOR_EDGE_RGB
WM_PANEL_HEIGHT = 24
# The two windows of the default desktop: beside each other and not over each
# other, so the corner of one never hides the text of the other (docs/desktop.md).
WM_EDIT_RECT = (40, 40, 640, 420)
WM_FM_RECT = (700, 200, 520, 380)
# The square of closing at the right end of a button of the panel: the same
# cross a title bar has, twelve pixels of it in a button of the panel.
WM_PANEL_CLOSE = 12
WM_TITLE_KEEP = 64
# The shadow of a window: three pixels of a darker wallpaper at its right and
# below, and the ring of the focus around it.
WM_SHADOW = 3
WM_FOCUS_RING = 2

# The terminal of 2.0 (`/bin/term`, ADR 0054): the rectangle the server gives
# the first one (the middle of the desk, over the two windows of 1.7), the cell
# of the small font and of the large one, and the words of its greeting.
WM_TERM_RECT = (320, 178, 640, 420)
WM_TERM_PAD = UI_FRAME_PAD
TERM_CELL_LARGE = 2
TERM_GREETING = "Micro OS terminal. Ctrl+T"
TERM_PROMPT = "$ "
# The message of the second size of the text: the line the window writes when
# Ctrl+T changes the cell from 8x16 to 16x32.
TERM_LARGE_TEXT = "term: the text is 16x32"

# The clock of the panel: eight cells of the twenty-four hours, "15:04:05",
# at the right end of the bar with the air of the panel around it (the source
# is ports/port_x86_64/programs/wm.c and components/ui/src/clock.c).
WM_CLOCK_CELL = CELL_WIDTH
WM_CLOCK_PAD = 8
WM_CLOCK_RECT = (VBE_WIDTH - WM_CLOCK_PAD - 8 * WM_CLOCK_CELL, VBE_HEIGHT - WM_PANEL_HEIGHT + 4,
                 8 * WM_CLOCK_CELL, CELL_HEIGHT)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-x86")

# The directory the screenshots of the run are kept in (`--shots`): a person
# reads the picture of a window of the desktop without running QEMU, and the
# report of a lane names the file it looked at.
SHOTS_DIR = None

# The file of the font the console draws with, and the size of a cell of it in
# the picture: the 8x16 font of the PC (Spleen, ports/port_x86_64).
FONT_SOURCE = os.path.join(ROOT, "ports/port_x86_64/src/font_spleen.c")


def board_line() -> str:
    """The line of the board of the port, read from the source of the machine.

    The x86-64 port is not QEMU's board - the name says what the machine of the
    port is - and a rename there is one edit of `kmain.c` and not one edit of
    every smoke of the port (`board: x86_64` since the wire of 2.0).
    """
    try:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(root, "ports/port_x86_64/src/kmain.c")
        with open(path, encoding="utf-8") as handle:
            found = re.search(r'\.name\s*=\s*"([^"]+)"', handle.read())
        if found is not None:
            return "board: " + found.group(1)
    except OSError:
        pass
    return "board: x86_64"


def load_font() -> dict:
    """The 8x16 font the picture is made of, read from its table in the tree.

    The test draws no glyph itself: it takes the table the kernel was built
    with, so a change of the font - a glyph redrawn, a row moved - is a
    failure of this test and not of a copy of the table inside it. One byte a
    row, the most significant bit the leftmost pixel, the table covers the 95
    codes of the printable ASCII (0x20 to 0x7E).
    """
    try:
        with open(FONT_SOURCE, encoding="utf-8") as handle:
            source = handle.read()
    except OSError as error:
        raise RuntimeError(f"cannot read the font {FONT_SOURCE}: {error}") from error

    rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},\s*){15}0x[0-9A-Fa-f]{2})\}", source)
    if len(rows) != 95:
        raise RuntimeError(f"the font {FONT_SOURCE} has {len(rows)} glyph(s), not 95")

    font = {}
    for index, row in enumerate(rows):
        glyph = tuple(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", row))
        font[chr(0x20 + index)] = glyph

    return font


GLYPHS = load_font()


def title_height(scale: int = 1) -> int:
    """The height of the bar of a window in a font of `scale` pixels a bit.

    The source is `UI_FRAME_TITLE_HEIGHT(font)` of `ui/draw.h`: four pixels of
    air, the row of the font and a pixel of separator. The second size of the
    text of the terminal makes the bar taller as well, so the body of the
    window starts at the thirty-seventh pixel and not at the twenty-first.
    """
    return UI_FRAME_MARGIN + CELL_HEIGHT * scale + 1


def body_y(rect: tuple, row: int, scale: int = 1) -> int:
    """The pixel of the row `row` of the body of the window `rect`."""
    return rect[1] + title_height(scale) + row * CELL_HEIGHT * scale


def text_at(
    picture,
    rect: tuple,
    letters: str,
    row: int | None = None,
    body: bool = True,
    ink: tuple | None = None,
    paper: tuple | None = None,
) -> bool:
    """A run of letters in a window of the desk: its title bar or a body row.

    `row` is a row of the body and `body` false names the title bar, which is
    the pixels of the client at the margin of its frame (docs/desktop.md). The
    colours are the roles of the palette: the ink of the title on the bar of a
    window, the ink of the body on the paper of a window.
    """
    if picture is None:
        return False

    x = rect[0] + UI_FRAME_PAD

    if body and row is not None:
        return picture.text_at(
            x,
            body_y(rect, row),
            letters,
            INK_RGB if ink is None else ink,
            SURFACE_RGB if paper is None else paper,
        )

    return picture.text_at(
        x,
        rect[1] + UI_TITLE_Y,
        letters,
        TITLE_INK_RGB if ink is None else ink,
        SURFACE_HIGH_RGB if paper is None else paper,
    )


def text_at_scaled(
    picture,
    rect: tuple,
    letters: str,
    scale: int,
    row: int | None = None,
    body: bool = True,
    ink: tuple | None = None,
    paper: tuple | None = None,
) -> bool:
    """A run of letters in a window whose text is the font of `scale`."""
    if picture is None:
        return False

    x = rect[0] + UI_FRAME_PAD

    if body and row is not None:
        return picture.text_at_scaled(
            x,
            body_y(rect, row, scale),
            letters,
            INK_RGB if ink is None else ink,
            SURFACE_RGB if paper is None else paper,
            scale,
        )

    return picture.text_at_scaled(
        x,
        rect[1] + UI_FRAME_MARGIN,
        letters,
        TITLE_INK_RGB if ink is None else ink,
        SURFACE_HIGH_RGB if paper is None else paper,
        scale,
    )


class Picture:
    """A PPM of the screen as a grid of pixels, and the cells of the font.

    The screenshot of QEMU is a binary PPM (P6): a header of three values and
    then three bytes a pixel, from the top left. A cell of the console is eight
    pixels wide and sixteen tall, and the glyph inside it is the 8x16 font of
    the PC - so a cell is read as the bitmap of a letter and compared with the
    table of the font. The step of a line is the cell plus the gap of
    /etc/font, and the methods that walk the grid take that step as an
    argument.
    """

    def __init__(self, width: int, height: int, pixels: bytes) -> None:
        self.width = width
        self.height = height
        self.pixels = pixels

    def pixel(self, x: int, y: int) -> tuple:
        index = (y * self.width + x) * 3
        return tuple(self.pixels[index : index + 3])

    def ink_pixels(self) -> int:
        """The bytes of the picture that are not black: an ink that a cell has."""
        return sum(1 for byte in self.pixels if byte)

    def only_colors(self, ink: tuple, paper: tuple) -> bool:
        """Every pixel of the picture is one of the two colours of the console.

        A pixel that is neither is a pixel no cell of the console drew: the
        grey of the text mode, the palette of the firmware, the leftovers of
        a frame that was on the glass before this one. The counts of the three
        channels are compared as well, so a pixel that mixes the two is a
        foreign pixel and not a colour between them.
        """
        red = self.pixels[0::3]
        green = self.pixels[1::3]
        blue = self.pixels[2::3]

        if not (
            set(red) <= {paper[0], ink[0]}
            and set(green) <= {paper[1], ink[1]}
            and set(blue) <= {paper[2], ink[2]}
        ):
            return False

        return red.count(ink[0]) == green.count(ink[1]) == blue.count(ink[2])

    def text_at(self, x: int, y: int, letters: str, ink: tuple, paper: tuple) -> bool:
        """A run of glyphs at a pixel, not at a cell of the console grid.

        The console draws its rows on the lines of the grid; a program of an
        interface draws a title at the margin of its frame and a row of text
        where the model of `components/ui` puts it, and those rows are not at
        a multiple of the sixteen pixels of a cell. The test compares the
        eight by sixteen block at the pixel with the table of the font, so a
        word of a program is read as the cells of the console are.
        """
        for index, letter in enumerate(letters):
            glyph = GLYPHS[letter]
            x0 = x + index * CELL_WIDTH

            if x0 < 0 or y < 0 or x0 + CELL_WIDTH > self.width or y + CELL_HEIGHT > self.height:
                return False

            for row, bits in enumerate(glyph):
                for column in range(CELL_WIDTH):
                    expected = ink if bits & (0x80 >> column) else paper

                    if self.pixel(x0 + column, y + row) != expected:
                        return False

        return True

    def text_at_scaled(
        self, x: int, y: int, letters: str, ink: tuple, paper: tuple, scale: int
    ) -> bool:
        """A run of glyphs in a cell of `scale` pixels a bit of the font.

        The second size of the text of a window (ADR 0054) is the table of the
        console drawn as squares of two by two: the test reads it the same way
        it reads the small one and compares the block of the cell.
        """
        cell_w = CELL_WIDTH * scale
        cell_h = CELL_HEIGHT * scale

        for index, letter in enumerate(letters):
            glyph = GLYPHS[letter]
            x0 = x + index * cell_w

            if x0 < 0 or y < 0 or x0 + cell_w > self.width or y + cell_h > self.height:
                return False

            for row, bits in enumerate(glyph):
                for column in range(CELL_WIDTH):
                    expected = ink if bits & (0x80 >> column) else paper

                    for dy in range(scale):
                        for dx in range(scale):
                            if self.pixel(x0 + column * scale + dx,
                                          y + row * scale + dy) != expected:
                                return False

        return True

    def letters_at_scaled(
        self, x: int, y: int, cells: int, ink: tuple, paper: tuple, scale: int
    ) -> str:
        """The letters of `cells` cells of a scaled font; '?' for a foreign one."""
        letters = []

        for index in range(cells):
            letters.append(
                self.scaled_glyph_at(x + index * CELL_WIDTH * scale, y, ink, scale) or "?"
            )

        return "".join(letters)

    def scaled_glyph_at(self, x: int, y: int, ink: tuple, scale: int) -> str | None:
        """The letter of a cell of a scaled font, or None for a block of no glyph."""
        table = {glyph: letter for letter, glyph in GLYPHS.items()}
        rows = []

        if x < 0 or y < 0 or x + CELL_WIDTH * scale > self.width or (
            y + CELL_HEIGHT * scale > self.height
        ):
            return None

        for row in range(CELL_HEIGHT):
            value = 0

            for column in range(CELL_WIDTH):
                if self.pixel(x + column * scale, y + row * scale) == ink:
                    value |= 0x80 >> column

            rows.append(value)

        return table.get(tuple(rows))

    def glyph_at(self, x: int, y: int, ink: tuple, paper: tuple) -> str | None:
        """The letter of the cell drawn at a pixel, or None.

        The block of eight by sixteen pixels is read as a bitmap and looked up
        in the table of the font, the way the cells of the console are read;
        the difference is the pixel and not the cell of the grid.
        """
        table = {glyph: letter for letter, glyph in GLYPHS.items()}
        rows = []

        if x < 0 or y < 0 or x + CELL_WIDTH > self.width or y + CELL_HEIGHT > self.height:
            return None

        for row in range(CELL_HEIGHT):
            value = 0

            for column in range(CELL_WIDTH):
                if self.pixel(x + column, y + row) == ink:
                    value |= 0x80 >> column

            rows.append(value)

        return table.get(tuple(rows))

    def letters_at(self, x: int, y: int, cells: int, ink: tuple, paper: tuple) -> str:
        """The letters of `cells` cells from a pixel; '?' for a foreign cell."""
        letters = []

        for index in range(cells):
            letter = self.glyph_at(x + index * CELL_WIDTH, y, ink, paper)
            letters.append(letter if letter is not None else "?")

        return "".join(letters)

    def cell_glyph(self, row: int, column: int, ink: tuple, pitch: int = LINE_PITCH) -> tuple:
        """The 8x16 bitmap of a cell: one byte a line, the top bit on the left.

        The answer is the bitmap and not a letter: comparing it with the table
        of the font is the caller's business. A cell of a line that the picture
        does not hold - a row below the grid - is the empty tuple.
        """
        x0 = column * CELL_WIDTH
        y0 = row * pitch
        want = bytes(ink)
        rows = []

        if x0 + CELL_WIDTH > self.width or y0 + CELL_HEIGHT > self.height:
            return ()

        for row_index in range(CELL_HEIGHT):
            value = 0
            start = ((y0 + row_index) * self.width + x0) * 3

            for column_index in range(CELL_WIDTH):
                if self.pixels[start + column_index * 3 : start + column_index * 3 + 3] == want:
                    value |= 0x80 >> column_index

            rows.append(value)

        return tuple(rows)

    def foreign_cells(self, ink: tuple, pitch: int = LINE_PITCH) -> list:
        """The cells of the grid that no cell of the console could have drawn.

        A cell is the paper, a glyph of the font or the cursor (the ink of the
        whole cell). A cell that is none of the three holds pixels that came
        from somewhere else - a second copy of the text a few pixels below the
        first, a frame of another screen, a scale of the host - and the line
        and the column of each one are the answer.
        """
        table = {glyph: letter for letter, glyph in GLYPHS.items()}
        cells = []
        rows = 0

        while (rows * pitch) + CELL_HEIGHT <= self.height:
            rows += 1

        for row in range(rows):
            for column in range(self.width // CELL_WIDTH):
                bits = self.cell_glyph(row, column, ink, pitch)

                if set(bits) == {0}:
                    continue  # the paper

                if bits in table:
                    continue  # a glyph of the font

                if self.cell_is_ink(row, column, ink, pitch):
                    continue  # the cursor

                cells.append((row, column))

        return cells

    def glyphs_seen(self, ink: tuple, pitch: int = LINE_PITCH) -> set:
        """The letters of the font that the picture holds, one cell each."""
        table = {glyph: letter for letter, glyph in GLYPHS.items()}
        seen = set()
        rows = 0

        while (rows * pitch) + CELL_HEIGHT <= self.height:
            rows += 1

        for row in range(rows):
            for column in range(self.width // CELL_WIDTH):
                letter = table.get(self.cell_glyph(row, column, ink, pitch))

                if letter is not None:
                    seen.add(letter)

        return seen

    def lowest_ink_row(self) -> int:
        """The last line of the picture that has a pixel that is not black."""
        stride = self.width * 3

        for y in range(self.height - 1, -1, -1):
            if any(self.pixels[y * stride : (y + 1) * stride]):
                return y
        return -1

    def cell_is(
        self,
        row: int,
        column: int,
        glyph: tuple,
        ink: tuple,
        paper: tuple,
        pitch: int = LINE_PITCH,
    ) -> bool:
        """The cell is the letter: every pixel of its eight by sixteen rows."""
        x0 = column * CELL_WIDTH
        y0 = row * pitch

        if x0 + CELL_WIDTH > self.width or y0 + CELL_HEIGHT > self.height:
            return False

        for row_index, bits in enumerate(glyph):
            expected = b""
            for column_index in range(CELL_WIDTH):
                expected += bytes(ink if bits & (0x80 >> column_index) else paper)
            start = ((y0 + row_index) * self.width + x0) * 3

            if self.pixels[start : start + CELL_WIDTH * 3] != expected:
                return False

        return True

    def row_of(self, text: str, ink: tuple, paper: tuple, pitch: int = LINE_PITCH) -> int | None:
        """The row of the grid whose first cells spell `text`, or None.

        A console whose log is longer than the glass scrolls, so the first line
        of the boot is not always the first row of the picture: a check that
        reads the banner asks where it is instead of assuming the row 0. A
        machine of a longer boot scrolls one line earlier than a machine of a
        shorter one, and neither is a failure of the screen.
        """
        for row in range(self.height // pitch):
            if all(
                self.cell_is(row, index, GLYPHS[letter], ink, paper, pitch)
                for index, letter in enumerate(text)
            ):
                return row

        return None

    def cell_is_ink(self, row: int, column: int, ink: tuple, pitch: int = LINE_PITCH) -> bool:
        """The cell is one block of the colour: what the cursor is."""
        x0 = column * CELL_WIDTH
        y0 = row * pitch
        expected = bytes(ink) * CELL_WIDTH

        if x0 + CELL_WIDTH > self.width or y0 + CELL_HEIGHT > self.height:
            return False

        for y in range(y0, y0 + CELL_HEIGHT):
            start = (y * self.width + x0) * 3
            if self.pixels[start : start + CELL_WIDTH * 3] != expected:
                return False

        return True

    def cursor_cells(self, ink: tuple, pitch: int = LINE_PITCH) -> int:
        """How many cells are one block of the ink: the cursor of the screen.

        The console draws the cursor as the reverse of a cell - the ink fills
        the eight by sixteen pixels and the letter, when there is one, is in
        the paper - so a cell that is all ink is the cursor and nothing else
        the console draws is.
        """
        count = 0
        rows = 0

        while (rows * pitch) + CELL_HEIGHT <= self.height:
            rows += 1

        for row in range(rows):
            for column in range(self.width // CELL_WIDTH):
                if self.cell_is_ink(row, column, ink, pitch):
                    count += 1

        return count

    def find_run(
        self, letters: str, ink: tuple, paper: tuple, pitch: int = LINE_PITCH
    ) -> tuple | None:
        """The first `(row, column)` of a run of cells that spells `letters`.

        The cells are compared with the table of the font one by one, and
        every one of them has to be the glyph in `ink` on `paper`: the test
        reads the colour of a word and not only the word, which is the point
        of a command that colours a name or a log line that colours a level.
        """
        rows = 0

        while (rows * pitch) + CELL_HEIGHT <= self.height:
            rows += 1

        for row in range(rows):
            # A row that has no pixel of the colour holds no word of it.
            start = (row * pitch) * self.width * 3
            end = ((row * pitch) + CELL_HEIGHT) * self.width * 3
            if bytes(ink) not in self.pixels[start:end]:
                continue

            for column in range(self.width // CELL_WIDTH - len(letters) + 1):
                if all(
                    self.cell_is(row, column + index, GLYPHS[letter], ink, paper, pitch)
                    for index, letter in enumerate(letters)
                ):
                    return (row, column)

        return None


class Machine:
    """The emulated machine, its serial port and its monitor.

    Both devices are unix sockets: the serial port is read as a stream and the
    monitor is a socket that takes one command at a time, so nothing has to be
    parsed out of the console of QEMU itself.
    """

    def __init__(self, loader: str, kernel: str, workdir: str, vga: str, display: str = "none",
                 image: str | None = None, cpu: str | None = None) -> None:
        self.serial_path = os.path.join(workdir, "serial.sock")
        self.monitor_path = os.path.join(workdir, "monitor.sock")
        self.transcript = b""

        # A machine of a person has SMEP and SMAP; the default processor of
        # QEMU does not. A test of a copy the kernel makes from the memory of
        # a program has to ask for the two bits or it checks a machine that
        # never refuses the copy made outside the window of ADR 0052.
        command = ["qemu-system-x86_64"]
        if cpu is not None:
            command += ["-cpu", cpu]
        command += [
            "-kernel",
            loader,
            "-initrd",
            kernel,
            "-m",
            "256",
            "-no-reboot",
            # The card of the machine is the subject of the test and not its
            # instrument: by default no display of QEMU is opened, the video
            # memory and its picture are still there and the monitor reads
            # them with pmemsave and screendump. `--display gtk` opens the real
            # window, and the same cards below are then the picture of it.
            "-display",
            display,
            # `-vga std` is the VGA of QEMU with the Bochs VBE interface
            # (1234:1111, ADR 0023); `-vga cirrus` is a card with a text mode
            # and no such interface, which is the fallback of the port.
            "-vga",
            vga,
            "-serial",
            f"unix:{self.serial_path},server=on,wait=off",
            "-monitor",
            f"unix:{self.monitor_path},server=on,wait=off",
        ]

        # The machine of the interface boots the disk of `make x86-disk`: the
        # two devices are of the kernel, but /bin/ui is a file of the volume
        # like every other program of /bin.
        if image is not None:
            command += [
                "-drive",
                f"file={image},format=raw,if=ide,index=0,cache=writeback",
            ]

        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.serial = self._connect(self.serial_path)
        self.monitor = self._connect(self.monitor_path)
        self._drain_monitor()

    @staticmethod
    def _connect(path: str, timeout: float = 30.0) -> socket.socket:
        """The socket of QEMU, with the patience of a machine under load.

        Ten seconds was enough for a machine of a person and not for a runner
        of a build farm where three QEMUs start at once: the test then said
        "QEMU did not start" and it was the wait that was short, not the
        machine. Half a minute costs nothing when the socket is there at once.
        """
        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            if os.path.exists(path):
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    sock.connect(path)
                except OSError:
                    sock.close()
                else:
                    sock.settimeout(0.2)
                    return sock
            time.sleep(0.05)

        raise RuntimeError(f"no socket at {path}: QEMU did not start")

    def _read(self, sock: socket.socket, window: float) -> bytes:
        """Whatever the socket has, waiting at most `window` seconds."""
        chunk = b""
        sock.settimeout(max(window, 0.01))
        try:
            part = sock.recv(4096)
            while part:
                chunk += part
                if len(chunk) > 65536:
                    break
                part = sock.recv(4096)
        except (socket.timeout, BlockingIOError):
            pass
        return chunk

    def _drain_monitor(self, window: float = 0.2) -> str:
        """The banner and the prompt of the monitor, which are not a command."""
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def read_serial(self) -> str:
        """The serial port so far: the boot log and the echo of the shell."""
        self.transcript += self._read(self.serial, 0.01)
        return self.transcript.decode("utf-8", errors="replace")

    def wait_serial(self, needle: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.read_serial():
                return True
            time.sleep(0.05)
        return False

    def prompts(self) -> int:
        """How many prompts the shell has printed so far.

        The transcript is one stream, so a word of a command is somewhere in
        it from the boot on: the prompt is the mark that the shell finished a
        line, and a count taken before a command is the one the answer of that
        command has to pass.
        """
        return self.read_serial().count(PROMPT)

    def run_line(self, line: str, timeout: float) -> bool:
        """Type a line and wait for the prompt of its answer.

        The count of the prompts is taken before the line goes out: the shell
        answers a command of the files in a few milliseconds, and a count taken
        after it would already carry the answer of this command.
        """
        count = self.prompts()
        deadline = time.monotonic() + timeout

        self.type_line(line)

        while time.monotonic() < deadline:
            if self.read_serial().count(PROMPT) > count:
                return True
            time.sleep(0.05)

        return False

    def type_line(self, line: str) -> None:
        """A line of the shell through the serial port, which is a console too."""
        self.serial.sendall((line + "\n").encode("ascii"))

    def monitor_command(self, command: str, window: float = 2.0) -> str:
        """One command of the human monitor, answered with its own prompt."""
        self.monitor.sendall((command + "\n").encode("ascii"))
        text = b""
        deadline = time.monotonic() + window
        while time.monotonic() < deadline:
            text += self._read(self.monitor, 0.05)
            if b"(qemu)" in text:
                break
        return text.decode("utf-8", errors="replace")

    def screen_text(self, directory: str) -> str:
        """The text of the VGA, read out of the memory of the machine."""
        path = os.path.join(directory, "vga.bin")
        if os.path.exists(path):
            os.unlink(path)

        self.monitor_command(f'pmemsave 0x{VGA_BASE:x} {VGA_BYTES} "{path}"')

        # The dump is written before the monitor answers with its prompt, but
        # a file of a slow machine may still be in the page cache: the wait is
        # a courtesy and the size is the proof.
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if os.path.exists(path) and os.path.getsize(path) == VGA_BYTES:
                break
            time.sleep(0.02)

        try:
            with open(path, "rb") as handle:
                blob = handle.read()
        except FileNotFoundError:
            return ""

        lines = []
        for row in range(VGA_ROWS):
            cells = blob[row * VGA_COLUMNS * 2 : (row + 1) * VGA_COLUMNS * 2]
            text = "".join(
                chr(cells[index]) if 0x20 <= cells[index] < 0x7F else " "
                for index in range(0, len(cells), 2)
            )
            lines.append(text.rstrip())
        return "\n".join(lines)

    def attributes(self, directory: str) -> list:
        """The attribute byte of every cell of the text screen."""
        path = os.path.join(directory, "vga.bin")

        try:
            with open(path, "rb") as handle:
                blob = handle.read()
        except FileNotFoundError:
            return []

        return list(blob[1::2])

    def picture(self, directory: str) -> Picture | None:
        """The screen as a picture: what the device draws, not what it holds.

        screendump asks the video device to draw its screen, which is the
        picture a window would show: the text memory of the fallback and the
        framebuffer of the new screen are two ways to the same call.
        """
        path = os.path.join(directory, "screen.ppm")
        if os.path.exists(path):
            os.unlink(path)

        self.monitor_command(f'screendump "{path}"')

        deadline = time.monotonic() + 5.0
        size = -1
        while time.monotonic() < deadline:
            if os.path.exists(path):
                current = os.path.getsize(path)
                if current > 0 and current == size:
                    break
                size = current
            time.sleep(0.05)

        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except FileNotFoundError:
            return None

        # The header of a binary PPM: P6, the width and the height, and the
        # largest value of a channel. The pixels follow the fourth line.
        lines = data.split(b"\n", 3)
        try:
            width, height = (int(value) for value in lines[1].split())
        except (IndexError, ValueError):
            return None

        return Picture(width, height, lines[3] if len(lines) > 3 else b"")

    def keys(self, names: list) -> None:
        """Press the keys of the monitor: the device interrupts the kernel."""
        for name in names:
            self.monitor_command(f"sendkey {name}")
            time.sleep(0.05)

    def cursor(self) -> int:
        """The position of the cursor of the hardware, out of the CRTC.

        The controller keeps the cursor in the registers 14 and 15 (the low
        and the high byte of one number), addressed through the index port:
        the monitor can read an I/O port, so the position the kernel wrote is
        a number this test can compare with its own idea of the screen.
        """
        position = 0

        for shift, register in ((0, 0x0F), (8, 0x0E)):
            self.monitor_command(f"o /b 0x3d4 0x{register:02x}")
            answer = self.monitor_command("i /b 0x3d5")
            found = re.findall(r"portb\[0x03d5\] = 0x([0-9a-fA-F]+)", answer)
            if len(found) != 1:
                return -1
            position |= int(found[0], 16) << shift

        return position

    def close(self) -> None:
        for sock in (self.serial, self.monitor):
            try:
                sock.close()
            except OSError:
                pass

        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

        if self.process.stdout is not None:
            self.process.stdout.close()


def find_line(text: str, needle: str) -> tuple:
    """The (row, column) of the last line of `text` that holds `needle`.

    The last one, because a word of the boot is on the screen from the boot
    on: the listing or the line a command just wrote is the newest of them.
    """
    lines = text.split("\n")

    for index in range(len(lines) - 1, -1, -1):
        column = lines[index].find(needle)
        if column >= 0:
            return index, column

    return None, -1


def attribute_at(machine: Machine, workdir: str, needle: str) -> int:
    """The attribute byte of the first cell of `needle` on the text screen.

    -1 when the word is not there: the cell is read out of the memory of the
    VGA with `pmemsave`, one byte a cell and the colour in the odd one.
    """
    row, column = find_line(machine.screen_text(workdir), needle)

    if row is None:
        return -1

    attributes = machine.attributes(workdir)
    index = row * VGA_COLUMNS + column

    return attributes[index] if index < len(attributes) else -1


def type_fsck(machine: Machine) -> None:
    """Type `fsck` with a wrong letter erased on the way.

    The wrong letter and the Backspace are part of the test: the shell echoes
    every key it takes, and the echo of a backspace is "\\b \\b", which the
    screen has to erase like a terminal does. A keyboard without the editing
    keys would type "fscxk" and the shell would answer "unknown command",
    which the answer below would not find.
    """
    machine.keys(["f", "s", "c", "x", "backspace", "k", "ret"])


def run_framebuffer(loader: str, kernel: str, timeout: float, verbose: bool, display: str) -> list:
    """The screen of ADR 0023: the framebuffer, its 8x16 font and its line gap."""
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"framebuffer: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-fb-") as workdir:
        machine = Machine(loader, kernel, workdir, "std", display)

        try:
            # --- the first frame: the banner is on the glass
            #
            # The console writes the screen before it writes COM1, so the
            # banner in the transcript is a line that is already on the
            # picture; the dump is taken at once, before the rest of the boot
            # covers it.
            banner = machine.wait_serial(board_line(), timeout)
            expect("the boot reaches the banner", banner, "the banner never reached COM1")

            first = machine.picture(workdir)
            expect(
                "the first frame is the picture of the port",
                first is not None and (first.width, first.height) == (VBE_WIDTH, VBE_HEIGHT),
                f"the first screendump is {first.width}x{first.height}"
                if first is not None
                else "screendump wrote no PPM",
            )

            if first is not None:
                # A pixel that is neither the paper nor the ink is a pixel no
                # cell of this console drew: the grey of the text mode
                # (0xAAAAAA), the palette of the firmware, the leftovers of
                # the screen before this one.
                expect(
                    "the first frame has only the colours of the console",
                    first.only_colors(GREY, BLACK),
                    "a pixel of the first frame is neither black nor the grey of the console",
                )
                # Every cell of the frame is the paper, a glyph of the font or
                # the cursor. A cell that is none of the three holds pixels
                # that came from somewhere else - a second copy of the text a
                # few pixels below the first, at any offset.
                foreign = first.foreign_cells(GREY)
                expect(
                    "every cell of the first frame is a glyph or the paper",
                    not foreign,
                    f"{len(foreign)} cell(s) of the first frame hold no glyph: {foreign[:5]}",
                )
                # The first line of the screen is the banner of the kernel,
                # "MicroOS ...", and the font of the port has the lower case:
                # the cells are the letters themselves and not their upper
                # case, which is what the 5x7 font of the board would draw.
                expect(
                    "the first line of the first frame is the banner",
                    all(
                        first.cell_is(0, index, GLYPHS[letter], GREY, BLACK)
                        for index, letter in enumerate("MicroOS")
                    ),
                    "the cell of the first line is not the glyph of the font",
                )
                # The step of a line is the cell of 16 pixels: the second line
                # of the boot starts on the second row of cells.
                expect(
                    "the lines of the first frame are 16 pixels apart",
                    first.cell_is(1, 0, GLYPHS["b"], GREY, BLACK),
                    "the cell of the second line is not at the step of 16 pixels",
                )

            # --- the screen after the boot
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the serial console is up as before", booted, "the shell never printed its prompt")

            serial = machine.read_serial()
            expect(
                "the port chooses the framebuffer of 160x50 cells",
                "x86-64: the screen is the framebuffer of 1280x800, 16 bpp, 160x50 cells" in serial,
                "the boot did not name the framebuffer of ADR 0023 and its grid",
            )
            expect(
                "the boot applies the font of the PC",
                f"font: line_gap 0, the cell is 8x16 at 16 px, {ROWS_GAP0} rows of {COLUMNS} "
                "columns" in serial,
                "the boot did not name the font, the line step and the grid",
            )

            picture = machine.picture(workdir)
            expect("the device has a picture", picture is not None, "screendump wrote no PPM")

            if picture is not None:
                expect(
                    "the picture is the mode of ADR 0023",
                    (picture.width, picture.height) == (VBE_WIDTH, VBE_HEIGHT),
                    f"the screenshot is {picture.width}x{picture.height}",
                )
                foreign = picture.foreign_cells(GREY)
                expect(
                    "the picture after the boot is one grid of the font",
                    picture.only_colors(GREY, BLACK) and not foreign,
                    f"{len(foreign)} cell(s) of the settled picture hold no glyph: "
                    f"{foreign[:5]}",
                )
                # The lower case of the PC: a 'g', a 'p' and a 'y' are drawn
                # with the tail below the line of the other letters, and the
                # 5x7 font of the board has no lower case at all - so a cell
                # that matches one of them is the glyph of Spleen and not the
                # upper case of a smaller font.
                seen = picture.glyphs_seen(GREY)
                expect(
                    "the font has the lower case with its tails",
                    {"g", "p", "y"} <= seen,
                    f"the picture holds no glyph of {sorted({'g', 'p', 'y'} - seen)}",
                )
                expect(
                    "the cells of the first line are the glyphs of the font",
                    all(
                        picture.cell_is(0, index, GLYPHS[letter], GREY, BLACK)
                        for index, letter in enumerate("MicroOS")
                    ),
                    "a cell of the first line is not the glyph of the font",
                )
                expect(
                    "the theme of the boot is grey on black",
                    picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) == BLACK,
                    f"the paper is {picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2)}",
                )
                cursors = picture.cursor_cells(GREY)
                expect(
                    "the console draws the cursor",
                    cursors == 1,
                    f"{cursors} cell(s) of the picture are all ink",
                )
                expect(
                    "the device draws the text",
                    picture.ink_pixels() > 10000,
                    f"only {picture.ink_pixels()} pixel(s) of the picture are not black",
                )

            # --- the line gap of /etc/font (ADR 0021)
            machine.type_line("config set font.line_gap 2")
            expect(
                "the gap of the font moves the lines",
                machine.wait_serial(
                    f"font: line_gap 2, the cell is 8x16 at {LINE_PITCH_GAP2} px, "
                    f"{ROWS_GAP2} rows of {COLUMNS} columns",
                    timeout,
                ),
                "the console did not lay the screen out at 18 pixels a line",
            )

            # The line of the console is printed when the layout is decided;
            # the repaint of a screen of fifty rows follows it, so the
            # picture is taken again until the grid it asks for is there. A
            # single dump after the line can catch the screen in the middle
            # of its own repaint.
            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.only_colors(GREY, BLACK)
                and not seen.foreign_cells(GREY, LINE_PITCH_GAP2)
                and seen.cell_is(1, 0, GLYPHS["b"], GREY, BLACK, LINE_PITCH_GAP2),
            )
            if picture is not None:
                expect(
                    "the picture of the gap of 2 is 44 rows of 18 pixels",
                    picture.only_colors(GREY, BLACK)
                    and not picture.foreign_cells(GREY, LINE_PITCH_GAP2)
                    and picture.cell_is(1, 0, GLYPHS["b"], GREY, BLACK, LINE_PITCH_GAP2),
                    "the picture is not the grid of 18 pixels a line",
                )

            machine.type_line("cat /etc/font")
            expect(
                "the gap is a key of /etc/font",
                machine.wait_serial("line_gap=2", timeout),
                "the file of the font does not keep the gap",
            )

            machine.type_line("config set font.line_gap 0")
            expect(
                "the gap goes back to the whole cell",
                machine.wait_serial(
                    f"font: line_gap 0, the cell is 8x16 at {LINE_PITCH} px, "
                    f"{ROWS_GAP0} rows of {COLUMNS} columns",
                    timeout,
                ),
                "the console did not lay the screen out at 16 pixels a line again",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.only_colors(GREY, BLACK) and not seen.foreign_cells(GREY),
            )
            if picture is not None:
                expect(
                    "the picture of the whole cell is 50 rows of 16 pixels",
                    picture.only_colors(GREY, BLACK) and not picture.foreign_cells(GREY),
                    "the picture is not the grid of 16 pixels a line",
                )

            # --- the keyboard, and the picture it changes
            ink_before = machine.picture(workdir)
            type_fsck(machine)

            answered = machine.wait_serial(ANSWER, timeout)
            expect("the keyboard reaches the shell", answered, f"the serial console never said {ANSWER!r}")

            picture = machine.picture(workdir)
            expect(
                "the answer reaches the picture",
                picture is not None
                and ink_before is not None
                and picture.lowest_ink_row() > ink_before.lowest_ink_row(),
                "the screen did not grow below the prompt after the command",
            )

            # --- the colours of /etc/console (ADR 0021) on the new screen
            machine.type_line("config set console.theme light")
            themed = machine.wait_serial("console: theme light, invert 0", timeout)
            expect(
                "config set repaints the framebuffer",
                themed,
                "the console did not apply the theme of /etc",
            )

            picture = machine.picture(workdir)
            if picture is not None:
                expect(
                    "the light theme is black on light grey",
                    picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) == GREY
                    and picture.pixel(0, VBE_HEIGHT - 2) == GREY,
                    f"the paper is {picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2)}",
                )
                # The banner is the first line of the boot, and the boot of a
                # machine whose log is longer than the glass scrolls: the check
                # asks where the banner is instead of assuming the row 0, and a
                # line more in the log of a kernel is not a failure of the
                # screen.
                expect(
                    "the text of the boot is read in the new colours",
                    picture.cell_is(0, 0, GLYPHS["M"], BLACK, GREY)
                    or picture.cell_is(0, 0, GLYPHS["m"], BLACK, GREY)
                    or picture.row_of("MicroOS", BLACK, GREY) is not None,
                    "no row of the picture holds the banner in the colours of the theme",
                )
                foreign = picture.foreign_cells(BLACK)
                expect(
                    "the repaint left no pixel of the old pair",
                    picture.only_colors(BLACK, GREY) and not foreign,
                    f"{len(foreign)} cell(s) of the light picture hold no glyph: "
                    f"{foreign[:5]}",
                )
                if verbose:
                    print("--- the framebuffer after the theme ---")
                    print(f"{picture.width}x{picture.height}, {picture.ink_pixels()} pixel(s) of ink")

            # --- the colours of a command and of the log (K11c)
            #
            # `ls --color` paints a directory blue and an executable green
            # (the SGR sequences of a terminal), the log marks a warning amber
            # and an error red, and `ink`/`paper` of /etc/console name the
            # pair of the plain text. Every check below reads the colour of
            # the cells out of the picture and not the text of the serial
            # console, which has no sequences at all.
            expect(
                "the console goes back to the dark pair",
                machine.run_line("config set console.theme dark", timeout)
                and "console: theme dark, invert 0" in machine.read_serial(),
                "the console did not apply the dark theme again",
            )

            # The volume of this boot is the one the port formats itself:
            # /bin is made by the disk image of `make x86-smoke` and this
            # machine has none, so the executable of the test is a file of
            # /etc with the bit set and the listing is of that directory.
            machine.run_line("chmod 0755 /etc/setting", timeout)
            expect(
                "ls --color answers",
                machine.run_line("ls --color /etc", timeout),
                "the shell did not answer ls",
            )
            picture = machine.picture(workdir)
            expect(
                "ls --color writes an executable in green",
                picture is not None and picture.find_run("setting", GREEN, BLACK) is not None,
                "no run of the word 'setting' is the green of the palette on the black paper",
            )

            expect(
                "ls --color answers for a directory",
                machine.run_line("ls --color /", timeout),
                "the shell did not answer ls",
            )
            picture = machine.picture(workdir)
            expect(
                "ls --color writes a directory in blue",
                picture is not None and picture.find_run("etc", BLUE, BLACK) is not None,
                "no run of the word 'etc' is the blue of the palette on the black paper",
            )

            # A screen of the commands before this one is cleared, so the pair
            # of the console is the whole picture of a listing not coloured.
            machine.run_line("clear", timeout)
            expect(
                "ls --color=never answers",
                machine.run_line("ls --color=never /etc", timeout),
                "the shell did not answer ls",
            )
            picture = machine.picture(workdir)
            expect(
                "ls without --color writes the pair of the console",
                picture is not None and picture.only_colors(GREY, BLACK),
                "a pixel of the plain listing is outside the pair of the console",
            )

            # A value of /etc/console that is neither a name nor 0xRRRR is the
            # theme and a warning, which the log writes in amber.
            expect(
                "a broken ink is the theme and a warning",
                machine.run_line("config set console.ink nonsense", timeout)
                and "console: ink 'nonsense' is not a colour name" in machine.read_serial(),
                "the console did not warn about the value of the file",
            )
            picture = machine.picture(workdir)
            expect(
                "a warning of the log is amber",
                picture is not None and picture.find_run("console:", AMBER, BLACK) is not None,
                "no run of the word 'console:' is the amber of the palette",
            )

            # An error of a call: the filesystem names the device it does not
            # have, at the level of an error, and the log reads the line in
            # red. The mount point is made first, so that the sentence is of
            # the device and not of a path that is not there.
            machine.run_line("mkdir /tmp", timeout)
            expect(
                "a failure of a call is reported",
                machine.run_line("mount nosuchdev /tmp mfs", timeout)
                and "mfs: there is no block device 'nosuchdev'" in machine.read_serial(),
                "the filesystem did not name the device it does not have",
            )
            picture = machine.picture(workdir)
            expect(
                "an error of the log is red",
                picture is not None and picture.find_run("mfs:", RED, BLACK) is not None,
                "no run of the word 'mfs:' is the red of the palette",
            )

            # `ink` and `paper` of /etc/console: a number of RGB565 reaches
            # the picture as it was written, and the grid is painted again.
            expect(
                "the ink of the file is applied",
                machine.run_line("config set console.ink 0xF800", timeout),
            )
            picture = machine.picture(workdir)
            expect(
                "the ink of the file is the red of 0xF800",
                picture is not None
                and picture.find_run("console:", (255, 0, 0), BLACK) is not None,
                "the plain text is not the red of 0xF800",
            )

            expect(
                "the paper of the file is applied",
                machine.run_line("config set console.paper 0x001F", timeout),
            )
            picture = machine.picture(workdir)
            expect(
                "the paper of the file is the blue of 0x001F",
                picture is not None
                and picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) == (0, 0, 255),
                f"the paper is {picture.pixel(VBE_WIDTH - 2, VBE_HEIGHT - 2) if picture else None}",
            )
            expect(
                "the ink of the text is still the red of the file",
                picture is not None
                and picture.find_run("console:", (255, 0, 0), (0, 0, 255)) is not None,
                "the run of 'console:' is not the red ink on the blue paper",
            )

            # The keys go back to the theme for a machine that boots again.
            machine.run_line("config set console.ink theme", timeout)
            expect(
                "the pair of the theme comes back",
                machine.run_line("config set console.paper theme", timeout),
                "the console did not go back to the pair of the theme",
            )
        finally:
            machine.close()

    return checks


def run_ui(loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str) -> list:
    """The first interface of ADR 0028: /bin/ui on the screen and its keys.

    The machine boots the disk of `make x86-disk`, the shell starts the
    program through `exec`, and the test reads the three things the skeleton
    promises: the program takes the screen from the kernel (the ioctl), the
    panel and the window are not the console (the screenshot has the colours
    of the program), Tab moves the focus and `q` returns the machine to the
    shell, which repaints its own grid.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"ui: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-ui-") as workdir:
        machine = Machine(loader, kernel, workdir, "std", display, image)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the shell of the disk is up", booted, "the shell never printed its prompt")

            before = machine.picture(workdir)
            expect(
                "the machine has a console before the interface",
                before is not None and before.pixel(0, 0) == BLACK,
                "the screenshot before exec is not the console",
            )

            # The program is a file of /bin like every other program: the
            # shell starts it with the call and waits for it to end.
            count = machine.prompts()
            machine.type_line("exec /bin/ui")

            taken = machine.wait_serial("fb: the console is taken", timeout)
            expect(
                "the interface takes the screen",
                taken,
                "the kernel never said that /dev/fb0 was taken (FBIO_TAKE)",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(0, 0) == UI_PANEL_RGB
                and seen.find_run("tasks", UI_TEXT_RGB, UI_PANEL_RGB) is not None,
            )
            expect("the device has the picture of the interface", picture is not None,
                   "screendump wrote no PPM")

            if picture is not None:
                expect(
                    "the picture is still the mode of ADR 0023",
                    (picture.width, picture.height) == (VBE_WIDTH, VBE_HEIGHT),
                    f"the screenshot is {picture.width}x{picture.height}",
                )
                expect(
                    "the panel is one line of the program",
                    picture.pixel(0, 0) == UI_PANEL_RGB
                    and picture.pixel(VBE_WIDTH - 1, 0) == UI_PANEL_RGB
                    and picture.pixel(VBE_WIDTH - 1, UI_PANEL_HEIGHT - 1) == UI_PANEL_RGB,
                    f"the first pixels are {picture.pixel(0, 0)}",
                )
                expect(
                    "the panel is not the console",
                    not picture.only_colors(GREY, BLACK),
                    "the picture of the interface is made of the two colours of the console",
                )
                # The words of the panel are the glyphs of the port's font in
                # the white of the interface on the blue of the panel.
                expect(
                    "the panel shows the clock, the tasks and the keys",
                    picture.find_run("tasks", UI_TEXT_RGB, UI_PANEL_RGB) is not None
                    and picture.find_run("quit", UI_TEXT_RGB, UI_PANEL_RGB) is not None
                    and picture.find_run("up", UI_TEXT_RGB, UI_PANEL_RGB) is not None,
                    "no run of 'up', 'tasks' or 'quit' is on the panel",
                )
                # The list of windows on the left: the first row is the
                # focused one, the second is black.
                status = picture.pixel(UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + 2)
                console = picture.pixel(
                    UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + UI_ITEM_STEP + 2
                )
                expect(
                    "the list starts on the status window",
                    status == GREY and console == BLACK,
                    f"the two rows of the list are {status} and {console}",
                )
                # The console window is the black of the console with the
                # border of the interface around it.
                expect(
                    "the window of the log has the border of the interface",
                    picture.pixel(UI_CONSOLE_X - 2, UI_CONSOLE_Y - 2) != BLACK,
                    "the border of the console window is not drawn",
                )
                if verbose:
                    print("--- the picture of /bin/ui ---")
                    print(f"{picture.width}x{picture.height}, {picture.ink_pixels()} pixel(s) of ink")

            # Tab moves the focus: one rectangle of the list changes and the
            # panel and the window of the log stay where they were.
            machine.keys(["tab"])
            swapped = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(
                    UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + UI_ITEM_STEP + 2
                )
                == GREY,
                seconds=4.0,
            )
            expect(
                "Tab moves the focus to the console window",
                swapped is not None
                and swapped.pixel(UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + 2) == BLACK
                and swapped.pixel(UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + UI_ITEM_STEP + 2)
                == GREY,
                "the two rows of the list did not trade their colours",
            )
            expect(
                "the frame of the interface did not move",
                swapped is not None
                and swapped.pixel(0, 0) == UI_PANEL_RGB
                and swapped.pixel(UI_CONSOLE_X - 2, UI_CONSOLE_Y - 2) != BLACK,
                "a rectangle of the interface outside the list changed",
            )

            machine.keys(["tab"])
            back = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(UI_ITEM_X + UI_ITEM_WIDTH - 4, UI_ITEM_Y0 + 2) == GREY,
                seconds=4.0,
            )
            expect(
                "a second Tab comes back to the status window",
                back is not None,
                "the focus did not come back after the second Tab",
            )

            # q leaves: the program gives the screen back, the shell prints
            # its prompt and the console is the machine again.
            machine.keys(["q"])

            given = machine.wait_serial("fb: the console is given back", timeout)
            expect(
                "the interface gives the screen back",
                given,
                "the kernel never said that /dev/fb0 was given back (FBIO_GIVE)",
            )
            expect(
                "the shell waits for the interface and returns",
                machine.wait_serial("the program has finished", timeout),
                "the shell did not name the end of the program",
            )
            expect(
                "the prompt is back on the serial console",
                _wait_prompts(machine, count + 1, timeout),
                "the shell did not print its prompt after the interface",
            )

            shell = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(0, 0) != UI_PANEL_RGB,
                seconds=6.0,
            )
            expect(
                "the console is the machine again",
                shell is not None
                and shell.pixel(0, 0) == BLACK
                and shell.only_colors(GREY, BLACK),
                "the picture after q is not the console of the kernel",
            )
            expect(
                "the prompt of the shell is on the picture",
                shell is not None and shell.find_run("microos>", GREY, BLACK) is not None,
                "no run of 'microos>' is the grey of the console on the black paper",
            )
            expect(
                "the keys of the interface did not reach the shell",
                "unknown command" not in machine.read_serial(),
                "a key of the interface became a line of the shell",
            )

            if verbose and shell is not None:
                print("--- the picture of the console after q ---")
                print(f"{shell.width}x{shell.height}, {shell.ink_pixels()} pixel(s) of ink")
        finally:
            machine.close()

    return checks


def run_editor(loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str) -> list:
    """The editor of ADR 0030: a file, the arrows and Ctrl+S, on the screen.

    The machine boots a copy of the disk of `make x86-disk`, the shell starts
    `/bin/edit` on a file that is not there yet, the test types two lines
    through the keyboard of QEMU (IRQ1 and the E0 sequences of Enter and of
    the arrows), saves with Ctrl+S, leaves with Ctrl+Q, and then boots the
    same disk a second time: `cat /edit.txt` has to show the two lines. The
    file is the proof that the editor wrote what the keys said, and the
    screenshot is the proof that it showed the same thing.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"edit: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-edit-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(image, disk)
        machine = Machine(loader, kernel, workdir, "std", display, disk)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the shell of the disk is up", booted, "the shell never printed its prompt")

            count = machine.prompts()
            machine.type_line("exec /bin/edit /edit.txt")

            taken = machine.wait_serial("fb: the console is taken", timeout)
            expect(
                "the editor takes the screen",
                taken,
                "the kernel never said that /dev/fb0 was taken (FBIO_TAKE)",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(
                    UI_FRAME_PAD, UI_TITLE_Y, "edit - /edit.txt", EDIT_TITLE_INK_RGB, EDIT_BAR_RGB
                )
                and seen.text_at(
                    UI_FRAME_PAD, UI_STATUS_Y, "edit  L1 C1  0 bytes", EDIT_DIM_INK_RGB,
                    EDIT_BAR_RGB
                )
                and seen.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT) == EDIT_INK_RGB,
            )
            expect("the window of the editor is drawn", picture is not None,
                   "screendump wrote no PPM")

            if picture is not None:
                expect(
                    "the title says which file is open",
                    picture.text_at(UI_FRAME_PAD, UI_TITLE_Y, "edit - /edit.txt",
                                    EDIT_TITLE_INK_RGB, EDIT_BAR_RGB),
                    "the title bar does not hold 'edit - /edit.txt'",
                )
                expect(
                    "the status line names the keys",
                    picture.text_at(
                        UI_FRAME_PAD, UI_STATUS_Y, "edit  L1 C1  0 bytes", EDIT_DIM_INK_RGB,
                        EDIT_BAR_RGB
                    ),
                    "the status line is not 'edit  L1 C1  0 bytes'",
                )
                expect(
                    "the cursor is one inverted cell of the first line",
                    picture.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT) == EDIT_INK_RGB
                    and picture.pixel(UI_FRAME_PAD + 7,
                                      UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT - 1) == EDIT_INK_RGB
                    and picture.pixel(UI_FRAME_PAD + 8, UI_FRAME_TITLE_HEIGHT) == EDIT_PAPER_RGB,
                    "the first cell of the body is not the block of the cursor",
                )

            # The keys of the keyboard, not the serial port: two letters, the
            # Enter of the main block and two more.
            machine.keys(["h", "i"])
            typed = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(
                    UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT, "hi", EDIT_INK_RGB, EDIT_PAPER_RGB
                )
                and seen.pixel(UI_FRAME_PAD + 16, UI_FRAME_TITLE_HEIGHT) == EDIT_INK_RGB
                and seen.text_at(UI_FRAME_PAD, UI_STATUS_Y, "edit [modified]", EDIT_DIM_INK_RGB,
                                 EDIT_BAR_RGB),
            )
            expect(
                "the letters of the keyboard are in the file",
                typed is not None
                and typed.text_at(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT, "hi", EDIT_INK_RGB,
                                  EDIT_PAPER_RGB),
                "the first line of the body is not 'hi'",
            )
            expect(
                "the cursor moved after the letters",
                typed is not None
                and typed.pixel(UI_FRAME_PAD + 16, UI_FRAME_TITLE_HEIGHT) == EDIT_INK_RGB,
                "the cursor is not the cell after 'hi'",
            )
            expect(
                "the status line says the file has changed",
                typed is not None
                and typed.text_at(
                    UI_FRAME_PAD, UI_STATUS_Y, "edit [modified]", EDIT_DIM_INK_RGB, EDIT_BAR_RGB
                ),
                "the status line does not say [modified]",
            )

            machine.keys(["ret", "o", "k"])
            second = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(
                    UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT, "ok", EDIT_INK_RGB,
                    EDIT_PAPER_RGB
                ),
            )
            expect(
                "Enter starts a second line",
                second is not None
                and second.text_at(
                    UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT, "ok", EDIT_INK_RGB,
                    EDIT_PAPER_RGB
                ),
                "the second line of the body is not 'ok'",
            )

            # Ctrl+S writes the file and says so.
            machine.keys(["ctrl-s"])
            saved = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(
                    UI_FRAME_PAD, UI_STATUS_Y, "saved 5 bytes", EDIT_DIM_INK_RGB, EDIT_BAR_RGB
                )
                and seen.text_at(UI_FRAME_PAD, UI_TITLE_Y, "edit - /edit.txt",
                                 EDIT_TITLE_INK_RGB, EDIT_BAR_RGB),
            )
            expect(
                "Ctrl+S writes the file",
                saved is not None,
                "the status line never said 'saved 5 bytes'",
            )
            expect(
                "a saved file has no mark of a change",
                saved is not None
                and saved.text_at(UI_FRAME_PAD, UI_TITLE_Y, "edit - /edit.txt",
                                  EDIT_TITLE_INK_RGB, EDIT_BAR_RGB),
                "the title still marks the file as modified",
            )

            # Ctrl+Q leaves, the screen is the console again.
            machine.keys(["ctrl-q"])
            given = machine.wait_serial("fb: the console is given back", timeout)
            expect("Ctrl+Q gives the screen back", given,
                   "the kernel never said that /dev/fb0 was given back (FBIO_GIVE)")
            expect(
                "the shell waits for the editor and returns",
                machine.wait_serial("the program has finished", timeout),
                "the shell did not name the end of the program",
            )
            expect(
                "the prompt is back after the editor",
                _wait_prompts(machine, count + 1, timeout),
                "the shell did not print its prompt after the editor",
            )

            shell = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(0, 0) == BLACK,
                seconds=6.0,
            )
            expect(
                "the console is the machine again",
                shell is not None and shell.only_colors(GREY, BLACK),
                "the picture after Ctrl+Q is not the console of the kernel",
            )
        finally:
            machine.close()

        # The same disk, a second boot: the file the editor wrote is there.
        seconds_dir = os.path.join(workdir, "second")
        os.makedirs(seconds_dir, exist_ok=True)
        machine = Machine(loader, kernel, seconds_dir, "std", display, disk)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the disk of the editor boots again", booted,
                   "the shell never printed its prompt on the second boot")

            count = machine.prompts()
            expect(
                "cat reads the two lines of the editor",
                machine.run_line("cat /edit.txt", timeout),
                "the shell did not answer cat",
            )
            expect(
                "the file the editor saved is on the disk",
                "hi\r\nok" in machine.read_serial(),
                "cat did not print 'hi' and 'ok'",
            )
            expect(
                "the prompt is back after cat",
                _wait_prompts(machine, count + 1, timeout),
                "the shell did not print its prompt after cat",
            )
        finally:
            machine.close()

    return checks


def run_fm(loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str) -> list:
    """The file manager of ADR 0030: a directory, the arrows, a delete.

    The machine boots a copy of the disk, the shell makes a file in the root
    (`echo gone > /zzz`), and `/bin/fm /` lists it. The test reads the title,
    the names and the selected row out of the screenshot, moves the selection
    with the arrows and End, opens the modal window of `d` (the rectangle of
    the dialog and the sentence that names the file), answers `n` once and
    `y` once, and then reads the answer of the call in the status line: the
    file is gone from the list and the line says it was deleted.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"fm: {name}", bool(condition), detail))

    def row_holding(picture, letters: str) -> int:
        """The row of the list that spells `letters`, -1 when there is none."""
        for row in range((UI_STATUS_Y - UI_FRAME_TITLE_HEIGHT) // CELL_HEIGHT):
            if picture.text_at(
                UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + row * CELL_HEIGHT, letters, FM_INK_RGB,
                FM_PAPER_RGB
            ):
                return row
        return -1

    with tempfile.TemporaryDirectory(prefix="microos-x86-fm-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(image, disk)
        machine = Machine(loader, kernel, workdir, "std", display, disk)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the shell of the disk is up", booted, "the shell never printed its prompt")
            expect(
                "the fixture of the delete is written",
                machine.run_line("echo gone > /zzz", timeout),
                "the shell did not write /zzz",
            )

            count = machine.prompts()
            machine.type_line("exec /bin/fm /")

            taken = machine.wait_serial("fb: the console is taken", timeout)
            expect(
                "the file manager takes the screen",
                taken,
                "the kernel never said that /dev/fb0 was taken (FBIO_TAKE)",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(UI_FRAME_PAD, UI_TITLE_Y, "fm - /",
                                          FM_TITLE_INK_RGB, FM_BAR_RGB)
                and row_holding(seen, "etc/") >= 0
                and row_holding(seen, "proc/") >= 0
                and row_holding(seen, "zzz") >= 0,
            )
            expect("the window of the file manager is drawn", picture is not None,
                   "screendump wrote no PPM")

            zzz_row = -1

            if picture is not None:
                expect(
                    "the title names the directory",
                    picture.text_at(UI_FRAME_PAD, UI_TITLE_Y, "fm - /", FM_TITLE_INK_RGB,
                                    FM_BAR_RGB),
                    "the title bar does not hold 'fm - /'",
                )
                expect(
                    "the list of the root holds the directories",
                    row_holding(picture, "etc/") >= 0
                    and row_holding(picture, "proc/") >= 0
                    and picture.text_at(
                        UI_FRAME_PAD,
                        UI_FRAME_TITLE_HEIGHT,
                        "bin/",
                        FM_SELECT_INK_RGB,
                        FM_SELECT_RGB,
                    ),
                    "the list does not hold 'etc/', 'proc/' and the selected 'bin/'",
                )
                expect(
                    "the list holds the file of the fixture",
                    row_holding(picture, "zzz") >= 0,
                    "the list does not hold 'zzz'",
                )
                expect(
                    "the status line names the keys",
                    "Enter open"
                    in picture.letters_at(UI_FRAME_PAD, UI_STATUS_Y, 100, FM_DIM_INK_RGB,
                                          FM_BAR_RGB)
                    and "q quit"
                    in picture.letters_at(UI_FRAME_PAD, UI_STATUS_Y, 100, FM_DIM_INK_RGB,
                                          FM_BAR_RGB),
                    "the status line does not name the keys",
                )
                expect(
                    "the first row of the list has the focus",
                    picture.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT) == FM_SELECT_RGB
                    and picture.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT)
                    == FM_PAPER_RGB,
                    "the first row is not the grey of the focus",
                )
                zzz_row = row_holding(picture, "zzz")

            # The arrows move the selection: the first row is the paper and
            # the second is the grey, and no other row of the picture moved.
            machine.keys(["down"])
            moved = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT) == FM_PAPER_RGB
                and seen.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT)
                == FM_SELECT_RGB,
            )
            expect(
                "the down arrow moves the selection",
                moved is not None
                and moved.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT) == FM_PAPER_RGB
                and moved.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + CELL_HEIGHT)
                == FM_SELECT_RGB,
                "the grey of the selection did not move to the second row",
            )

            # End: the last name of the list, which is the fixture.
            machine.keys(["end"])
            last = settled_picture(
                machine,
                workdir,
                lambda seen: zzz_row >= 0
                and seen.pixel(UI_FRAME_PAD, UI_FRAME_TITLE_HEIGHT + zzz_row * CELL_HEIGHT)
                == FM_SELECT_RGB,
            )
            expect(
                "End selects the last name of the list",
                last is not None and zzz_row >= 0,
                "the last row is not the grey of the focus",
            )

            # d opens the modal window: its border is the cyan of the focus
            # over the paper of the list, and it names the file.
            machine.keys(["d"])
            dialog = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(FM_MODAL_X, FM_MODAL_Y) == FM_FOCUS_RGB
                and seen.text_at(
                    FM_MODAL_X + UI_FRAME_PAD,
                    FM_MODAL_Y + UI_FRAME_TITLE_HEIGHT,
                    "Delete zzz?",
                    FM_INK_RGB,
                    FM_PAPER_RGB,
                ),
            )
            expect(
                "d opens a modal window",
                dialog is not None,
                "no rectangle of the dialog is on the screen",
            )
            expect(
                "the dialog asks about the file",
                dialog is not None
                and dialog.text_at(
                    FM_MODAL_X + UI_FRAME_PAD,
                    FM_MODAL_Y + UI_FRAME_TITLE_HEIGHT,
                    "Delete zzz?",
                    FM_INK_RGB,
                    FM_PAPER_RGB,
                ),
                "the dialog does not ask 'Delete zzz?'",
            )

            # n closes it and the file stays.
            machine.keys(["n"])
            cancelled = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(FM_MODAL_X, FM_MODAL_Y) != FM_FOCUS_RGB
                and zzz_row >= 0
                and seen.text_at(
                    UI_FRAME_PAD,
                    UI_FRAME_TITLE_HEIGHT + zzz_row * CELL_HEIGHT,
                    "zzz",
                    FM_SELECT_INK_RGB,
                    FM_SELECT_RGB,
                ),
            )
            expect(
                "n closes the dialog and keeps the file",
                cancelled is not None
                and cancelled.pixel(FM_MODAL_X, FM_MODAL_Y) != FM_FOCUS_RGB
                and zzz_row >= 0
                and cancelled.text_at(
                    UI_FRAME_PAD,
                    UI_FRAME_TITLE_HEIGHT + zzz_row * CELL_HEIGHT,
                    "zzz",
                    FM_SELECT_INK_RGB,
                    FM_SELECT_RGB,
                ),
                "the dialog stayed or the name left the list",
            )

            # d and y: the call removes the file and the status line says so.
            machine.keys(["d"])
            settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(FM_MODAL_X, FM_MODAL_Y) == FM_FOCUS_RGB,
            )
            machine.keys(["y"])
            deleted = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(
                    UI_FRAME_PAD, UI_STATUS_Y, "deleted /zzz", FM_DIM_INK_RGB, FM_BAR_RGB
                )
                and row_holding(seen, "bin/") >= 0
                and row_holding(seen, "proc/") >= 0,
            )
            expect(
                "y deletes the file",
                deleted is not None,
                "the status line never said 'deleted /zzz'",
            )
            expect(
                "the name leaves the list",
                deleted is not None and row_holding(deleted, "zzz") < 0,
                "the list still holds 'zzz' after the delete",
            )

            # q returns to the shell.
            machine.keys(["q"])
            expect(
                "q gives the screen back",
                machine.wait_serial("fb: the console is given back", timeout),
                "the kernel never said that /dev/fb0 was given back (FBIO_GIVE)",
            )
            expect(
                "the shell waits for the file manager and returns",
                machine.wait_serial("the program has finished", timeout),
                "the shell did not name the end of the program",
            )
            expect(
                "the prompt is back after the file manager",
                _wait_prompts(machine, count + 1, timeout),
                "the shell did not print its prompt after the file manager",
            )
            expect(
                "the keys of the file manager did not reach the shell",
                "unknown command" not in machine.read_serial(),
                "a key of the file manager became a line of the shell",
            )

            shell = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(0, 0) == BLACK,
                seconds=6.0,
            )
            expect(
                "the console is the machine again",
                shell is not None and shell.only_colors(GREY, BLACK),
                "the picture after q is not the console of the kernel",
            )
        finally:
            machine.close()

    return checks


def _wait_prompts(machine: Machine, wanted: int, timeout: float) -> bool:
    """The prompt count of the shell, once it reaches `wanted`."""
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if machine.prompts() >= wanted:
            return True
        time.sleep(0.05)

    return False


def stray_pixels(picture, x0: int, y0: int, x1: int, y1: int, paper: tuple) -> int:
    """How many pixels of a rectangle are not the paper the caller expects.

    A bar of a title is one colour with the ink of its text on it; a pixel of
    any other colour in the part of the bar past the title is a fragment of a
    frame that was painted and not put right.
    """
    count = 0

    for y in range(y0, y1):
        for x in range(x0, x1):
            if picture.pixel(x, y) != paper:
                count += 1

    return count


def run_desktop(
    loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str
) -> list:
    """The desktop of 1.7: `/bin/wm`, two clients and the pointer.

    The machine boots a copy of the disk and the shell starts `/bin/wm`, which
    takes the screen, starts `/bin/edit` on `/doc.txt` and `/bin/fm` on `/`
    with `fork` and `execve`, and owns the picture from then on. The test
    reads the palette out of the screenshot - the gradient of the wallpaper,
    the air and the separator of a frame, the shadow of a window, the accent
    of the focus - and then the panel and the two windows; it drives the
    pointer through the PS/2 mouse of QEMU (`mouse_move`, `mouse_button`) and
    the keys through `sendkey`, and reads the lines of the server: a click
    raises the window it lands on, a key goes to the window with the keys, a
    drag of a title bar moves the window, Alt+F4 closes it and the focus goes
    back to what is left. The last window ends the desktop, the grid of the
    shell comes back and `cat /doc.txt` reads the line the editor typed -
    which is the proof that the buffer of a client, the keys the server
    forwarded and the file of the program are one story. The desktop starts a
    second time with `theme=light` of /etc/console and the same checks of the
    desk are read in the light palette.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"desktop: {name}", bool(condition), detail))

    def goto(machine, x: int, y: int) -> None:
        """The pointer to a known point: the corner first, then the difference.

        The mouse of the machine measures a difference and not a position, so
        a test that wants to click a pixel of a window starts from a corner it
        cannot miss: a move of more than the picture is the corner, whatever
        the pointer was before.
        """
        for _ in range(12):
            machine.monitor_command("mouse_move -200 -200")
            time.sleep(0.02)

        while x or y:
            step_x = max(-200, min(200, x))
            step_y = max(-200, min(200, y))
            machine.monitor_command(f"mouse_move {step_x} {step_y}")
            x -= step_x
            y -= step_y
            time.sleep(0.04)

        time.sleep(0.15)

    def wait_new(machine, needle: str, after: int, timeout: float) -> bool:
        """Wait for the (after+1)-th line of the transcript.

        The transcript is one stream and the second desktop of the run prints
        the same words as the first one: a search of the whole transcript
        answers with the line of the first desktop and the check passes before
        the second one has said a word. The count taken before the command is
        the mark, as it is for the prompt of the shell.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if machine.read_serial().count(needle) > after:
                return True
            time.sleep(0.05)
        return False

    def click(machine) -> None:
        machine.monitor_command("mouse_button 1")
        time.sleep(0.15)
        machine.monitor_command("mouse_button 0")
        time.sleep(0.30)

    def title_at(picture, rect) -> bool:
        x, y, _, _ = rect
        return picture.text_at(x + UI_FRAME_PAD, y + UI_TITLE_Y, "edit - /doc.txt",
                               EDIT_TITLE_INK_RGB, EDIT_BAR_RGB) or picture.text_at(
                                   x + UI_FRAME_PAD, y + UI_TITLE_Y, "edit - /doc.txt *",
                                   EDIT_TITLE_INK_RGB, EDIT_BAR_RGB)

    with tempfile.TemporaryDirectory(prefix="microos-x86-desktop-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(image, disk)
        machine = Machine(loader, kernel, workdir, "std", display, disk)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the shell of the disk is up", booted, "the shell never printed its prompt")

            count = machine.prompts()
            machine.type_line("exec /bin/wm")

            expect(
                "the server takes the screen",
                machine.wait_serial("fb: the console is taken", timeout),
                "the kernel never said that /dev/fb0 was taken (FBIO_TAKE)",
            )
            expect(
                "the two windows of the desktop are started",
                machine.wait_serial("wm: the desktop is up", timeout),
                "the server never said that its windows were up",
            )
            transcript = machine.read_serial()
            expect(
                "the editor is a window of the server",
                "wm: window edit - /doc.txt" in transcript,
                "the server did not start /bin/edit on /doc.txt",
            )
            expect(
                "the file manager is a window of the server",
                "wm: window fm - /" in transcript,
                "the server did not start /bin/fm on /",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(4, 4) == WALL_TOP_RGB
                and title_at(seen, WM_EDIT_RECT)
                and seen.text_at(WM_FM_RECT[0] + UI_FRAME_PAD, WM_FM_RECT[1] + UI_TITLE_Y,
                                 "fm - /", FM_TITLE_INK_RGB, FM_BAR_RGB),
                seconds=10.0,
            )
            expect("the picture of the desktop is drawn", picture is not None,
                   "screendump wrote no PPM")

            if picture is not None:
                expect(
                    "the wallpaper is a gradient of the theme",
                    picture.pixel(4, 4) == WALL_TOP_RGB
                    and picture.pixel(4, 770) == WALL_BOTTOM_RGB
                    and picture.pixel(4, 4) != picture.pixel(4, 770),
                    f"the desk is {picture.pixel(4, 4)} at the top and "
                    f"{picture.pixel(4, 770)} at the bottom, not the gradient of the palette",
                )
                expect(
                    "the title bar of the editor is the text of the client",
                    title_at(picture, WM_EDIT_RECT),
                    "the title of the window of the editor is not 'edit - /doc.txt'",
                )
                expect(
                    "the title and the body of a window are two inks",
                    EDIT_TITLE_INK_RGB != EDIT_INK_RGB
                    and brightness(EDIT_TITLE_INK_RGB) > brightness(EDIT_INK_RGB),
                    "the title of a window is not brighter than the text of its body",
                )
                expect(
                    "the title has air on either side of its text",
                    all(
                        picture.pixel(x, WM_EDIT_RECT[1] + UI_TITLE_Y + 8) == EDIT_BAR_RGB
                        for x in range(WM_EDIT_RECT[0] + 1, WM_EDIT_RECT[0] + UI_FRAME_PAD)
                    ),
                    "a pixel of the title touches the border of the window",
                )
                expect(
                    "the separator divides the title from the body",
                    picture.pixel(WM_EDIT_RECT[0] + 3,
                                  WM_EDIT_RECT[1] + UI_FRAME_TITLE_HEIGHT - 1) == BORDER_RGB,
                    "no line of the border colour under the title of the editor",
                )
                expect(
                    "the editor draws its body",
                    picture.pixel(WM_EDIT_RECT[0] + 20, WM_EDIT_RECT[1] + 60) == EDIT_PAPER_RGB,
                    "the body of the editor is not its paper",
                )
                expect(
                    "the body has air on either side of its text",
                    picture.pixel(WM_EDIT_RECT[0] + 2, WM_EDIT_RECT[1] + 40) == EDIT_PAPER_RGB
                    and picture.pixel(WM_EDIT_RECT[0] + 4,
                                      WM_EDIT_RECT[1] + UI_FRAME_TITLE_HEIGHT + 8)
                    == EDIT_PAPER_RGB,
                    "a pixel of the body text touches the border of the window",
                )
                expect(
                    "the title bar of the file manager is the text of the client",
                    picture.text_at(WM_FM_RECT[0] + UI_FRAME_PAD, WM_FM_RECT[1] + UI_TITLE_Y,
                                    "fm - /", FM_TITLE_INK_RGB, FM_BAR_RGB),
                    "the title of the window of the file manager is not 'fm - /'",
                )
                expect(
                    "the list of the file manager is in the window",
                    picture.text_at(WM_FM_RECT[0] + UI_FRAME_PAD,
                                    WM_FM_RECT[1] + UI_FRAME_TITLE_HEIGHT,
                                    "bin/", FM_SELECT_INK_RGB, FM_SELECT_RGB),
                    "the first row of the root is not the selected 'bin/'",
                )
                expect(
                    "the panel is a bar of the bottom of the picture",
                    picture.pixel(4, VBE_HEIGHT - 4) == WM_PANEL_RGB,
                    "the bottom of the picture is not the surface of the panel",
                )
                expect(
                    "a line separates the panel from the desk",
                    picture.pixel(4, VBE_HEIGHT - WM_PANEL_HEIGHT) == BORDER_RGB,
                    "the top row of the panel is not the border of the palette",
                )
                expect(
                    "the panel holds a button a window",
                    picture.text_at(
                        14, VBE_HEIGHT - WM_PANEL_HEIGHT + 4, "edit - /doc.txt",
                        WM_PANEL_INK_RGB, WM_PANEL_BUTTON_RGB
                    ),
                    "the panel does not name the editor",
                )
                expect(
                    "the button of the window with the keys is the accent",
                    picture.text_at(
                        176, VBE_HEIGHT - WM_PANEL_HEIGHT + 4, "fm - /",
                        WM_PANEL_ACCENT_INK_RGB, WM_PANEL_FOCUS_RGB
                    ),
                    "the button of the file manager is not the accent of the theme",
                )
                expect(
                    "the server draws the pointer",
                    picture.pixel(VBE_WIDTH // 2, VBE_HEIGHT // 2) == WM_CURSOR_RGB,
                    "the middle of the picture is not the ink of the pointer",
                )
                expect(
                    "the pointer has a black outline",
                    picture.pixel(VBE_WIDTH // 2 - 1, VBE_HEIGHT // 2 + 2) == WM_CURSOR_EDGE_RGB
                    and picture.pixel(VBE_WIDTH // 2, VBE_HEIGHT // 2) != WM_CURSOR_EDGE_RGB,
                    "the pointer has no outline of the edge colour",
                )
                expect(
                    "the shadow of a window is darker than the desk",
                    picture.pixel(WM_EDIT_RECT[0] + 20,
                                  WM_EDIT_RECT[1] + WM_EDIT_RECT[3] + 1)
                    != picture.pixel(1250, WM_EDIT_RECT[1] + WM_EDIT_RECT[3] + 1)
                    and brightness(picture.pixel(WM_EDIT_RECT[0] + 20,
                                                 WM_EDIT_RECT[1] + WM_EDIT_RECT[3] + 1))
                    < brightness(picture.pixel(1250, WM_EDIT_RECT[1] + WM_EDIT_RECT[3] + 1)),
                    "the pixels under the editor are the wallpaper and not its shadow",
                )
                expect(
                    "the window of the file manager has the keys",
                    picture.pixel(WM_FM_RECT[0] - 1, WM_FM_RECT[1] - 1) == WM_FOCUS_RGB,
                    "the ring of the focus is not around the file manager",
                )
                expect(
                    "the accent of the focus is not the yellow of the CGA",
                    WM_FOCUS_RGB != rgb565(0xFFEA),
                    "the ring of the focus is the yellow of the terminal again",
                )
                expect(
                    "the window under it does not",
                    picture.pixel(WM_EDIT_RECT[0] - 2, WM_EDIT_RECT[1] - 2) == WALL_TOP_RGB,
                    "a ring of the focus is around the editor as well",
                )

            # --- a click raises the window it lands on ---------------------
            goto(machine, 200, 200)
            click(machine)
            expect(
                "a click on the window under it raises the editor",
                machine.wait_serial("wm: raise edit", timeout),
                "the server did not say that the editor came to the top",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(WM_EDIT_RECT[0] - 1, WM_EDIT_RECT[1] - 1) == WM_FOCUS_RGB,
            )
            expect(
                "the ring of the focus follows the raise",
                picture is not None
                and picture.pixel(WM_EDIT_RECT[0] - 1, WM_EDIT_RECT[1] - 1) == WM_FOCUS_RGB,
                "the ring of the focus is not around the editor",
            )

            # The window that does not have the keys is a window a person
            # reads: a raise is a change of the order and not a blank window.
            expect(
                "the window without the keys keeps its title",
                picture is not None
                and picture.text_at(WM_FM_RECT[0] + UI_FRAME_PAD,
                                    WM_FM_RECT[1] + UI_TITLE_Y, "fm - /", FM_TITLE_INK_RGB,
                                    FM_BAR_RGB),
                "the title of the file manager went away with the keys",
            )
            expect(
                "the window without the keys keeps its text",
                picture is not None
                and picture.text_at(WM_FM_RECT[0] + UI_FRAME_PAD,
                                    WM_FM_RECT[1] + UI_FRAME_TITLE_HEIGHT,
                                    "bin/", FM_SELECT_INK_RGB, FM_SELECT_RGB),
                "the list of the file manager went away with the keys",
            )

            # --- a key goes to the window with the keys --------------------
            goto(machine, 120, 160)
            click(machine)
            machine.keys(["h", "e", "l", "l", "o"])

            body = (WM_EDIT_RECT[0] + UI_FRAME_PAD, WM_EDIT_RECT[1] + UI_FRAME_TITLE_HEIGHT)
            status = (WM_EDIT_RECT[0] + UI_FRAME_PAD,
                      WM_EDIT_RECT[1] + WM_EDIT_RECT[3] - CELL_HEIGHT)

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(*body, "hello", EDIT_INK_RGB, EDIT_PAPER_RGB),
            )
            expect(
                "the keys go to the window with the focus",
                picture is not None
                and picture.text_at(*body, "hello", EDIT_INK_RGB, EDIT_PAPER_RGB),
                "the editor did not draw the keys the server forwarded",
            )
            expect(
                "the status line of the editor is the modified document",
                picture is not None
                and picture.text_at(*status, "edit [modified]", EDIT_DIM_INK_RGB, EDIT_BAR_RGB),
                "the status line does not say that the document changed",
            )

            # The file is written by the client and the line of the answer is
            # the status line of its window: the desktop has no console.
            machine.keys(["ctrl-s"])

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(*status, "saved 5 bytes", EDIT_DIM_INK_RGB,
                                          EDIT_BAR_RGB),
            )
            expect(
                "the editor saves the document with Ctrl+S",
                picture is not None
                and picture.text_at(*status, "saved 5 bytes", EDIT_DIM_INK_RGB, EDIT_BAR_RGB),
                "the status line of the editor does not say that it saved the file",
            )

            # --- the bar of the title is not a trail of the pointer --------
            #
            # Every place the pointer leaves is painted again, and the part of
            # the bar that is past the title has to be the paper of the bar: a
            # row of fragments there was the strip of the buffer read at the
            # column of the damage instead of the column of the row.
            goto(machine, 600, WM_EDIT_RECT[1] + 10)
            for _ in range(5):
                machine.monitor_command("mouse_move -100 0")
                time.sleep(0.15)
            goto(machine, 640, 600)

            picture = settled_picture(machine, workdir, lambda seen: True)
            expect(
                "the pointer leaves no fragment in the bar of the title",
                picture is not None
                and not stray_pixels(
                    picture,
                    WM_EDIT_RECT[0] + 184,
                    WM_EDIT_RECT[1] + UI_TITLE_Y,
                    WM_EDIT_RECT[0] + WM_EDIT_RECT[2] - 24,
                    WM_EDIT_RECT[1] + UI_TITLE_Y + CELL_HEIGHT,
                    EDIT_BAR_RGB,
                ),
                "the bar of the title of the editor holds pixels that are not its paper",
            )

            # --- the modal of the unsaved file leaves no fragment ----------
            #
            # The dialog is a window of the editor in the middle of its own
            # rectangle: 44 cells wide, the frame and three lines.
            modal = (
                WM_EDIT_RECT[0] + (WM_EDIT_RECT[2] - 44 * CELL_WIDTH) // 2,
                WM_EDIT_RECT[1] + (WM_EDIT_RECT[3] - (UI_FRAME_TITLE_HEIGHT + 3 * CELL_HEIGHT)) // 2,
            )
            machine.keys(["x"])
            machine.keys(["ctrl-q"])
            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(modal[0] + UI_FRAME_PAD, modal[1] + UI_TITLE_Y,
                                          "unsaved changes", EDIT_TITLE_INK_RGB, EDIT_BAR_RGB),
            )
            expect(
                "Ctrl+Q asks about the unsaved file in a modal window",
                picture is not None
                and picture.text_at(modal[0] + UI_FRAME_PAD, modal[1] + UI_TITLE_Y,
                                    "unsaved changes", EDIT_TITLE_INK_RGB, EDIT_BAR_RGB),
                "the question about the unsaved file is not on the picture",
            )

            machine.keys(["n"])
            time.sleep(0.5)
            goto(machine, 640, 600)
            picture = settled_picture(
                machine,
                workdir,
                lambda seen: not stray_pixels(
                    seen,
                    WM_EDIT_RECT[0] + 184,
                    WM_EDIT_RECT[1] + UI_TITLE_Y,
                    WM_EDIT_RECT[0] + WM_EDIT_RECT[2] - 24,
                    WM_EDIT_RECT[1] + UI_TITLE_Y + CELL_HEIGHT,
                    EDIT_BAR_RGB,
                ),
            )
            expect(
                "the modal leaves no fragment in the bar of the title",
                picture is not None
                and not stray_pixels(
                    picture,
                    WM_EDIT_RECT[0] + 184,
                    WM_EDIT_RECT[1] + UI_TITLE_Y,
                    WM_EDIT_RECT[0] + WM_EDIT_RECT[2] - 24,
                    WM_EDIT_RECT[1] + UI_TITLE_Y + CELL_HEIGHT,
                    EDIT_BAR_RGB,
                ),
                "the bar of the title of the editor holds pixels that are not its paper",
            )
            expect(
                "the answer of the modal keeps the document",
                picture is not None
                and picture.text_at(*body, "hello", EDIT_INK_RGB, EDIT_PAPER_RGB),
                "the text of the document went away with the modal",
            )

            # The document is saved again, so the title of the window is the
            # name of the file and the drags below have one string to match.
            machine.keys(["ctrl-s"])
            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.text_at(*status, "saved 6 bytes", EDIT_DIM_INK_RGB,
                                          EDIT_BAR_RGB),
            )
            expect(
                "the editor saves the document again",
                picture is not None
                and picture.text_at(*status, "saved 6 bytes", EDIT_DIM_INK_RGB, EDIT_BAR_RGB),
                "the status line of the editor does not say that it saved six bytes",
            )

            # --- a drag of the title bar moves the window ------------------
            goto(machine, 300, WM_EDIT_RECT[1] + 10)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            for _ in range(2):
                machine.monitor_command("mouse_move 50 25")
                time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.4)

            expect(
                "a drag of the title bar moves the window",
                machine.wait_serial("wm: move edit - /doc.txt to 140,90", timeout),
                "the server did not say that the editor moved to 140,90",
            )

            moved = (140, 90, WM_EDIT_RECT[2], WM_EDIT_RECT[3])
            picture = settled_picture(machine, workdir, lambda seen: title_at(seen, moved))
            expect(
                "the window is drawn at its new rectangle",
                picture is not None and title_at(picture, moved),
                "the title of the editor is not at 140,90",
            )
            expect(
                "the place it left is the wallpaper again",
                picture is not None
                and desk_at(picture, WM_EDIT_RECT[0] + 4, WM_EDIT_RECT[1] + 2),
                "the old rectangle of the editor is still painted",
            )

            # --- a window dragged to an edge keeps its head ----------------
            #
            # The bar of the title is the handle of a window: a window pulled
            # past the top-left corner loses the part of itself that is outside
            # the picture and keeps the bar, so a person can drag it back. The
            # window is at 140,90 now and the grab is near the right end of its
            # bar, so the pointer can pull it past the corner.
            goto(machine, 740, 100)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            for _ in range(5):
                machine.monitor_command("mouse_move -200 -200")
                time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.5)

            expect(
                "a window dragged past the corner keeps its bar on the picture",
                machine.wait_serial(f"wm: move edit - /doc.txt to -{WM_EDIT_RECT[2] - WM_TITLE_KEEP},0",
                                    timeout),
                "the server did not keep the bar of the title on the picture",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(10, 5) == EDIT_BAR_RGB
                and seen.pixel(0, 0) == WM_CURSOR_RGB,
            )
            expect(
                "the bar of the title is at the top of the picture",
                picture is not None and picture.pixel(10, 5) == EDIT_BAR_RGB,
                "the corner of the picture holds no bar of a title",
            )
            expect(
                "the pointer of the corner is in the picture",
                picture is not None and picture.pixel(0, 0) == WM_CURSOR_RGB,
                "the pointer of the corner is not on the picture",
            )

            # The sliver of the bar that is left is the handle: drag it back.
            goto(machine, 20, 8)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            for _ in range(2):
                machine.monitor_command("mouse_move 300 200")
                time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.5)
            expect(
                "a window of the corner comes back by its bar",
                machine.wait_serial("wm: move edit - /doc.txt to 24,400", timeout),
                "the drag of the sliver of the bar moved the window somewhere else",
            )

            # And the same bar puts it back where the test knows it.
            goto(machine, 124, 410)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            machine.monitor_command("mouse_move 16 -200")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 0 -160")
            time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.5)

            # The pointer is over the bar it dragged: it goes away before the
            # picture is read, because a cursor over the title hides a letter.
            goto(machine, 640, 600)
            picture = settled_picture(machine, workdir, lambda seen: title_at(seen, WM_EDIT_RECT))
            expect(
                "the bar of the title puts the window back",
                picture is not None and title_at(picture, WM_EDIT_RECT),
                "the editor is not back at its first rectangle",
            )

            # --- a window that hangs past the bottom comes back inside ----
            #
            # The bar of a window may hang over the bottom of the picture (the
            # drag keeps the bar and not the body); a window of that shape is
            # put away from the panel and the panel brings it back, and it
            # comes back at a rectangle that fits, because the model of a
            # window refuses a rectangle that is not a part of the screen.
            goto(machine, 300, 50)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            machine.monitor_command("mouse_move 0 200")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 0 200")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 0 200")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 0 100")
            time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.5)

            expect(
                "a window can hang past the bottom of the picture",
                machine.wait_serial("wm: move edit - /doc.txt to 40,740", timeout),
                "the drag did not put the window at 40,740",
            )

            goto(machine, 50, VBE_HEIGHT - 14)
            click(machine)
            expect(
                "the panel puts a window of the bottom away",
                machine.wait_serial("wm: put away edit", timeout),
                "the server did not put the editor away",
            )
            goto(machine, 50, VBE_HEIGHT - 14)
            click(machine)
            expect(
                "the panel brings it back inside the picture",
                machine.wait_serial("wm: restore edit", timeout),
                "the server did not bring the editor back",
            )

            goto(machine, 640, 600)
            back = (WM_EDIT_RECT[0], VBE_HEIGHT - WM_PANEL_HEIGHT - WM_EDIT_RECT[3],
                    WM_EDIT_RECT[2], WM_EDIT_RECT[3])
            picture = settled_picture(machine, workdir, lambda seen: title_at(seen, back))
            expect(
                "the window of the bottom comes back on the picture",
                picture is not None and title_at(picture, back),
                f"the title of the editor is not at {back[0]},{back[1]}",
            )

            # --- a raise puts a window over the one that covered it ---------
            #
            # The default desktop keeps the two windows apart (a raise must not
            # blank the text of the other window); the stacking is still the
            # rule, and a person who puts one window over another gets it. The
            # status line of the file manager is the pixel to read: it is blue
            # when the file manager is on top of the editor and black when the
            # editor is.
            goto(machine, 300, 365)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            machine.monitor_command("mouse_move 200 -56")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 200 0")
            time.sleep(0.15)
            machine.monitor_command("mouse_move 160 0")
            time.sleep(0.15)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.5)

            expect(
                "a window can be put over another one",
                machine.wait_serial("wm: move edit - /doc.txt to 600,300", timeout),
                "the drag did not put the editor over the file manager",
            )

            goto(machine, 900, 400)
            overlap = (WM_FM_RECT[0] + UI_FRAME_PAD, WM_FM_RECT[1] + WM_FM_RECT[3] - 10)
            picture = settled_picture(
                machine, workdir, lambda seen: seen.pixel(*overlap) == EDIT_PAPER_RGB
            )
            expect(
                "the window that was dragged last covers the other one",
                picture is not None and picture.pixel(*overlap) == EDIT_PAPER_RGB,
                f"the pixel {overlap} of the file manager is not the paper of the editor",
            )

            goto(machine, 900, 210)
            click(machine)
            expect(
                "a click raises the window over the one that covered it",
                machine.wait_serial("wm: raise fm", timeout),
                "the server did not raise the file manager",
            )

            goto(machine, 640, 700)
            picture = settled_picture(
                machine, workdir, lambda seen: seen.pixel(*overlap) == FM_BAR_RGB
            )
            expect(
                "the window that was raised is the one of the two that is drawn",
                picture is not None and picture.pixel(*overlap) == FM_BAR_RGB,
                f"the pixel {overlap} is not the line of status of the file manager",
            )

            # --- the pointer stays in the picture at both corners ----------
            for _ in range(12):
                machine.monitor_command("mouse_move 250 250")
                time.sleep(0.02)
            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(VBE_WIDTH - 12, VBE_HEIGHT - 15) == WM_CURSOR_RGB,
            )
            expect(
                "the pointer stays in the picture at the far corner",
                picture is not None
                and picture.pixel(VBE_WIDTH - 12, VBE_HEIGHT - 15) == WM_CURSOR_RGB,
                "the pointer of the far corner is not on the picture",
            )

            # --- a click on the file manager and Alt+F4 --------------------
            goto(machine, 900, 500)
            click(machine)
            expect(
                "a click raises the file manager",
                machine.wait_serial("wm: raise fm", timeout),
                "the server did not raise the file manager",
            )

            # --- the button of the panel acts on the release ---------------
            #
            # A press that slides away is a press of nothing: the window of a
            # button of the panel goes away when the button comes up on the
            # button, and not when a finger touches it.
            put_away = machine.read_serial().count("wm: put away")
            goto(machine, 50, VBE_HEIGHT - 14)
            machine.monitor_command("mouse_button 1")
            time.sleep(0.2)
            machine.monitor_command("mouse_move 300 0")
            time.sleep(0.2)
            machine.monitor_command("mouse_button 0")
            time.sleep(0.6)
            expect(
                "a press that slides off the panel does nothing",
                machine.read_serial().count("wm: put away") == put_away,
                "the window went away although the release was not on the button",
            )

            goto(machine, 50, VBE_HEIGHT - 14)
            click(machine)
            expect(
                "a click on the button of the panel puts the window away",
                machine.wait_serial("wm: put away edit", timeout),
                "the server did not put the editor away",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: desk_at(seen, WM_EDIT_RECT[0] + 20, WM_EDIT_RECT[1] + 60),
            )
            expect(
                "the window that is put away leaves the picture",
                picture is not None
                and desk_at(picture, WM_EDIT_RECT[0] + 20, WM_EDIT_RECT[1] + 60),
                "the rectangle of the editor is still painted",
            )

            goto(machine, 50, VBE_HEIGHT - 14)
            click(machine)
            expect(
                "a click on the button of the panel brings it back",
                machine.wait_serial("wm: restore edit", timeout),
                "the server did not bring the editor back",
            )

            # --- the square of closing of the panel closes a window --------
            #
            # The cross of the button is the second way out of a window, and
            # the one that works when the bar of its title is out of reach.
            button_w = len("edit - /doc.txt") * CELL_WIDTH + 12 + WM_PANEL_CLOSE + 2
            cross_x = 8 + button_w - WM_PANEL_CLOSE - 1 + WM_PANEL_CLOSE // 2
            goto(machine, cross_x, VBE_HEIGHT - 14)
            click(machine)

            expect(
                "the square of the panel asks the window to leave",
                machine.wait_serial("wm: ask to leave edit", timeout),
                "the cross of the panel did not ask the editor to leave",
            )
            expect(
                "the square of the panel closes the window",
                machine.wait_serial("wm: close edit", timeout),
                "the editor did not close its window",
            )

            empty = (WM_EDIT_RECT[0] + 20, WM_EDIT_RECT[1] + 60)
            picture = settled_picture(
                machine, workdir, lambda seen: desk_at(seen, *empty)
            )
            expect(
                "the rectangle of the closed window is the wallpaper",
                picture is not None and desk_at(picture, *empty),
                f"the pixel {empty} of the closed window is "
                f"{picture.pixel(*empty) if picture else None}",
            )
            expect(
                "the focus goes back to the window that is left",
                picture is not None
                and picture.pixel(WM_FM_RECT[0] - 1, WM_FM_RECT[1] - 1) == WM_FOCUS_RGB,
                "the ring of the focus is not around the file manager",
            )

            # --- Alt+F4 closes the last window and the desktop -------------
            machine.keys(["alt-f4"])
            expect(
                "Alt+F4 asks the window with the focus to leave",
                machine.wait_serial("wm: ask to leave fm - /", timeout),
                "the server did not forward the close to the file manager",
            )
            expect(
                "the file manager leaves",
                machine.wait_serial("wm: close fm - /", timeout),
                "the file manager did not close its window",
            )
            expect(
                "the last window ends the desktop",
                machine.wait_serial("wm: the desktop is closed", timeout),
                "the server did not close the desktop",
            )
            expect(
                "the shell has its screen back",
                machine.wait_serial(PROMPT, timeout),
                "the prompt of the shell never came back",
            )
            expect(
                "the file the editor typed is the file of the volume",
                machine.run_line("cat /doc.txt", timeout),
                "cat /doc.txt did not answer",
            )

            text = machine.read_serial()
            expect(
                "the line the editor typed is the line of the file",
                "hello" in text.split("cat /doc.txt")[-1],
                "the file does not hold the word the keys typed",
            )

            console = machine.picture(workdir)
            expect(
                "the console of the kernel is the picture again",
                console is not None and console.pixel(4, 4) == BLACK,
                "the picture after the desktop is not the console",
            )

            # --- the light theme of /etc/console ---------------------------
            #
            # The desktop reads the `theme` of /etc/console at its start, as
            # the console does: one word of the file and the desk, the windows
            # and the panel are the light palette, which is the proof that the
            # colours are roles and not numbers written into a program.
            expect(
                "the console takes the light theme",
                machine.run_line("config set console.theme light", timeout),
                "config did not answer for the light theme",
            )

            up_before = machine.read_serial().count("wm: the desktop is up")
            machine.type_line("exec /bin/wm")
            expect(
                "the desktop of the light theme starts",
                machine.wait_serial("wm: the palette is light", timeout),
                "the server did not take the theme of /etc/console",
            )
            expect(
                "the windows of the light desktop are up",
                wait_new(machine, "wm: the desktop is up", up_before, timeout),
                "the server never said that its windows were up in the light theme",
            )

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(4, 4) == WALL_TOP_LIGHT_RGB
                and seen.pixel(WM_EDIT_RECT[0] + 20, WM_EDIT_RECT[1] + 60)
                == palette_rgb(255, 255, 255),
                seconds=10.0,
            )
            expect(
                "the light theme is the gradient of the desk",
                picture is not None
                and picture.pixel(4, 4) == WALL_TOP_LIGHT_RGB
                and picture.pixel(4, 770) == WALL_BOTTOM_LIGHT_RGB
                and picture.pixel(4, 4) != picture.pixel(4, 770),
                "the desk of the light theme is not the light gradient",
            )
            expect(
                "the light theme is the surface of the panel",
                picture is not None and picture.pixel(4, VBE_HEIGHT - 4) == PANEL_LIGHT_RGB,
                "the panel of the light theme is not its surface",
            )
            # The body of the editor is white in the light theme, and a single
            # pixel of it is not the body: the first line of the file is a
            # glyph there, and a picture taken a frame early is the dark theme.
            # The share of the body that is white is the fact; one pixel is a
            # coin toss (it failed twice in the gate of 2.0.0-rc1 and passed on
            # the same tree a minute later).
            white = palette_rgb(255, 255, 255)
            body_total = 0
            body_white = 0
            if picture is not None:
                for x in range(WM_EDIT_RECT[0] + 8, WM_EDIT_RECT[0] + WM_EDIT_RECT[2] - 8, 3):
                    for y in range(WM_EDIT_RECT[1] + 40, WM_EDIT_RECT[1] + WM_EDIT_RECT[3] - 8, 3):
                        body_total += 1
                        if picture.pixel(x, y) == white:
                            body_white += 1
            expect(
                "the light theme is the sheet of a window",
                body_total > 0 and body_white * 10 >= body_total * 9,
                f"the body of the editor of the light theme is white in "
                f"{body_white} of {body_total} sample(s)",
            )
            expect(
                "the pointer of the light theme is seen on the white sheet",
                picture is not None
                and picture.pixel(VBE_WIDTH // 2 - 1, VBE_HEIGHT // 2 + 2) == WM_CURSOR_EDGE_RGB
                and picture.pixel(VBE_WIDTH // 2, VBE_HEIGHT // 2) != WM_CURSOR_EDGE_RGB,
                "the pointer of the light theme has no outline of the edge colour",
            )

            close_before = machine.read_serial().count("wm: close fm - /")
            machine.keys(["alt-f4"])
            expect(
                "the light desktop closes a window",
                wait_new(machine, "wm: close fm - /", close_before, timeout),
                "the file manager of the light theme did not close",
            )
            closed_before = machine.read_serial().count("wm: the desktop is closed")
            machine.keys(["alt-f4"])
            expect(
                "the light desktop closes its last window",
                wait_new(machine, "wm: the desktop is closed", closed_before, timeout),
                "the desktop of the light theme did not end",
            )
            expect(
                "the shell has its screen back after the light theme",
                machine.wait_serial(PROMPT, timeout),
                "the prompt of the shell never came back after the light desktop",
            )
        finally:
            machine.close()

    return checks


def run_text(loader: str, kernel: str, timeout: float, verbose: bool, display: str) -> list:
    """The fallback of ADR 0023: the text mode of the VGA, 80x25.

    The card is a Cirrus, which has a text mode and no Bochs VBE interface, so
    the port keeps the screen the firmware left and the whole console of 1.1
    is the one a machine without a picture has.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"text mode: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-text-") as workdir:
        machine = Machine(loader, kernel, workdir, "cirrus", display)

        try:
            booted = machine.wait_serial(PROMPT, timeout)
            expect("the serial console is up as before", booted, "the shell never printed its prompt")
            expect(
                "the port keeps the text mode",
                "x86-64: the screen is the text mode of the VGA, 80x25" in machine.read_serial(),
                "the boot did not say it fell back to the text mode",
            )

            boot = machine.screen_text(workdir)
            if verbose:
                print("--- the text screen after the boot ---")
                print(boot)

            expect("the kernel writes to the video memory", "MicroOS" in boot)
            expect("the prompt is on the screen", PROMPT.strip() in boot)
            # A screen nobody cleared would carry the line of the firmware at
            # the top; the kernel clears it before the banner.
            expect("the screen is the kernel's own", "SeaBIOS" not in boot and "iPXE" not in boot)

            picture = machine.picture(workdir)
            expect(
                "the device has the text picture of 80x25",
                picture is not None and (picture.width, picture.height) == (720, 400),
                f"the screenshot is {picture.width}x{picture.height}"
                if picture is not None
                else "screendump wrote no PPM",
            )
            expect(
                "the device draws the text",
                picture is not None and picture.ink_pixels() > 1000,
                f"only {picture.ink_pixels()} pixel(s) are not black"
                if picture is not None
                else "no picture",
            )

            # The cursor of the hardware is where the prompt ends: the shell
            # writes "microos> " and waits, so the register of the CRTC and the
            # screen of the kernel have to agree on the same cell.
            expected = -1
            for index, line in enumerate(boot.split("\n")):
                column = line.rfind(PROMPT.strip())
                if column >= 0:
                    # The prompt and the space after it are the cells the
                    # shell wrote last: the cursor sits after both.
                    expected = index * VGA_COLUMNS + column + len(PROMPT)

            position = machine.cursor()
            expect(
                "the cursor follows the text",
                position == expected and expected >= 0,
                f"the CRTC says {position}, the prompt ends at {expected}",
            )

            # --- the keyboard
            type_fsck(machine)

            answered = machine.wait_serial(ANSWER, timeout)
            expect("the keyboard reaches the shell", answered, f"the serial console never said {ANSWER!r}")

            window = machine.screen_text(workdir)
            if verbose:
                print("--- the text screen after fsck ---")
                print(window)

            expect("the keys are echoed on the screen", "fsck" in window)
            expect("the answer is on the screen", ANSWER in window)
            expect("the shell waits for the next line", PROMPT.strip() in window)

            # --- the colours of /etc/console (ADR 0021) on the text screen
            machine.type_line("config set console.theme light")
            themed = machine.wait_serial("the attribute of the screen 112", timeout)
            expect(
                "the theme reaches the attribute of the VGA",
                themed,
                "the console did not put black on light grey on the text screen",
            )

            # The attribute of every cell is the pair, and the characters stay
            # where they are: the line that was on the glass is read again.
            machine.screen_text(workdir)
            attributes = machine.attributes(workdir)
            expect(
                "every cell carries the new attribute",
                attributes and all(value == 0x70 for value in attributes),
                f"{len(set(attributes))} attribute(s) on the screen",
            )
            expect(
                "the text is still there after the repaint",
                "fsck" in machine.screen_text(workdir),
            )

            # --- the palette of the sixteen attributes on the text screen
            #
            # The framebuffer carries a colour of 0xRRRR as it was written
            # (above); this screen has the sixteen attributes of the VGA and
            # nothing else, so a command that colours a name and a log that
            # marks a level are read here as the nibble of the pair.
            expect(
                "the console goes back to the dark pair",
                machine.run_line("config set console.theme dark", timeout),
            )

            expect(
                "ls --color answers on the text screen",
                machine.run_line("ls --color /", timeout),
            )
            machine.run_line("chmod 0755 /etc/setting", timeout)
            expect(
                "a directory is the blue attribute of the VGA",
                attribute_at(machine, workdir, "proc/") == 0x01,
                f"the attribute of 'proc/' is {attribute_at(machine, workdir, 'proc/'):#04x}, "
                "not the blue on black",
            )

            expect(
                "ls --color answers for a directory of files",
                machine.run_line("ls --color /etc", timeout),
            )
            expect(
                "an executable is the green attribute of the VGA",
                attribute_at(machine, workdir, "setting") == 0x02,
                f"the attribute of 'setting' is "
                f"{attribute_at(machine, workdir, 'setting'):#04x}, not the green on black",
            )

            expect(
                "a broken ink is a warning",
                machine.run_line("config set console.ink nonsense", timeout),
            )
            expect(
                "a warning is the amber attribute of the VGA",
                attribute_at(machine, workdir, "console: ink") == 0x06,
                f"the attribute of the warning is "
                f"{attribute_at(machine, workdir, 'console: ink'):#04x}, not the amber on black",
            )

            machine.run_line("mkdir /tmp", timeout)
            expect(
                "a failure of a call is reported here too",
                machine.run_line("mount nosuchdev /tmp mfs", timeout),
            )
            expect(
                "an error is the red attribute of the VGA",
                attribute_at(machine, workdir, "mfs: there is no block device") == 0x04,
                f"the attribute of the error is "
                f"{attribute_at(machine, workdir, 'mfs: there is no block device'):#04x}, "
                "not the red on black",
            )
        finally:
            machine.close()

    return checks


def settled_picture(machine, directory: str, condition, seconds: float = 4.0):
    """The picture once the condition holds, or the last one before the end.

    A command that changes the layout of the console prints its line before
    the screen is painted again; a dump taken right after the line can catch
    the picture in the middle of the repaint. The polling is a property of the
    check of a picture and not of the machine: the command itself is done.
    """
    deadline = time.monotonic() + seconds
    picture = machine.picture(directory)
    while (
        picture is not None
        and not condition(picture)
        and time.monotonic() < deadline
    ):
        time.sleep(0.1)
        picture = machine.picture(directory)

    return picture


def keep_picture(machine, directory: str, name: str):
    """The screen of the machine into the directory of the screenshots.

    `--shots` names a directory a person reads after the run: the picture of
    the desk, of a window of a client and of the second size of the text are
    the evidence of a lane and not a step of the test, so the test keeps them
    and goes on when the directory cannot be written.
    """
    picture = machine.picture(directory)

    if picture is None or SHOTS_DIR is None:
        return picture

    try:
        os.makedirs(SHOTS_DIR, exist_ok=True)
        shutil.copyfile(os.path.join(directory, "screen.ppm"), os.path.join(SHOTS_DIR, name))
    except OSError as error:
        print(f"the screenshot {name} was not kept: {error}", file=sys.stderr)

    return picture


def changed_outside(first, second, keep) -> tuple:
    """How many pixels of two pictures differ outside the rectangles of `keep`.

    A compositor that repaints a rectangle paints the pixels of that rectangle
    and no other: a difference outside it is a fragment of the old picture - a
    flicker - and the answer of the test is the pixel and not a rate.
    """
    if first is None or second is None:
        return -1, None

    count = 0
    where = None
    stride = first.width * 3

    for y in range(first.height):
        row_a = first.pixels[y * stride : (y + 1) * stride]
        row_b = second.pixels[y * stride : (y + 1) * stride]

        if row_a == row_b:
            continue

        for (x, top, w, h) in keep:
            if top <= y < top + h:
                a = bytearray(row_a)
                b = bytearray(row_b)
                a[x * 3 : (x + w) * 3] = b"\0" * (w * 3)
                b[x * 3 : (x + w) * 3] = b"\0" * (w * 3)
                row_a, row_b = bytes(a), bytes(b)

        if row_a == row_b:
            continue

        for x in range(first.width):
            at = x * 3
            if row_a[at : at + 3] != row_b[at : at + 3]:
                count += 1
                if where is None:
                    where = (x, y)

    return count, where


def changed_inside(first, second, rect) -> tuple:
    """The pixels that differ inside `rect`, and the band of rows they are in.

    The answer is `(count, x0, y0, x1, y1)`: a key of a terminal that painted
    one row of its window has a band of one cell of the font and no other
    pixel, which is the half of "the damage and not a frame" that the two
    screenshots can see.
    """
    if first is None or second is None:
        return -1, 0, 0, 0, 0

    x, top, w, h = rect
    count = 0
    x0 = first.width
    y0 = first.height
    x1 = -1
    y1 = -1

    for y in range(top, top + h):
        for px in range(x, x + w):
            if first.pixel(px, y) != second.pixel(px, y):
                count += 1
                x0 = min(x0, px)
                y0 = min(y0, y)
                x1 = max(x1, px)
                y1 = max(y1, y)

    return count, x0, y0, x1, y1


def meter_numbers(text: str, after: int = 0) -> tuple:
    """The numbers of the last line of the meter of the frames after `after`.

    The line is the one `/bin/wm` writes on F12:
    "wm: the meter: 23 frames, 284160 px, the largest 624x64, 4 ms at the worst".
    """
    found = None

    for match in re.finditer(
        r"wm: the meter: (\d+) frames, (\d+) px, the largest (\d+)x(\d+), (\d+) ms",
        text[after:],
    ):
        found = tuple(int(value) for value in match.groups())

    return found


def wait_count(machine, needle: str, wanted: int, timeout: float) -> bool:
    """Wait until the transcript of the machine holds `wanted` copies of a line.

    The count is of the whole run, so a caller that waits for a line the
    machine printed before - "the desktop is closed" of a second desktop, the
    prompt of a second session - names the number it wants and not a
    difference taken after the fact.
    """
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if machine.read_serial().count(needle) >= wanted:
            return True
        time.sleep(0.05)

    return False


def wait_for(machine, needle: str, count: int, timeout: float) -> bool:
    """Wait for `count` copies of a line more than the transcript has now."""
    return wait_count(machine, needle, machine.read_serial().count(needle) + count, timeout)


def press_until(machine, key: str, needle: str, timeout: float = 20.0, tries: int = 3) -> bool:
    """A key pressed until its line is in the log, the way a person presses again.

    The byte of a key of the PS/2 is lost on a host under load (K21 of
    docs/debts.md), and a check of the desk that waits twenty seconds for the
    answer of one key would fail there although the machine is right. The
    answer is counted, so a press whose line was already in the log does not
    count for this one.
    """
    for _ in range(tries):
        # The count is taken before the press: a line that arrives while the
        # key is still on its way is the answer of this press and not a second
        # one to wait for.
        wanted = machine.read_serial().count(needle) + 1
        machine.keys([key])

        # The window of one press is long: the start of a program of the desk
        # is a `fork`, an `execve` and a frame of a client, and a second press
        # of Alt+T while the first is starting would be a second window.
        if wait_count(machine, needle, wanted, 10.0):
            return True

    return False


def wait_meter(machine, start: int, timeout: float):
    """The numbers of the next meter of the frames printed after `start`."""
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        numbers = meter_numbers(machine.read_serial(), start)
        if numbers is not None:
            return numbers
        time.sleep(0.05)

    return None


def run_shell_desktop(
    loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str
) -> list:
    """The desk a person starts at the prompt, on a machine with SMAP.

    `run_desktop` types `exec /bin/wm` at the prompt too, but its QEMU has the
    default processor, which does not advertise SMAP: a read the kernel makes
    from the memory of a program outside the window of ADR 0052 is an
    ordinary read there, and the fault of the laptop of a person is invisible.
    This boot asks for SMEP and SMAP and types the same line, then reads the
    picture of a window and checks that the machine is still answering after
    the desk has closed.

    The window that comes up is the proof of the whole path: the path of the
    open of `/dev/fb0`, the fork and execve of the two clients, the `poll` of
    their pipes and the copies of their revents into the array of the server.
    Any of those that faults in ring 0 ends with "the machine stops here"
    instead of a painted window.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"shell exec wm: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-shell-wm-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(image, disk)
        machine = Machine(loader, kernel, workdir, "std", display, disk,
                          cpu="qemu64,+smep,+smap")

        try:
            expect("the shell of the machine is up", machine.wait_serial(PROMPT, timeout),
                   "the shell never printed its prompt")
            boot = machine.read_serial()
            expect("the machine advertises the two bits",
                   "x86-64: SMEP and SMAP are on" in boot,
                   "the boot did not say that SMAP is on: the machine would not check the window")

            machine.type_line("exec /bin/wm")
            expect("the server takes the screen",
                   machine.wait_serial("fb: the console is taken", timeout),
                   "the kernel never said that /dev/fb0 was taken")
            expect("the desk of the shell comes up",
                   machine.wait_serial("wm: the desktop is up", timeout),
                   "the server never finished starting its windows; the log ends with "
                   + repr(machine.read_serial()[-300:]))
            expect("the machine did not stop on the way",
                   "the machine stops here" not in machine.read_serial(),
                   "a fault of ring 0 ended the machine before the desk was up")

            def title(seen) -> bool:
                return seen.text_at(WM_EDIT_RECT[0] + UI_FRAME_PAD,
                                    WM_EDIT_RECT[1] + UI_TITLE_Y, "edit - /doc.txt",
                                    EDIT_TITLE_INK_RGB, EDIT_BAR_RGB)

            picture = settled_picture(
                machine, workdir,
                lambda seen: seen.pixel(4, 4) == WALL_TOP_RGB and title(seen),
                seconds=10.0,
            )
            # settled_picture gives the last picture even when the condition
            # never held, so the picture is read here and not only its
            # existence: a stopped machine keeps the grid of the console on
            # the screen and must not pass for a desk with a window.
            painted = picture is not None and picture.pixel(4, 4) == WALL_TOP_RGB and title(picture)
            expect("a window of the desk is on the screen", painted,
                   "screendump never showed the wallpaper and the title of the editor")
            if picture is not None:
                keep_picture(machine, workdir, "shell-wm.ppm")

            expect("Alt+F4 closes the file manager",
                   press_until(machine, "alt-f4", "wm: close fm - /", timeout),
                   "the key did not reach the file manager")
            expect("Alt+F4 closes the editor",
                   press_until(machine, "alt-f4", "wm: close edit - /doc.txt", timeout),
                   "the key did not reach the editor")
            expect("the last window ends the desk",
                   machine.wait_serial("wm: the desktop is closed", timeout),
                   "the server did not close with its last window")
            expect("the shell answers after the desk",
                   machine.run_line("help", timeout),
                   "the machine was not alive after the desk; the log ends with "
                   + repr(machine.read_serial()[-300:]))
        finally:
            machine.close()

    return checks


def run_boot_desktop(
    loader: str, kernel: str, image: str, timeout: float, verbose: bool, display: str
) -> list:
    """The desk that starts with the machine, its clock and its terminal (2.0).

    The machine of this run is told to start the desktop by a file of /etc and
    not by a command a person types: `/etc/session` is the list of commands of
    the console session, the boot runs its `exec /bin/wm`, and the desk of 1.7
    comes up on the picture while the shell of the machine waits for it
    (ADR 0054). The test reads the clock of the panel, presses Alt+T for a
    terminal window, types a line into it, reads the answer of the program out
    of the pixels of the window, measures the frame of the compositor with the
    meter of the server (F12), stops a command with Ctrl+C, puts the window in
    the large font, and closes the last window, which gives the screen and the
    prompt of the shell back.
    """
    checks = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        checks.append((f"boot: {name}", bool(condition), detail))

    with tempfile.TemporaryDirectory(prefix="microos-x86-boot-") as workdir:
        disk = os.path.join(workdir, "disk.img")
        shutil.copyfile(image, disk)

        # --- the machine is told where its desk is --------------------------
        machine = Machine(loader, kernel, workdir, "std", display, disk)

        try:
            expect("the shell of the disk is up",
                   machine.wait_serial(PROMPT, timeout),
                   "the shell never printed its prompt")
            expect("the session of the console is the first one",
                   "shell ready" in machine.read_serial(),
                   "the boot did not start a session of the console")
            expect("a person writes the commands of the session",
                   machine.run_line("write /etc/session exec /bin/wm", timeout),
                   "the shell refused to write /etc/session")
            expect("the commands of the session are the desk",
                   machine.run_line("cat /etc/session", timeout),
                   "cat did not read the file of the session")
            expect("the file of the session names the server",
                   "exec /bin/wm" in machine.read_serial(),
                   "/etc/session does not hold the line of the server")
        finally:
            machine.close()

        # --- the machine boots to the desktop -------------------------------
        machine = Machine(loader, kernel, workdir, "std", display, disk)

        try:
            started = machine.wait_serial("wm: the desktop is up", timeout * 3)
            transcript = machine.read_serial()

            expect("the desk starts with the machine", started,
                   "the session did not run /bin/wm of /etc/session")
            expect("the session runs the commands of /etc before its prompt",
                   "session: exec /bin/wm" in transcript,
                   "the log of the session does not name the line it ran")
            expect("the shell waits under the desk",
                   PROMPT not in transcript,
                   "the prompt of the shell came before the desktop, so a "
                   "person would type at the shell and not at the desk")
            expect("the screen is the picture of a program",
                   machine.wait_serial("fb: the console is taken", timeout),
                   "the server did not take the screen")
            expect("the clock of the panel is the twenty-four hours of the theme",
                   machine.wait_serial("wm: the clock is the twenty-four hours", timeout),
                   "the server did not read the clock of /etc/console")
            expect("the two windows of the desk are the programs of 1.7",
                   "wm: window edit - /doc.txt" in transcript
                   and "wm: window fm - /" in transcript,
                   "the desk of the boot is not the desk of the server")

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: seen.pixel(4, 4) == WALL_TOP_RGB
                and text_at(seen, WM_EDIT_RECT, "edit - /doc.txt"),
            )
            expect("the desk of the machine is painted", picture is not None,
                   "screendump wrote no PPM")
            picture = keep_picture(machine, workdir, "boot-desk.ppm")

            clock = None
            if picture is not None:
                clock = picture.letters_at(
                    WM_CLOCK_RECT[0], WM_CLOCK_RECT[1], 8, WM_PANEL_INK_RGB, WM_PANEL_RGB
                )

            expect("the panel shows the hour of the machine",
                   clock is not None and re.fullmatch(r"[0-9]{2}:[0-9]{2}:[0-9]{2}", clock)
                   is not None,
                   f"the cells of the clock are {clock!r}")

            later = settled_picture(
                machine,
                workdir,
                lambda seen: seen.letters_at(
                    WM_CLOCK_RECT[0], WM_CLOCK_RECT[1], 8, WM_PANEL_INK_RGB, WM_PANEL_RGB
                )
                != clock,
                4.0,
            )
            expect("the clock of the panel follows the second",
                   later is not None
                   and later.letters_at(
                       WM_CLOCK_RECT[0], WM_CLOCK_RECT[1], 8, WM_PANEL_INK_RGB, WM_PANEL_RGB
                   )
                   != clock,
                   "the hour of the panel did not change in four seconds")
            changed, where = changed_outside(picture, later, [WM_CLOCK_RECT])
            expect("the second of the desk is the rectangle of the clock",
                   later is not None and changed == 0,
                   f"{changed} pixel(s) outside the clock changed, first at {where}")

            # --- Alt+T opens a terminal window ------------------------------
            expect("Alt+T opens a terminal window",
                   press_until(machine, "alt-t", "wm: window term", timeout),
                   "the server did not start /bin/term")
            expect("the program of the terminal is a window",
                   machine.wait_serial("term: the window of the terminal is up", timeout),
                   "the program of the terminal did not start")

            picture = settled_picture(
                machine,
                workdir,
                lambda seen: text_at(seen, WM_TERM_RECT, "term")
                and text_at(seen, WM_TERM_RECT, TERM_GREETING, row=0, body=True),
            )
            picture = keep_picture(machine, workdir, "boot-term.ppm")

            expect("the terminal is a window of the desk",
                   picture is not None and text_at(picture, WM_TERM_RECT, "term"),
                   "the title bar of the terminal is not 'term'")
            expect("the terminal greets the person of the machine",
                   picture is not None
                   and text_at(picture, WM_TERM_RECT, TERM_GREETING, row=0, body=True),
                   "the first row of the window is not the greeting of the terminal")
            expect("the terminal has a prompt",
                   picture is not None
                   and text_at(picture, WM_TERM_RECT, TERM_PROMPT, row=2, body=True),
                   "the prompt of the terminal is not in the window")

            # --- the compositor: a frame of the desk against a key ----------
            expect("the server has a meter of its frames",
                   press_until(machine, "f12", "wm: the meter", timeout),
                   "F12 did not print the meter of the compositor; the log ends with "
                   + repr(machine.read_serial()[-300:]))

            start = len(machine.read_serial())
            machine.keys(["f12"])
            idle = wait_meter(machine, start, timeout)

            expect("a still desk paints nothing but the second of the clock",
                   idle is not None and idle[0] <= 1
                   and idle[1] <= WM_CLOCK_RECT[2] * WM_CLOCK_RECT[3],
                   f"the frames of a desk with no key are {idle}, and a still desk is "
                   f"at most the rectangle of the clock")

            # --- a line typed into the window, and its answer ---------------
            start = len(machine.read_serial())
            for key in "cat /etc/setting":
                machine.keys([{" ": "spc", "/": "slash"}.get(key, key)])
            machine.keys(["ret"])

            expect("the command of the window is the one that typed",
                   machine.wait_serial("term: the command cat", timeout),
                   "the window did not run the line of the keys")
            expect("the program of the window is a child of the machine",
                   machine.wait_serial("term: cat left with 0", timeout),
                   "the command of the window did not end")

            answer = settled_picture(
                machine,
                workdir,
                lambda seen: text_at(seen, WM_TERM_RECT, "hostname=x86_64", row=4, body=True),
            )
            answer = keep_picture(machine, workdir, "boot-term-answer.ppm")

            expect("the answer of the command is in the window",
                   answer is not None
                   and text_at(answer, WM_TERM_RECT, "hostname=x86_64", row=4, body=True)
                   and text_at(answer, WM_TERM_RECT, "console=vga", row=5, body=True),
                   "the window of the terminal does not hold the lines of /etc/setting")
            expect("the prompt of the terminal comes back after the command",
                   answer is not None
                   and text_at(answer, WM_TERM_RECT, TERM_PROMPT, row=6, body=True),
                   "the terminal did not return to its prompt")

            machine.keys(["f12"])
            numbers = wait_meter(machine, start, timeout)

            expect("the meter counts the frames of a typed line",
                   numbers is not None,
                   "the F12 after the line printed no meter")

            expect("the line typed in the window cost a part of the screen",
                   numbers is not None and numbers[1] < VBE_WIDTH * VBE_HEIGHT // 2,
                   f"the frames of the line and the answer are {numbers}, not a part of "
                   f"the {VBE_WIDTH * VBE_HEIGHT} pixels of the picture")
            expect("a frame of the desk after the first is a rectangle of a window",
                   numbers is not None and numbers[2] * numbers[3] < VBE_WIDTH * VBE_HEIGHT // 4,
                   f"the largest rectangle of the run is {numbers[2:4] if numbers else None}, "
                   f"not a part of the picture")

            # --- a key of the window costs a row of the window --------------
            #
            # The meter of the server is reset by F12, so the three keys
            # between two presses are the whole measurement: the compositor
            # paints the row of the line and, at most, the rectangle of the
            # clock, and neither is a screen.
            start = len(machine.read_serial())
            machine.keys(["f12"])
            wait_meter(machine, start, timeout)
            start = len(machine.read_serial())
            for key in "abc":
                machine.keys([key])
            time.sleep(0.4)
            machine.keys(["f12"])
            keys = wait_meter(machine, start, timeout)

            expect("the meter counts the frames of three keys",
                   keys is not None,
                   "the F12 after the keys printed no meter")

            expect("a key of the window costs its row and not a screen",
                   keys is not None and keys[0] >= 1 and keys[1] > 0
                   and keys[1] < 8 * WM_TERM_RECT[2] * CELL_HEIGHT
                   and keys[3] <= CELL_HEIGHT and keys[2] <= WM_TERM_RECT[2],
                   f"the frames of three keys are {keys}: a screen is "
                   f"{VBE_WIDTH * VBE_HEIGHT} pixels and a row of the window "
                   f"{WM_TERM_RECT[2] * CELL_HEIGHT}")

            # The line of the three keys is erased, so the command of the next
            # check is the one the keys type.
            machine.keys(["ctrl-u"])
            time.sleep(0.2)

            # --- a key paints a row and nothing else ------------------------
            before = keep_picture(machine, workdir, "boot-before-key.ppm")
            machine.keys(["x", "y"])
            time.sleep(0.4)
            after = machine.picture(workdir)

            changed, where = changed_outside(before, after, [WM_TERM_RECT, WM_CLOCK_RECT])
            expect("a key of the window does not paint the desk",
                   before is not None and after is not None and changed == 0,
                   f"{changed} pixel(s) of the desk outside the window changed, first at {where}")

            inside, _, y0, _, y1 = changed_inside(before, after, WM_TERM_RECT)
            expect("a key paints one row of the window",
                   inside > 0 and (y1 - y0) < CELL_HEIGHT,
                   f"{inside} pixel(s) of the window changed in the rows {y0}..{y1}, and a "
                   f"key of a row changes one cell of the font")

            # The line of the two keys is erased whole, so the command of the
            # next check is the one its keys type and not "xycat".
            machine.keys(["ctrl-u"])
            time.sleep(0.2)

            # --- Ctrl+C stops the command of the window --------------------
            commands = machine.read_serial().count("term: the command cat")
            for key in "cat":
                machine.keys([key])
            machine.keys(["ret"])
            expect("the window runs a program that reads its input",
                   wait_count(machine, "term: the command cat", commands + 1, timeout),
                   "the window did not run the second command; the log ends with "
                   + repr(machine.read_serial()[-200:]))
            for key in "hello":
                machine.keys([key])
            machine.keys(["ret"])
            time.sleep(0.6)

            typed = machine.picture(workdir)
            expect("the keys of a running program are its input",
                   typed is not None
                   and text_at(typed, WM_TERM_RECT, "hello", row=7, body=True),
                   "the program of the window did not echo the line of the keys")

            expect("Ctrl+C tells the command to stop",
                   press_until(machine, "ctrl-c", "term: Ctrl+C", timeout),
                   "the window did not interrupt the command")
            expect("the command of the window is gone",
                   machine.wait_serial("term: cat left with 130", timeout),
                   "the interrupted command did not end")
            interrupted = settled_picture(
                machine,
                workdir,
                lambda seen: text_at(seen, WM_TERM_RECT, TERM_PROMPT, row=9, body=True),
            )
            expect("the window comes back to its prompt after Ctrl+C",
                   interrupted is not None
                   and text_at(interrupted, WM_TERM_RECT, TERM_PROMPT, row=9, body=True),
                   "the terminal did not print its prompt after the interrupt")

            # --- the second size of the font --------------------------------
            expect("Ctrl+T is the second size of the text",
                   press_until(machine, "ctrl-t", "term: the text is 16x32", timeout),
                   "the window did not change its font")
            large = settled_picture(
                machine,
                workdir,
                lambda seen: text_at_scaled(seen, WM_TERM_RECT, TERM_LARGE_TEXT, scale=2,
                                            row=0, body=True),
            )
            large = keep_picture(machine, workdir, "boot-term-large.ppm")

            expect("the large text of the terminal is the same glyphs at twice the cell",
                   large is not None
                   and text_at_scaled(large, WM_TERM_RECT, TERM_LARGE_TEXT, scale=2, row=0,
                                      body=True),
                   "the row of the large text is not the font of the desk drawn twice")
            expect("the window of the large text holds the prompt of its grid",
                   large is not None
                   and text_at_scaled(large, WM_TERM_RECT, TERM_PROMPT, scale=2, row=1,
                                      body=True),
                   "the prompt of the large text is not in the window")

            expect("Ctrl+T comes back to the small text",
                   press_until(machine, "ctrl-t", "term: the text is 8x16", timeout),
                   "the window did not come back to the small font")

            # --- the desk ends with its last window -------------------------
            expect("Ctrl+Q leaves the terminal",
                   press_until(machine, "ctrl-q", "wm: close term", timeout),
                   "the window of the terminal did not close")

            # The windows of the default desk leave one at a time: the focus
            # goes to the window under the one that closed, and the Alt+F4 of
            # the next turn is the close of that one.
            expect("Alt+F4 closes the file manager of the desk",
                   press_until(machine, "alt-f4", "wm: close fm - /", timeout),
                   "the file manager of the boot did not close")
            expect("Alt+F4 closes the editor of the desk",
                   press_until(machine, "alt-f4", "wm: close edit - /doc.txt", timeout),
                   "the editor of the boot did not close")

            expect("the last window ends the desk of the boot",
                   machine.wait_serial("wm: the desktop is closed", timeout),
                   "the desk did not close with its last window")
            expect("the shell of the session has its screen back",
                   machine.wait_serial(PROMPT, timeout),
                   "the session did not come back to its prompt")
            expect("the shell of the machine works after the desk",
                   machine.run_line("cat /etc/session", timeout),
                   "the shell did not run a command after the desktop")

            frames = None
            for match in re.finditer(
                r"wm: (\d+) frames, the first (\d+) px of (\d+) in (\d+) ms", machine.read_serial()
            ):
                frames = tuple(int(value) for value in match.groups())

            expect("the report of the compositor compares a frame with a screen",
                   frames is not None and frames[1] == frames[2] == VBE_WIDTH * VBE_HEIGHT,
                   f"the first frame of the report is {frames}")

            # --- the clock of another format of the theme -------------------
            expect("the theme names the twelve hours",
                   machine.run_line("config set console.clock 12h", timeout),
                   "config refused the clock of the theme")

            machine.type_line("exec /bin/wm")
            expect("the desk of the theme starts",
                   machine.wait_serial("wm: the clock is the twelve hours", timeout),
                   "the server did not read the new format of /etc/console")

            twelve_x = WM_CLOCK_RECT[0] - 3 * CELL_WIDTH

            def twelve_hour(seen) -> str:
                return seen.letters_at(twelve_x, WM_CLOCK_RECT[1], 11, WM_PANEL_INK_RGB,
                                       WM_PANEL_RGB).strip()

            # The desk is the picture of the *windows* of the theme as well:
            # the clock of the panel is in the first frame of the server, and a
            # dump taken for the clock alone can be a desk whose clients have
            # not painted yet.
            clock = settled_picture(
                machine,
                workdir,
                lambda seen: text_at(seen, WM_EDIT_RECT, "edit - /doc.txt")
                and re.fullmatch(r"\d{1,2}:\d{2}:\d{2} (AM|PM)", twelve_hour(seen)) is not None,
                6.0,
            )
            letters = twelve_hour(clock) if clock is not None else None
            expect("the panel of the twelve hours writes the hour of the theme",
                   letters is not None
                   and re.fullmatch(r"\d{1,2}:\d{2}:\d{2} (AM|PM)", letters) is not None,
                   f"the cells of the twelve-hour clock are {letters!r}")
            keep_picture(machine, workdir, "boot-clock-12h.ppm")

            desktops = machine.read_serial().count("wm: the desktop is closed")
            prompts = machine.read_serial().count(PROMPT)

            expect("Alt+F4 closes the file manager of the theme",
                   press_until(machine, "alt-f4", "wm: close fm - /", timeout),
                   "the file manager of the theme did not close")
            expect("Alt+F4 closes the editor of the theme",
                   press_until(machine, "alt-f4", "wm: close edit - /doc.txt", timeout),
                   "the editor of the theme did not close")
            expect("the desk of the theme ends",
                   wait_count(machine, "wm: the desktop is closed", desktops + 1, timeout),
                   "the second desktop did not close; the log ends with "
                   + repr(machine.read_serial()[-300:]))
            expect("the session of the console is a fresh one",
                   wait_count(machine, PROMPT, prompts + 1, timeout),
                   "the prompt of the shell did not come back; the log ends with "
                   + repr(machine.read_serial()[-300:]))

            # --- the desk leaves, and the machine goes down -----------------
            #
            # The desk is a foreground program of the session (ADR 0054), so
            # the prompt it gave back is the prompt of the shell that can end
            # the machine: a `shutdown` of the desk has to sync the volumes and
            # take the power down like the one of ADR 0050 (`make power-smoke`
            # holds the same promise without a window).
            machine.type_line("shutdown")
            expect("the machine of the desk switches itself off",
                   machine.wait_serial("power: the volumes are synced", timeout * 2),
                   "shutdown did not sync the volumes; the log ends with "
                   + repr(machine.read_serial()[-300:]))

            # The ACPI S5 of QEMU is a few seconds of a machine under load and
            # the power of the guest is the end of the process: the wait is the
            # patience of two timeouts and not the window of one.
            deadline = time.monotonic() + 2 * timeout
            while machine.process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.2)

            expect("the desk of the machine ends with the power",
                   machine.process.poll() is not None,
                   "QEMU was still running after the shutdown of the desk")
        finally:
            machine.close()

    return checks


def main(argv: list) -> int:
    parser = argparse.ArgumentParser(description="the screen and keyboard test of the x86-64 port")
    parser.add_argument("--timeout", type=float, default=20.0, help="the seconds each boot gets")
    parser.add_argument("--verbose", action="store_true", help="print the screen of the machine")
    parser.add_argument(
        "--display",
        default="none",
        help="the display of QEMU: none (the default, no X needed) or gtk for the real window",
    )
    parser.add_argument(
        "--image",
        default=os.path.join(BUILD, "programs.img"),
        help="the disk of `make x86-disk`, which carries /bin/ui, /bin/edit and /bin/fm",
    )
    parser.add_argument(
        "--shots",
        default=os.path.join(BUILD, "screens"),
        help="the directory of the screenshots of the run (default: build-x86/screens)",
    )
    args = parser.parse_args(argv)

    global SHOTS_DIR
    SHOTS_DIR = args.shots if args.shots else None

    loader = os.path.join(BUILD, "loader32.elf")
    kernel = os.path.join(BUILD, "microos64.bin")

    for path in (loader, kernel):
        if not os.path.isfile(path):
            print(f"missing {path}: run make x86-build", file=sys.stderr)
            return 2

    if not os.path.isfile(args.image):
        print(f"missing {args.image}: run make x86-disk", file=sys.stderr)
        return 2

    try:
        checks = run_framebuffer(loader, kernel, args.timeout, args.verbose, args.display)
        checks += run_ui(loader, kernel, args.image, args.timeout, args.verbose, args.display)
        checks += run_editor(loader, kernel, args.image, args.timeout, args.verbose, args.display)
        checks += run_fm(loader, kernel, args.image, args.timeout, args.verbose, args.display)
        checks += run_desktop(loader, kernel, args.image, args.timeout, args.verbose, args.display)
        checks += run_shell_desktop(
            loader, kernel, args.image, args.timeout, args.verbose, args.display
        )
        checks += run_boot_desktop(
            loader, kernel, args.image, args.timeout, args.verbose, args.display
        )
        checks += run_text(loader, kernel, args.timeout, args.verbose, args.display)
    except FileNotFoundError:
        print("qemu-system-x86_64 is not installed", file=sys.stderr)
        return 2
    except RuntimeError as error:
        print(f"{error}", file=sys.stderr)
        return 2

    failed = [item for item in checks if not item[1]]

    print("Micro OS — the screen test of the x86-64 port (framebuffer, VGA text, PS/2 keyboard)")
    print("=" * 60)
    for name, ok, detail in checks:
        print(f"  {' ok ' if ok else 'FAIL'}  {name}")
        if detail and not ok:
            print(f"          {detail}")

    print("=" * 60)
    if failed:
        print(f"FAILED: {len(failed)} of {len(checks)} check(s) did not pass")
        return 1

    print(f"OK: {len(checks)} check(s) passed")

    if SHOTS_DIR is not None:
        print(f"the screenshots of the run are in {SHOTS_DIR}")

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
