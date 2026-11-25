/*
 * Micro OS — the screen of a terminal (ADR 0054, docs/desktop.md).
 *
 * A terminal window is a grid of cells and a stream of bytes: a program writes
 * a line, the terminal puts the characters in the cells, a newline moves to the
 * next row and a row that leaves the top of the grid is gone. The grid is the
 * portable half of `/bin/term`: it knows nothing of a process, a pipe or a
 * screen, so `tests/cases/test_ui_term.c` holds it on the host, and the
 * program is the transport of the bytes and the picture of the cells.
 *
 * The bytes of this system are the ones of the console of the kernel: text
 * with the line endings of a Unix, the backspace, the tab, and the escape
 * sequences a full-screen program of the console writes. The sequences of the
 * screen that the console of x86-64 grew in 1.9 are a subset of the ones here
 * (ADR 0051 §7): the cursor is put at a cell, the screen is cleared, and a
 * line is erased to its end. A sequence this file does not know is swallowed
 * whole and leaves no trace, which is the rule of the console and what makes
 * `ESC[?25l` harmless.
 *
 * A cell is one character and nothing else: the colours of a terminal window
 * are the two of the theme (`ink` on `surface`), and a program that colours a
 * name with the sixteen of the CGA is a program of the console. The damage is
 * a rectangle of cells - the rows that changed since the last question - so a
 * key repaints one row of the window and not the window.
 */
#ifndef MICRO_OS_UI_TERM_H
#define MICRO_OS_UI_TERM_H

#include <stdbool.h>

#include "kernel/types.h"

/* The largest terminal of a window of 1280x800 at the small cell (8x16) and
 * the room of a scroll of the rows: the buffers are the caller's and these
 * are the numbers a caller sizes them by. */
#define UI_TERM_COLS_MAX 160
#define UI_TERM_ROWS_MAX 50

/* One cell of the grid: a character, and the space is the empty cell. */
struct ui_term {
    int cols;
    int rows;
    char *cells; /* cols * rows, row after row, ' ' for an empty cell */
    int cursor_x;
    int cursor_y;

    /*
     * The deferred wrap of a terminal: a character written in the last cell of
     * a row leaves the cursor there and the *next* character moves to the row
     * below. A row filled to its width does not scroll the grid by itself,
     * which is what makes the status bar of a program one line and not two
     * (ADR 0051 §7).
     */
    bool wrap_next;

    /* The escape sequence in flight: 0 for the text, 1 after the ESC, 2 in a
     * CSI whose parameters are being read, 3 after the final byte. */
    int escape;
    int parameters[4];
    int parameter_count;

    /* The cells that changed since the last take, in cells: an empty damage
     * has `w` or `h` at zero. */
    int dirty_x0;
    int dirty_y0;
    int dirty_x1;
    int dirty_y1;

    /* The lines that scrolled off the top, for the report of the program. */
    unsigned scrolled;
};

/*
 * A terminal of `cols` by `rows` cells over the buffer of the caller, which
 * holds `cols * rows` bytes. The grid starts blank and the cursor is at the
 * top left.
 */
void ui_term_init(struct ui_term *term, int cols, int rows, char *cells);

/* The whole grid blank and the cursor at home; the damage is the whole grid. */
void ui_term_clear(struct ui_term *term);

/*
 * The bytes of a program: the characters go into the cells, a newline ends a
 * row, the carriage return is the start of the row, the backspace moves one
 * cell left (the caller writes the space that erases it), the tab goes to the
 * next multiple of eight columns, and the escape sequences of `ui_term.h` are
 * read whole. Returns the number of bytes taken, or a negative number when
 * there is no grid. A character before the printable ASCII is dropped: it is
 * a control code of a program and not a letter of the screen.
 */
int ui_term_put(struct ui_term *term, const char *data, k_size_t len);

/*
 * Put the cursor at a cell (0-based, already inside the grid) and forget the
 * deferred wrap: the move of a program that redraws a status line.
 */
void ui_term_goto(struct ui_term *term, int x, int y);

/*
 * The cells that changed since the last call, or false when none did: the
 * rectangle is the rows and the columns of the grid that changed, and a caller
 * draws the line of every row of it. Taking the damage empties it.
 */
bool ui_term_take_damage(struct ui_term *term, int *x, int *y, int *w, int *h);

#endif /* MICRO_OS_UI_TERM_H */
