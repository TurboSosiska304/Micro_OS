/*
 * Micro OS — the console of the x86-64 port: the cells, the cursor and the
 * escape sequences, on whichever screen the machine has.
 *
 * The dialogue of the kernel is text, and a screen of this port is a grid of
 * cells: 80 columns by 25 rows of the text mode, 160 by 50 of the
 * framebuffer of ADR 0023. What this file holds is the part of the console
 * that does not depend on which grid it is: the position of the cursor, the
 * scroll when the last line is full, the tab stops, the erase of the
 * backspace, the two sequences of the `clear` command, the translation of
 * the one UTF-8 character the kernel writes (the em dash of the banner) and
 * the SGR sequences that choose the colours of the text. The screen is a
 * table of operations (port_x86_64_console.h), and the two screens of the
 * port - src/vga.c and src/fb.c - put a cell where it is told.
 *
 * The screen itself is chosen at the boot and never changes: the framebuffer
 * when the Bochs VBE interface of the VGA answers (ADR 0023), the text mode of
 * the firmware otherwise. The console does not look at the choice again; the
 * one thing it says about it is the name in the boot's log line.
 *
 * The colours are the `console` system of /etc (ADR 0021) for the plain text
 * - a theme of dark or light, an inversion, and an ink and a paper a person
 * may name - and the SGR sequences of a terminal (kernel/ansi.h) for a word a
 * command colours: a directory of `ls`, a warning, an error. The screen
 * repaints what is already on the glass, so `config set console.theme light`
 * reads the line a person typed again in the new colours instead of erasing
 * it, and a word with a colour of its own keeps it.
 */
#include "port_x86_64_console.h"

#include <stdio.h>
#include <string.h>

#include "kernel/ansi.h"
#include "kernel/log.h"
#include "port_x86_64.h"
#include "port_x86_64_screen.h"
#include "settings/settings.h"

/* The tab stops of the terminal: every eight columns, from the left edge. */
#define CONSOLE_TAB 8

/*
 * The escape sequences the console reads: the pair of the `clear` command of
 * the shell (ESC[2J, ESC[H) and the SGR sequences of the colours
 * (ESC[...m, kernel/ansi.h). A window is not a terminal and the bytes are not
 * for a terminal emulator, so the port reads the sequences itself: the state
 * below is the whole parser - text, after ESC, inside a CSI until the final
 * byte - and a sequence of any other kind is dropped instead of being
 * printed.
 */
#define CONSOLE_TEXT 0u
#define CONSOLE_ESCAPE 1u
#define CONSOLE_CSI 2u

/* The room of the parameter list of one CSI sequence: the longest SGR line a
 * console of this tree writes is a handful of numbers. */
#define CONSOLE_PARAMS_MAX 16

/*
 * The text of the kernel is UTF-8 and a cell of the screen is one character.
 * The one character above 0x7F the kernel writes is the em dash of the banner
 * ("board: ... — ..."), and its ASCII form is the hyphen the documentation
 * uses; a byte of any other sequence is dropped, because a cell holds a
 * letter and not a piece of one.
 */
#define CONSOLE_UTF8_NONE 0u
#define CONSOLE_UTF8_E2 1u
#define CONSOLE_UTF8_E2_80 2u

/*
 * The pair of the console before /etc is read: light grey on black, the pair
 * a VGA starts in. The applier of `console` replaces it with the pair of the
 * file before the first command.
 */
#define CONSOLE_BOOT_FG 7u
#define CONSOLE_BOOT_BG 0u

static const struct port_x86_64_screen *s_screen;
static int s_row;
static int s_column;
static k_u8_t s_escape = CONSOLE_TEXT;
static k_u8_t s_utf8 = CONSOLE_UTF8_NONE;

/* The state of the SGR sequences and the pair the plain text is written in. */
static struct k_ansi s_ansi;
static k_u8_t s_plain_fg = CONSOLE_BOOT_FG;
static k_u8_t s_plain_bg = CONSOLE_BOOT_BG;

/* The parameter list of the CSI sequence being read. */
static char s_params[CONSOLE_PARAMS_MAX];
static k_size_t s_params_len;

const char *port_x86_64_console_name(void)
{
    return s_screen != NULL ? s_screen->name : "none";
}

/* The attribute of a letter written now: the SGR state over the pair of the
 * file, the byte every screen of the port understands. */
