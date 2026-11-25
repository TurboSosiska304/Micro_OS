/*
 * Micro OS — the grid of a terminal window (ui/term.h).
 *
 * The state machine is small on purpose: the console of the kernel is the
 * terminal of the machine and `/bin/term` is a window over the programs of
 * /bin, so the bytes here are the text, the line endings and the few sequences
 * of a program that clears a screen. A sequence is read byte by byte through
 * the states of `struct ui_term`, and one this version does not know ends at
 * its final byte like every other.
 */
#include "ui/term.h"

#include <string.h>

/* The sequences of the screen that this terminal answers: erase to the end of
 * the line, the whole screen, and the cursor home. The numbers are the ones of
 * `ports/port_x86_64/src/console.c` for ESC[K, ESC[2J and ESC[H. */
#define TERM_ESC 0x1B
#define TERM_TAB 8

static bool term_ready(const struct ui_term *term)
{
    return term != NULL && term->cells != NULL && term->cols > 0 && term->rows > 0;
}

static char *cell_at(struct ui_term *term, int x, int y)
{
    return &term->cells[(size_t)y * (size_t)term->cols + (size_t)x];
}

static void damage_clear(struct ui_term *term)
{
    term->dirty_x0 = 0;
    term->dirty_y0 = 0;
    term->dirty_x1 = -1;
    term->dirty_y1 = -1;
}

static void mark(struct ui_term *term, int x, int y)
{
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= term->cols) {
        x = term->cols - 1;
    }
    if (y >= term->rows) {
        y = term->rows - 1;
    }

    if (term->dirty_x1 < term->dirty_x0 || term->dirty_y1 < term->dirty_y0) {
        term->dirty_x0 = x;
        term->dirty_y0 = y;
        term->dirty_x1 = x;
        term->dirty_y1 = y;
        return;
    }

    if (x < term->dirty_x0) {
        term->dirty_x0 = x;
    }
    if (y < term->dirty_y0) {
        term->dirty_y0 = y;
    }
    if (x > term->dirty_x1) {
        term->dirty_x1 = x;
    }
    if (y > term->dirty_y1) {
        term->dirty_y1 = y;
    }
}

static void row_blank(struct ui_term *term, int y)
{
    memset(cell_at(term, 0, y), ' ', (size_t)term->cols);
}

void ui_term_init(struct ui_term *term, int cols, int rows, char *cells)
{
    if (term == NULL) {
        return;
    }

    memset(term, 0, sizeof(*term));

    if (cols > UI_TERM_COLS_MAX) {
        cols = UI_TERM_COLS_MAX;
    }
    if (rows > UI_TERM_ROWS_MAX) {
        rows = UI_TERM_ROWS_MAX;
    }

    term->cols = cols;
    term->rows = rows;
    term->cells = cells;

    damage_clear(term);
    ui_term_clear(term);
}

void ui_term_goto(struct ui_term *term, int x, int y)
{
    if (!term_ready(term)) {
        return;
    }

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x > term->cols - 1) {
        x = term->cols - 1;
    }
    if (y > term->rows - 1) {
        y = term->rows - 1;
    }

    term->cursor_x = x;
    term->cursor_y = y;
    term->wrap_next = false;
    mark(term, x, y);
}

void ui_term_clear(struct ui_term *term)
{
    int row;

    if (!term_ready(term)) {
        return;
    }

    for (row = 0; row < term->rows; row++) {
        row_blank(term, row);
    }

    term->cursor_x = 0;
    term->cursor_y = 0;
    term->wrap_next = false;
    term->escape = 0;

    mark(term, 0, 0);
    mark(term, term->cols - 1, term->rows - 1);
}

/* A line feed: down one row, and a row that leaves the top of the grid makes
 * the whole grid scroll. The scroll is the one damage a terminal cannot make
 * small: every row of the window is a row of another line after it. */
static void line_feed(struct ui_term *term)
{
    term->wrap_next = false;

    if (term->cursor_y + 1 < term->rows) {
        term->cursor_y++;
        return;
    }

    if (term->rows > 1) {
        memmove(cell_at(term, 0, 0), cell_at(term, 0, 1),
                (size_t)(term->rows - 1) * (size_t)term->cols);
    }

    row_blank(term, term->rows - 1);
    term->scrolled++;

    mark(term, 0, 0);
    mark(term, term->cols - 1, term->rows - 1);
}

/* The cursor to the start of the next row, the carriage return and the line
 * feed of a text: the console of the machine puts the cursor at the start of
 * the next line on a newline, and this terminal does the same. */
static void newline(struct ui_term *term)
{
    term->cursor_x = 0;
    line_feed(term);
}

static void put_character(struct ui_term *term, char character)
{
    if (term->wrap_next) {
        term->cursor_x = 0;
        line_feed(term);
    }

    *cell_at(term, term->cursor_x, term->cursor_y) = character;
    mark(term, term->cursor_x, term->cursor_y);

    if (term->cursor_x + 1 >= term->cols) {
        /* The last cell of a row: the cursor stays on it and the next
         * character wraps, so a full line is one row and not two. */
        term->wrap_next = true;
        return;
    }

    term->cursor_x++;
}

static void erase_to_end_of_line(struct ui_term *term)
{
    int x;

    for (x = term->cursor_x; x < term->cols; x++) {
        *cell_at(term, x, term->cursor_y) = ' ';
    }

    mark(term, term->cursor_x, term->cursor_y);
    mark(term, term->cols - 1, term->cursor_y);
}

static void erase_line(struct ui_term *term)
{
    row_blank(term, term->cursor_y);
    mark(term, 0, term->cursor_y);
    mark(term, term->cols - 1, term->cursor_y);
}

