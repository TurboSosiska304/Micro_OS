/*
 * Micro OS — the console of the x86-64 port: the cells, the cursor and the
 * escape sequences, on whichever screen the machine has.
 *
 * The dialogue of the kernel is text, and a screen of this port is a grid of
 * cells: 80 columns by 25 rows of the text mode, about 128 by 57 of the
 * framebuffer of ADR 0023. What this file holds is the part of the console
 * that does not depend on which grid it is: the position of the cursor, the
 * scroll when the last line is full, the tab stops, the erase of the
 * backspace, the two sequences of the `clear` command and the translation of
 * the one UTF-8 character the kernel writes (the em dash of the banner). The
 * screen is a table of five operations (port_x86_64_console.h), and the two
 * screens of the port - src/vga.c and src/fb.c - put a cell where it is told.
 *
 * The screen itself is chosen at the boot and never changes: the framebuffer
 * when the Bochs VBE interface of the VGA answers (ADR 0023), the text mode of
 * the firmware otherwise. The console does not look at the choice again; the
 * one thing it says about it is the name in the boot's log line.
 *
 * The colours are the `console` system of /etc (ADR 0021): a theme of dark or
 * light and the inversion of the pair. The screen repaints what is already on
 * the glass, so `config set console.theme light` reads the line a person typed
 * again in the new colours instead of erasing it.
 */
#include "port_x86_64_console.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "port_x86_64.h"
#include "port_x86_64_screen.h"
#include "settings/settings.h"

/* The tab stops of the terminal: every eight columns, from the left edge. */
#define CONSOLE_TAB 8

/*
 * The one escape sequence the console of the kernel writes is the pair of the
 * `clear` command of the shell: ESC[2J (erase the screen) and ESC[H (home the
 * cursor). A window is not a terminal and the bytes are not for a terminal
 * emulator, so the port reads the sequences itself: the state below is the
 * whole parser - text, after ESC, inside a CSI until the final byte - and a
 * sequence of any other kind is dropped instead of being printed.
 */
#define CONSOLE_TEXT 0u
#define CONSOLE_ESCAPE 1u
#define CONSOLE_CSI 2u

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

static const struct port_x86_64_screen *s_screen;
static int s_row;
static int s_column;
static k_u8_t s_escape = CONSOLE_TEXT;
static k_u8_t s_utf8 = CONSOLE_UTF8_NONE;

const char *port_x86_64_console_name(void)
{
    return s_screen != NULL ? s_screen->name : "none";
}

static void cursor_update(void)
{
    s_screen->cursor(s_row, s_column);
}

static void screen_clear(void)
{
    s_screen->clear();

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
        s_screen->scroll();
        s_row = s_screen->rows - 1;
    }

    cursor_update();
}

/* One character in the cell of the cursor, then the cursor moves on. */
static void put_char(char character)
{
    s_screen->put(s_row, s_column, character);

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

    s_screen->put(s_row, s_column, ' ');
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
     * The framebuffer first: the interface of the card is programmed, its
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

    screen_clear();
}

void port_x86_64_console_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL || s_screen == NULL || s_screen->columns <= 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        char character = data[i];

        if (s_escape == CONSOLE_ESCAPE) {
            /* The byte after ESC says what kind of a sequence follows; the
             * screen only knows the control sequences of the CSI form. */
            s_escape = (character == '[') ? CONSOLE_CSI : CONSOLE_TEXT;
            continue;
        }

        if (s_escape == CONSOLE_CSI) {
            if (character < '@' || character > '~') {
                continue; /* a parameter or an intermediate byte */
            }

            s_escape = CONSOLE_TEXT;

            if (character == 'J') {
                screen_clear(); /* ESC[2J: the cells and the cursor */
            } else if (character == 'H') {
                s_row = 0;
                s_column = 0;
                cursor_update();
            }
            /* Any other final byte ends the sequence and leaves no trace. */
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

/* --- the console system of /etc (ADR 0021) --- */

/*
 * The one system of /etc this machine has a use for is `console`: `display`
 * and `font` are the panel and the font of the board, and the load names them
 * and keeps the files. The theme and the inversion are the whole of the
 * console, and the screen they were chosen for repaints itself with them.
 */
static void apply_console(const char *system, const struct settings_table *table)
{
    const char *theme = settings_value(table, "theme", "dark");
    bool invert = settings_flag(table, "invert", false);
    bool light;

    K_UNUSED(system);

    if (strcmp(theme, "dark") == 0) {
        light = false;
    } else if (strcmp(theme, "light") == 0) {
        light = true;
    } else {
        kwarn("console: theme '%s' is not dark or light; the default dark is used", theme);
        light = false;
    }

    kinfo("console: theme %s, invert %d, the screen %s", light ? "light" : "dark", invert ? 1 : 0,
          port_x86_64_console_name());

    if (s_screen != NULL) {
        s_screen->colors(light, invert);
    }
}

static const struct settings_system s_console_system = {
    .name = "console",
    .apply = apply_console,
};

int port_x86_64_console_settings_register(void)
{
    return settings_register(&s_console_system);
}