static k_u8_t pen_attr(void)
{
    return k_ansi_attr(&s_ansi, s_plain_fg, s_plain_bg);
}

static void cursor_update(void)
{
    s_screen->cursor(s_row, s_column);
}

static void screen_clear(void)
{
    s_screen->clear(pen_attr());

    s_row = 0;
    s_column = 0;

    cursor_update();
}

static void next_line(void)
{
    s_column = 0;
    s_row++;

    if (s_row >= s_screen->rows) {
        /* The lines above the last one are gone: the screen scrolls up by a
         * row, which is what a terminal does when its bottom is reached. */
        s_screen->scroll(pen_attr());
        s_row = s_screen->rows - 1;
    }

    cursor_update();
}

/* One character in the cell of the cursor, then the cursor moves on. */
static void put_char(char character)
{
    s_screen->put(s_row, s_column, character, pen_attr());

    s_column++;
    if (s_column >= s_screen->columns) {
        next_line();
        return;
    }

    cursor_update();
}

static void backspace(void)
{
    if (s_column > 0) {
        s_column--;
    } else if (s_row > 0) {
        /* The first column of a line: the character before it is the last one
         * of the line above, which is where a terminal puts the cursor. */
        s_row--;
        s_column = s_screen->columns - 1;
    } else {
        return;
    }

    s_screen->put(s_row, s_column, ' ', pen_attr());
    cursor_update();
}

/*
 * The window of a device is mapped in 2 MiB pages, so the mode's address is
 * taken to the page it starts in and the offset is added back to the virtual
 * address. A BAR is aligned that way on every card the port has met, and the
 * arithmetic keeps the code honest if one is not.
 */
#define CONSOLE_PAGE_MASK 0x1FFFFFull

void port_x86_64_console_init(void)
{
    struct port_x86_64_screen_mode mode;

    /*
     * The text memory of the firmware is emptied first: those cells live at
     * the offset 0x18000 of the card's memory, which is inside the window of
     * the framebuffer, and nothing of the screen of another program belongs
     * in the picture of this one. On a machine without the interface this is
     * also the blank screen the fallback console starts from.
     */
    port_x86_64_screen_text.clear(PORT_X86_64_ATTR(CONSOLE_BOOT_FG, CONSOLE_BOOT_BG));

    /*
     * The framebuffer next: the interface of the card is programmed, its
     * window is mapped into the top 2 GiB (ADR 0007) and the renderer of the
     * kernel is registered over it. A machine whose interface did not answer
     * keeps the text mode the firmware left. The one failure between the two
     * - the mode was set and the window did not map - keeps the text screen
     * as well, although the card is already in a graphics mode and the cells
     * of 0xB8000 are then drawn nowhere; the log names it and COM1 still
     * carries the dialogue (debts.md, K11).
     */
    s_screen = NULL;

    if (port_x86_64_vbe_mode(&mode)) {
        k_u64_t offset = (k_u64_t)mode.phys & CONSOLE_PAGE_MASK;
        k_u64_t window = (k_u64_t)mode.phys - offset;

        if (port_x86_64_paging_map_device(PORT_X86_64_SCREEN_VIRT, window,
                                          (k_size_t)mode.bytes + (k_size_t)offset) > 0) {
            s_screen =
                port_x86_64_fb_screen(&mode, (void *)(uintptr_t)(PORT_X86_64_SCREEN_VIRT + offset));
        } else {
            kerror("screen: the window of the framebuffer cannot be mapped");
        }
    }

    if (s_screen == NULL) {
        s_screen = &port_x86_64_screen_text;
    }

    s_escape = CONSOLE_TEXT;
    s_utf8 = CONSOLE_UTF8_NONE;
    s_params_len = 0;
    k_ansi_reset(&s_ansi);

    screen_clear();
}

/*
 * The grid of the screen can become smaller than the position the console
 * holds: `font.line_gap` of /etc/font takes rows away from the picture (50
 * rows of 16 pixels are 44 of 18), and the cells above the new bottom are
 * gone. The position is brought back inside the grid before the next byte is
 * written, so the prompt of a shell that was at the last line is not written
 * into a row the screen no longer has.
 */
static void clamp_position(void)
{
    if (s_row >= s_screen->rows) {
        s_row = s_screen->rows - 1;
    }
    if (s_row < 0) {
        s_row = 0;
    }
    if (s_column >= s_screen->columns) {
        s_column = s_screen->columns - 1;
    }
    if (s_column < 0) {
        s_column = 0;
    }
}