static void sequence_final(struct ui_term *term, int final)
{
    switch (final) {
    case 'J': {
        /* ESC[J and ESC[0J erase from the cursor down, ESC[1J from the top to
         * the cursor, ESC[2J all of it. The parameter is the first one. */
        int mode = term->parameter_count > 0 ? term->parameters[0] : 0;

        if (mode == 2) {
            ui_term_clear(term);
            return;
        }

        if (mode == 0) {
            int row;

            erase_to_end_of_line(term);
            for (row = term->cursor_y + 1; row < term->rows; row++) {
                row_blank(term, row);
                mark(term, 0, row);
                mark(term, term->cols - 1, row);
            }
        }
        return;
    }
    case 'K':
        /* ESC[K to the end of the line, ESC[1K from its start, ESC[2K the line
         * whole. */
        {
            int mode = term->parameter_count > 0 ? term->parameters[0] : 0;

            if (mode == 1) {
                int x;

                for (x = 0; x <= term->cursor_x && x < term->cols; x++) {
                    *cell_at(term, x, term->cursor_y) = ' ';
                }

                mark(term, 0, term->cursor_y);
                mark(term, term->cursor_x, term->cursor_y);
            } else if (mode == 2) {
                erase_line(term);
            } else {
                erase_to_end_of_line(term);
            }
        }
        return;
    case 'H':
    case 'f': {
        /* ESC[row;colH, 1-based, a missing number is 1: the cursor home when
         * the sequence carries nothing. */
        int row = term->parameter_count > 0 ? term->parameters[0] : 1;
        int column = term->parameter_count > 1 ? term->parameters[1] : 1;

        ui_term_goto(term, column - 1, row - 1);
        return;
    }
    default:
        /* A sequence this terminal does not know leaves no trace, which is
         * the rule of the console and what makes ESC[?25l harmless. */
        return;
    }
}

/* The byte of a sequence in flight; the answer is false when it was taken by
 * the sequence and true when it is text after all (a sequence that never
 * finished). */
static bool escape_byte(struct ui_term *term, char byte)
{
    if (term->escape == 1) {
        if (byte == '[') {
            term->escape = 2;
            term->parameter_count = 0;
            term->parameters[0] = 0;
            term->parameters[1] = 0;
            term->parameters[2] = 0;
            term->parameters[3] = 0;
            return false;
        }

        /* ESC with a letter is a two-byte sequence of another screen (ESC c of
         * a reset, ESC 7 of a save): the final byte ends it and it leaves no
         * trace on the grid. */
        term->escape = 0;
        return false;
    }

    if (term->escape != 2) {
        term->escape = 0;
        return true;
    }

    if (byte >= '0' && byte <= '9') {
        if (term->parameter_count < 1) {
            term->parameter_count = 1;
        }

        term->parameters[term->parameter_count - 1] =
            term->parameters[term->parameter_count - 1] * 10 + (byte - '0');
        return false;
    }

    if (byte == ';') {
        if (term->parameter_count < 4) {
            term->parameter_count++;
            term->parameters[term->parameter_count - 1] = 0;
        }
        return false;
    }

    /* The private markers of a sequence (ESC[?25l) and the separators of the
     * parameters (a colon) are read and dropped; the final byte is the one of
     * the range 0x40..0x7E. */
    if (byte < 0x40 || byte > 0x7E) {
        return false;
    }

    term->escape = 0;
    sequence_final(term, (unsigned char)byte);
    return false;
}

int ui_term_put(struct ui_term *term, const char *data, k_size_t len)
{
    k_size_t at;

    if (!term_ready(term) || data == NULL) {
        return -1;
    }

    for (at = 0; at < len; at++) {
        unsigned char byte = (unsigned char)data[at];

        if (term->escape != 0) {
            (void)escape_byte(term, (char)byte);
            continue;
        }

        switch (byte) {
        case TERM_ESC:
            term->escape = 1;
            continue;
        case '\n':
            newline(term);
            continue;
        case '\r':
            term->cursor_x = 0;
            term->wrap_next = false;
            mark(term, 0, term->cursor_y);
            continue;
        case '\b':
            term->wrap_next = false;
            if (term->cursor_x > 0) {
                term->cursor_x--;
            }
            mark(term, term->cursor_x, term->cursor_y);
            continue;
        case '\t': {
            int next = (term->cursor_x / TERM_TAB + 1) * TERM_TAB;

            if (next >= term->cols) {
                next = term->cols - 1;
            }

            while (term->cursor_x < next) {
                put_character(term, ' ');
            }
            continue;
        }
        default:
            break;
        }

        if (byte < 0x20 || byte > 0x7E) {
            /* A control code of a program and not a letter of the screen: a
             * bell, a form feed and the bytes of a binary that reached the
             * window by accident. */
            continue;
        }

        put_character(term, (char)byte);
    }

    return (int)len;
}

bool ui_term_take_damage(struct ui_term *term, int *x, int *y, int *w, int *h)
{
    if (!term_ready(term) || term->dirty_x1 < term->dirty_x0 || term->dirty_y1 < term->dirty_y0) {
        damage_clear(term);
        return false;
    }

    if (x != NULL) {
        *x = term->dirty_x0;
    }
    if (y != NULL) {
        *y = term->dirty_y0;
    }
    if (w != NULL) {
        *w = term->dirty_x1 - term->dirty_x0 + 1;
    }
    if (h != NULL) {
        *h = term->dirty_y1 - term->dirty_y0 + 1;
    }

    damage_clear(term);

    return true;
}