/* One byte of the parameter list of a CSI sequence, bounded: a list longer
 * than the buffer is cut at a parameter boundary rather than overrun. */
static void params_add(char character)
{
    if (s_params_len + 1 < sizeof(s_params)) {
        s_params[s_params_len++] = character;
    }
}

/* The final byte of a CSI sequence: the sequences this system understands,
 * and a silent end for every other one. */
static void csi_final(char character)
{
    s_escape = CONSOLE_TEXT;

    if (character == 'm') {
        (void)k_ansi_sgr_text(&s_ansi, s_params, s_params_len);
    } else if (character == 'J') {
        screen_clear(); /* ESC[2J: the cells and the cursor */
    } else if (character == 'H') {
        s_row = 0;
        s_column = 0;
        cursor_update();
    }
    /* Any other final byte ends the sequence and leaves no trace. */

    s_params_len = 0;
}

void port_x86_64_console_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL || s_screen == NULL || s_screen->columns <= 0) {
        return;
    }

    clamp_position();

    for (i = 0; i < len; i++) {
        char character = data[i];

        if (s_escape == CONSOLE_ESCAPE) {
            /* The byte after ESC says what kind of a sequence follows; the
             * screen only knows the control sequences of the CSI form. */
            s_escape = (character == '[') ? CONSOLE_CSI : CONSOLE_TEXT;
            s_params_len = 0;
            continue;
        }

        if (s_escape == CONSOLE_CSI) {
            if (character >= '0' && character <= '9') {
                params_add(character);
                continue;
            }
            if (character == ';') {
                params_add(character);
                continue;
            }
            if (character < '@' || character > '~') {
                continue; /* an intermediate byte */
            }

            csi_final(character);
            continue;
        }

        if (character == 0x1B) {
            s_escape = CONSOLE_ESCAPE;
            continue;
        }

        if (s_utf8 == CONSOLE_UTF8_E2) {
            s_utf8 = ((k_u8_t)character == 0x80u) ? CONSOLE_UTF8_E2_80 : CONSOLE_UTF8_NONE;
            continue;
        }

        if (s_utf8 == CONSOLE_UTF8_E2_80) {
            /* E2 80 94 is the em dash; the other characters of the family
             * (the quotes, the ellipsis) are not ones the kernel writes, and
             * the cell has no glyph for them. */
            if ((k_u8_t)character == 0x94u) {
                put_char('-');
            }
            s_utf8 = CONSOLE_UTF8_NONE;
            continue;
        }

        if ((k_u8_t)character == 0xE2u) {
            s_utf8 = CONSOLE_UTF8_E2;
            continue;
        }

        switch (character) {
        case '\n':
            next_line();
            break;
        case '\r':
            s_column = 0;
            cursor_update();
            break;
        case '\t': {
            /* To the next stop, not by eight: a tab at the column 3 of the
             * line lands on the column 8 of the same line. The stops are the
             * ones of the terminal, so the text of a file lines up the same
             * way it does on the serial console. */
            int stop = (s_column / CONSOLE_TAB + 1) * CONSOLE_TAB;

            while (s_column < stop) {
                put_char(' ');
            }
            break;
        }
        case '\b':
            backspace();
            break;
        default:
            if ((k_u8_t)character < 0x20u || (k_u8_t)character >= 0x7Fu) {
                break; /* a control character or a byte no cell has a letter for */
            }
            put_char(character);
            break;
        }
    }
}

void port_x86_64_console_redraw(void)
{
    if (s_screen != NULL && s_screen->redraw != NULL) {
        s_screen->redraw();
    }
}

/* --- the console system of /etc (ADR 0021) --- */

/*
 * The names a person may write in `ink` and `paper` instead of a number of
 * 0xRRRR. They are the colours of the sixteen attributes a person recognises
 * and not the whole palette: "amber" is the brown of the CGA, which is the
 * colour a warning is read in, and "yellow" its bright form.
 */
static const struct color_name {
    const char *name;
    k_u8_t index;
} s_color_names[] = {
    {"black", 0},   {"blue", 1},  {"green", 2}, {"cyan", 3},    {"red", 4},
    {"magenta", 5}, {"amber", 6}, {"grey", 7},  {"yellow", 14}, {"white", 15},
};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

/*
 * A colour of the file: a name of the table above, or a number of RGB565
 * written `0xRRRR`. The number reaches the framebuffer as it is and the text
 * mode as the nearest of its sixteen colours. False for a word that is
 * neither, and the caller keeps the pair of the theme.
 */
static bool color_parse(const char *text, k_u16_t *rgb, k_u8_t *index)
{
    k_size_t i;

    if (text != NULL && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        k_u16_t value = 0;

        for (i = 2; i < 6; i++) {
            int digit = hex_digit(text[i]);

            if (digit < 0) {
                return false;
            }
            value = (k_u16_t)((value << 4) | (k_u16_t)digit);
        }
        if (text[6] != '\0') {
            return false;
        }

        *rgb = value;
        *index = port_x86_64_color_nearest(value);

        return true;
    }

    for (i = 0; i < K_ARRAY_SIZE(s_color_names); i++) {
        if (strcmp(text, s_color_names[i].name) == 0) {
            *index = s_color_names[i].index;
            *rgb = port_x86_64_palette[s_color_names[i].index];
            return true;
        }
    }

    return false;
}

/*
 * The console system of /etc this machine has a use for: `theme` is the pair
 * of the plain text, `invert` trades it, and `ink` and `paper` name a colour
 * of their own - the word `theme` means the pair the theme chose. A `display`
 * or a `font` is the panel and the font of the board, and the load names them
 * and keeps the files.
 *
 * The keys are read in that order, so an explicit `ink` or `paper` wins over
 * the theme and the inversion - a person who named a colour meant it. A value
 * that is neither a name nor 0xRRRR is the default of the key and a warning:
 * a typo in /etc must not leave a screen nobody can read.
 */
static void apply_console(const char *system, const struct settings_table *table)
{
    const char *theme = settings_value(table, "theme", "dark");
    const char *ink = settings_value(table, "ink", "theme");
    const char *paper = settings_value(table, "paper", "theme");
    bool invert = settings_flag(table, "invert", false);
    bool light;
    struct port_x86_64_pair pair;
    k_u16_t rgb;
    k_u8_t index;

    K_UNUSED(system);

    if (strcmp(theme, "dark") == 0) {
        light = false;
    } else if (strcmp(theme, "light") == 0) {
        light = true;
    } else {
        kwarn("console: theme '%s' is not dark or light; the default dark is used", theme);
        light = false;
    }

    /* The pair of the theme: light grey on black, or the two traded. */
    pair.ink = light ? 0x0000u : 0xAD55u;
    pair.paper = light ? 0xAD55u : 0x0000u;
    pair.fg = light ? 0u : 7u;
    pair.bg = light ? 7u : 0u;

    if (invert) {
        k_u16_t swap = pair.ink;
        k_u8_t swap_index = pair.fg;

        pair.ink = pair.paper;
        pair.paper = swap;
        pair.fg = pair.bg;
        pair.bg = swap_index;
    }

    if (strcmp(ink, "theme") != 0) {
        if (color_parse(ink, &rgb, &index)) {
            pair.ink = rgb;
            pair.fg = index;
        } else {
            kwarn("console: ink '%s' is not a colour name or 0xRRRR; the theme is used", ink);
        }
    }

    if (strcmp(paper, "theme") != 0) {
        if (color_parse(paper, &rgb, &index)) {
            pair.paper = rgb;
            pair.bg = index;
        } else {
            kwarn("console: paper '%s' is not a colour name or 0xRRRR; the theme is used", paper);
        }
    }

    /*
     * The line is written in the pair that is still on the glass and the
     * screen repaints the whole grid after it, so the sentence that names the
     * theme is read in the colours it names. The pair of the plain text
     * changes after that repaint: a letter written in the new pair before it
     * would be taken for the reverse of the old one when the two are the same
     * two colours traded (the dark and the light theme are exactly that).
     */
    kinfo("console: theme %s, invert %d, the screen %s", light ? "light" : "dark", invert ? 1 : 0,
          port_x86_64_console_name());

    if (s_screen != NULL) {
        s_screen->colors(&pair);
    }

    s_plain_fg = pair.fg;
    s_plain_bg = pair.bg;
}

static const struct settings_system s_console_system = {
    .name = "console",
    .apply = apply_console,
};

int port_x86_64_console_settings_register(void)
{
    return settings_register(&s_console_system);
}
