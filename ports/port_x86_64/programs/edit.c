/*
 * Micro OS — `/bin/edit`, a text editor on the screen (ADR 0030).
 *
 * The program is a document, a cursor and a window. It reads the file named
 * on the command line (a name that is not there is a new file), draws its
 * lines into the window of the screen, moves the cursor with the arrows,
 * inserts the characters of the keyboard and writes the file back on Ctrl+S.
 * Ctrl+Q leaves; with unsaved changes it asks first, in a modal window of the
 * model of `components/ui` that takes the keys until it is answered.
 *
 * Nothing is painted twice. The program keeps the text of every row it has
 * drawn and the cell of the cursor, and a key repaints the rows whose text
 * differs and the cell the cursor left: a line of 1280 pixels is one write of
 * the device, and a key costs one or two of them (ADR 0028: a blit, not a
 * mapping). The title and the line of the status are compared the same way.
 *
 * The document is the bss of the program: 32 KiB of text and the rows of the
 * window. The arena of the library is not linked in - an editor of a machine
 * with 32 MiB of window does not need an allocator.
 */
#include "uiprog.h"

#include <errno.h>
#include <unistd.h>

#include "prog.h"
#include "ui/draw.h"
#include "ui/keyboard.h"
#include "ui/window.h"
#include "ulibc.h"

#define EDIT_PATH_MAX 128
#define EDIT_TEXT_MAX 32768
#define EDIT_ROWS_MAX 64
#define EDIT_COLS_MAX UIPROG_COLS_MAX
#define EDIT_ID_DOC 1
#define EDIT_ID_QUIT 2
#define EDIT_TAB_SPACES 4

/* The colours of the editor: the roles of `ui/palette.h` and not a number of
 * its own, so the body, the bar of the title and the line of status are the
 * three surfaces of the desktop and the inks of the theme. `g_title_ink` is
 * the brightest of the three and `g_status_ink` the second, which is the
 * hierarchy of a window with one font. */
static k_u16_t g_paper;
static k_u16_t g_ink;
static k_u16_t g_border;
static k_u16_t g_focus;
static k_u16_t g_bar;
static k_u16_t g_title_ink;
static k_u16_t g_status_ink;

/* The text of the modal window that asks about an unsaved file. */
static const char *const s_quit_lines[] = {
    "Save with Ctrl+S and leave.",
    "y discards the changes and leaves.",
    "n (or Esc) comes back to the file.",
};

static struct uiprog g_screen;
static struct ui_screen g_model;
static struct ui_keyboard g_keyboard;
static struct ui_style g_style;

static char g_path[EDIT_PATH_MAX];
static char g_text[EDIT_TEXT_MAX];
static int g_length;
static int g_cursor;
static int g_top;
static int g_left;
static bool g_dirty;
static bool g_running = true;

/* The message of the file, shown in the status line until the next key: the
 * answer of a save or the reason a file was not read. */
static char g_message[EDIT_COLS_MAX + 1];

/* What the screen shows: one string a row and the cell of the cursor inside
 * it, so a key paints the rows whose text or cursor moved. */
static char g_shown[EDIT_ROWS_MAX][EDIT_COLS_MAX + 1];
static int g_shown_cursor[EDIT_ROWS_MAX];
static char g_shown_status[EDIT_COLS_MAX + 1];
static char g_shown_title[UI_TITLE_MAX];

static int g_rows;
static int g_cols;
static int g_body_x;
static int g_body_y;
static int g_status_y;
static char g_status[EDIT_COLS_MAX + 1];

/* --- the document --------------------------------------------------------- */

static int line_start(int line)
{
    int offset = 0;

    while (line > 0 && offset < g_length) {
        if (g_text[offset] == '\n') {
            line--;
        }
        offset++;
    }

    return offset;
}

static int line_end(int start)
{
    int offset = start;

    while (offset < g_length && g_text[offset] != '\n') {
        offset++;
    }

    return offset;
}

static int line_of(int offset)
{
    int line = 0;
    int index;

    for (index = 0; index < offset && index < g_length; index++) {
        if (g_text[index] == '\n') {
            line++;
        }
    }

    return line;
}

static int cursor_column(void)
{
    return g_cursor - line_start(line_of(g_cursor));
}

static int line_count(void)
{
    return line_of(g_length) + 1;
}

static void insert_char(char character)
{
    if (g_length + 1 >= EDIT_TEXT_MAX) {
        (void)snprintf(g_message, sizeof(g_message), "the file is full (%d bytes)", EDIT_TEXT_MAX);
        return;
    }

    memmove(g_text + g_cursor + 1, g_text + g_cursor, (size_t)(g_length - g_cursor));
    g_text[g_cursor] = character;
    g_length++;
    g_cursor++;
    g_dirty = true;
}

static void remove_range(int start, int count)
{
    if (start < 0 || count <= 0 || start + count > g_length) {
        return;
    }

    memmove(g_text + start, g_text + start + count, (size_t)(g_length - start - count));
    g_length -= count;
    g_dirty = true;
}

/* The cursor after a run of spaces of Tab: one character is a cell of the
 * screen and a tab is not, so the file gets spaces. */
static void insert_tab(void)
{
    int index;

    for (index = 0; index < EDIT_TAB_SPACES; index++) {
        insert_char(' ');
    }
}

/* The cursor to a line and a column, both clamped to the text: the column of
 * a short line is its end and not the end of the line before it. */
static void cursor_to(int line, int column)
{
    int start;
    int length;

    if (line < 0) {
        line = 0;
    }
    if (line >= line_count()) {
        line = line_count() - 1;
    }

    start = line_start(line);
    length = line_end(start) - start;

    if (column < 0) {
        column = 0;
    }
    if (column > length) {
        column = length;
    }

    g_cursor = start + column;
}

static void move_vertical(int delta)
{
    int line = line_of(g_cursor) + delta;
    int column = cursor_column();

    cursor_to(line, column);
}

/* --- the file ------------------------------------------------------------- */

static void file_message(const char *what, const char *path, int error)
{
    (void)snprintf(g_message, sizeof(g_message), "%s %s: error %d", what, path, error);
}

/*
 * The file into the document: 0 when it was read or is a new one, -1 when it
 * is there and cannot be edited - a file longer than the buffer is refused
 * whole, because an editor that saved the half it read would lose the tail.
 * The status line of a failure is the message of the file.
 */
static int document_load(const char *path)
{
    long size;
    long got;
    int fd;

    g_length = 0;
    g_cursor = 0;
    g_top = 0;
    g_left = 0;
    g_dirty = false;

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            file_message("cannot open", path, errno);
        }
        return 0;
    }

    size = lseek(fd, 0, ULIBC_SEEK_END);
    if (size < 0 || lseek(fd, 0, ULIBC_SEEK_SET) < 0) {
        (void)close(fd);
        file_message("cannot read", path, errno);
        return 0;
    }

    if (size >= EDIT_TEXT_MAX) {
        (void)close(fd);
        (void)snprintf(g_message, sizeof(g_message), "%s is %ld bytes, the editor holds %d", path,
                       size, EDIT_TEXT_MAX);
        return -1;
    }

    got = read(fd, g_text, (size_t)(EDIT_TEXT_MAX - 1));
    (void)close(fd);

    if (got < 0) {
        file_message("cannot read", path, (int)-got);
        return 0;
    }

    g_length = (int)got;
    (void)snprintf(g_message, sizeof(g_message), "%d bytes", g_length);

    return 0;
}

static bool document_save(void)
{
    long written;
    int fd;

    fd = open(g_path, ULIBC_O_WRONLY | ULIBC_O_CREAT | ULIBC_O_TRUNC);
    if (fd < 0) {
        file_message("cannot write", g_path, errno);
        return false;
    }

    written = write(fd, g_text, (size_t)g_length);
    if (written != (long)g_length) {
        file_message("short write to", g_path, (int)(written < 0 ? -written : 0));
        (void)close(fd);
        return false;
    }

    if (close(fd) != 0) {
        file_message("cannot close", g_path, errno);
        return false;
    }

    g_dirty = false;
    (void)snprintf(g_message, sizeof(g_message), "saved %d bytes", g_length);

    return true;
}

/* --- the picture ---------------------------------------------------------- */

/* The text of one line of the file inside the window, cut at the right edge
 * and padded with spaces; the cursor of the screen is the column of the
 * document's cursor when it is this line. */
static void row_text(int line, char *out, int cells, int *cursor_cell)
{
    int start = line_start(line);
    int length = line_end(start) - start;
    int column;

    for (column = 0; column < cells; column++) {
        int index = g_left + column;

        out[column] = index < length ? g_text[start + index] : ' ';
    }

    out[cells] = '\0';

    if (cursor_cell != NULL) {
        *cursor_cell = -1;

        if (line_of(g_cursor) == line) {
            int cursor = cursor_column() - g_left;

            if (cursor >= 0 && cursor < cells) {
                *cursor_cell = cursor;
            }
        }
    }
}

static void row_paint(int row)
{
    char text[EDIT_COLS_MAX + 1];
    int cursor_cell;
    int y = g_body_y + row * FONT8_HEIGHT;

    row_text(g_top + row, text, g_cols, &cursor_cell);

    if (strcmp(text, g_shown[row]) == 0 && g_shown_cursor[row] == cursor_cell) {
        return;
    }

    memcpy(g_shown[row], text, (size_t)g_cols + 1u);
    g_shown_cursor[row] = cursor_cell;

    (void)uiprog_text(&g_screen, g_body_x, y, text, g_cols, g_ink, g_paper);

    /* The cursor is the cell in the two colours traded: the letter, when
     * there is one, is read in the paper of the screen. */
    if (cursor_cell >= 0) {
        (void)uiprog_text_reverse(&g_screen, g_body_x + cursor_cell * FONT8_WIDTH, y,
                                  text + cursor_cell, 1, g_ink, g_paper);
    }
}

static void rows_invalidate(void)
{
    int row;

    for (row = 0; row < g_rows; row++) {
        g_shown[row][0] = '\1';
        g_shown[row][1] = '\0';
        g_shown_cursor[row] = -2;
    }
}

static void rows_paint(void)
{
    int row;

    for (row = 0; row < g_rows; row++) {
        row_paint(row);
    }
}

static void status_build(void)
{
    int line = line_of(g_cursor) + 1;
    int column = cursor_column() + 1;

    if (g_message[0] != '\0') {
        memcpy(g_status, g_message, strlen(g_message) + 1u);
        return;
    }

    (void)snprintf(g_status, sizeof(g_status),
                   "edit%s  L%d C%d  %d bytes  Ctrl+S save  Ctrl+Q quit",
                   g_dirty ? " [modified]" : "", line, column, g_length);
}

static void status_paint(void)
{
    int length;

    if (strcmp(g_status, g_shown_status) == 0) {
        return;
    }

    memcpy(g_shown_status, g_status, strlen(g_status) + 1u);

    (void)uiprog_fill(&g_screen, 0, g_status_y, (int)g_screen.info.width, FONT8_HEIGHT, g_bar);

    length = (int)strlen(g_status);
    if (length > g_cols) {
        length = g_cols;
    }

    (void)uiprog_text(&g_screen, g_body_x, g_status_y, g_status, length, g_status_ink, g_bar);
}

/* The title row of the frame, alone: the frame of the whole window is filed
 * with the pixels of the document, and a title that changed must not clear
 * them. */
static void title_paint(void)
{
    char title[UI_TITLE_MAX];
    struct ui_window *window;
    int index;
    int length;

    index = ui_screen_find(&g_model, EDIT_ID_DOC);
    if (index == UI_WINDOW_NONE) {
        return;
    }

    window = &g_model.windows[index];
    (void)snprintf(title, sizeof(title), "edit - %s%s", g_path, g_dirty ? " *" : "");

    if (strcmp(title, g_shown_title) == 0) {
        return;
    }

    memcpy(g_shown_title, title, strlen(title) + 1u);
    (void)snprintf(window->title, sizeof(window->title), "%s", title);

    /* The title of the window of the desktop as well: the bar of the server
     * and the button of its panel are the same words (docs/desktop.md). */
    (void)uiprog_title(&g_screen, title);

    (void)uiprog_fill(&g_screen, 0, UI_FRAME_MARGIN, (int)g_screen.info.width, FONT8_HEIGHT, g_bar);

    length = (int)strlen(title);
    if (length > g_cols) {
        length = g_cols;
    }

    (void)uiprog_text(&g_screen, g_body_x, UI_FRAME_MARGIN, title, length, g_title_ink, g_bar);
}

/* The cursor is always a cell of the window: the view follows it. */
static void view_follow(void)
{
    int line = line_of(g_cursor);
    int column = cursor_column();

    if (line < g_top) {
        g_top = line;
    }
    if (line >= g_top + g_rows) {
        g_top = line - g_rows + 1;
    }

    if (column < g_left) {
        g_left = column;
    }
    if (column >= g_left + g_cols) {
        g_left = column - g_cols + 1;
    }

    if (g_top < 0) {
        g_top = 0;
    }
    if (g_left < 0) {
        g_left = 0;
    }
}

/* --- the modal window of the unsaved file --------------------------------- */

static void modal_paint(const struct ui_window *window)
{
    int line;

    if (ui_draw_frame(&g_screen.target, window, uiprog_font(), &g_style, true, g_screen.line) !=
        0) {
        return;
    }

    for (line = 0; line < (int)K_ARRAY_SIZE(s_quit_lines); line++) {
        (void)uiprog_text(&g_screen, window->rect.x + UI_FRAME_PAD,
                          window->rect.y + UI_FRAME_TITLE_HEIGHT(uiprog_font()) +
                              line * FONT8_HEIGHT,
                          s_quit_lines[line], (int)strlen(s_quit_lines[line]), g_ink, g_paper);
    }
}

static void modal_open(void)
{
    struct ui_rect bounds;
    struct ui_rect rect;
    int index;

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = (int)g_screen.info.width;
    bounds.h = (int)g_screen.info.height;

    rect.w = 44 * FONT8_WIDTH;
    rect.h = UI_FRAME_TITLE_HEIGHT(uiprog_font()) + (int)K_ARRAY_SIZE(s_quit_lines) * FONT8_HEIGHT;
    rect.x = (bounds.w - rect.w) / 2;
    rect.y = (bounds.h - rect.h) / 2;

    index = ui_screen_add(&g_model, EDIT_ID_QUIT, "unsaved changes", &rect, 0);
    if (index == UI_WINDOW_NONE) {
        return;
    }

    if (ui_screen_set_modal(&g_model, index) == UI_WINDOW_NONE) {
        (void)ui_screen_remove(&g_model, index);
        return;
    }

    modal_paint(&g_model.windows[index]);
}

static void modal_close(void)
{
    int index = ui_screen_find(&g_model, EDIT_ID_QUIT);

    if (index == UI_WINDOW_NONE) {
        return;
    }

    ui_screen_clear_modal(&g_model);
    (void)ui_screen_remove(&g_model, index);

    /* The picture under the dialog is the document, and the document is
     * painted again from its text. */
    (void)ui_draw_frame(&g_screen.target, &g_model.windows[0], uiprog_font(), &g_style, true,
                        g_screen.line);
    title_paint();
    rows_invalidate();
    rows_paint();
    g_shown_status[0] = '\1';
    status_paint();
}

/* --- the keys ------------------------------------------------------------- */

static void request_quit(void)
{
    if (!g_dirty) {
        g_running = false;
        return;
    }

    if (ui_screen_find(&g_model, EDIT_ID_QUIT) == UI_WINDOW_NONE) {
        modal_open();
    }
}

static void modal_key(k_u16_t code)
{
    if (code == K_INPUT_KEY_Y) {
        g_running = false;
        return;
    }

    if (code == K_INPUT_KEY_N || code == K_INPUT_KEY_ESC) {
        modal_close();
        return;
    }

    if (g_keyboard.ctrl && code == K_INPUT_KEY_S) {
        if (document_save()) {
            g_running = false;
        }
        return;
    }
}

static void key_handle(k_u16_t code)
{
    char character = 0;

    if (ui_screen_find(&g_model, EDIT_ID_QUIT) != UI_WINDOW_NONE) {
        modal_key(code);
        return;
    }

    if (g_keyboard.ctrl) {
        if (code == K_INPUT_KEY_S) {
            (void)document_save();
            status_build();
            status_paint();
            title_paint();
            return;
        }
        if (code == K_INPUT_KEY_Q) {
            request_quit();
        }
        return;
    }

    /* A key of the document: the message of the last file operation is the
     * state of the file before this key and not after it. */
    g_message[0] = '\0';

    switch (code) {
    case K_INPUT_KEY_UP:
        move_vertical(-1);
        break;
    case K_INPUT_KEY_DOWN:
        move_vertical(1);
        break;
    case K_INPUT_KEY_LEFT:
        if (g_cursor > 0) {
            g_cursor--;
        }
        break;
    case K_INPUT_KEY_RIGHT:
        if (g_cursor < g_length) {
            g_cursor++;
        }
        break;
    case K_INPUT_KEY_HOME:
        g_cursor = line_start(line_of(g_cursor));
        break;
    case K_INPUT_KEY_END:
        g_cursor = line_end(line_start(line_of(g_cursor)));
        break;
    case K_INPUT_KEY_PAGEUP:
        move_vertical(-(g_rows - 1));
        break;
    case K_INPUT_KEY_PAGEDOWN:
        move_vertical(g_rows - 1);
        break;
    case K_INPUT_KEY_ENTER:
        insert_char('\n');
        break;
    case K_INPUT_KEY_BACKSPACE:
        if (g_cursor > 0) {
            remove_range(g_cursor - 1, 1);
            g_cursor--;
        }
        break;
    case K_INPUT_KEY_DELETE:
        remove_range(g_cursor, 1);
        break;
    case K_INPUT_KEY_TAB:
        insert_tab();
        break;
    case K_INPUT_KEY_ESC:
        request_quit();
        return;
    default:
        character = ui_keyboard_char(&g_keyboard, code);
        if (character >= 0x20 && character < 0x7f) {
            insert_char(character);
        }
        break;
    }

    view_follow();
    status_build();
    rows_paint();
    status_paint();
    title_paint();
}

/* --- the program ---------------------------------------------------------- */

static void layout(void)
{
    const struct ui_font *font = uiprog_font();

    g_body_x = UI_FRAME_PAD;
    g_body_y = UI_FRAME_TITLE_HEIGHT(font);
    g_status_y = (int)g_screen.info.height - FONT8_HEIGHT;
    g_cols = ((int)g_screen.info.width - 2 * g_body_x) / FONT8_WIDTH;
    g_rows = (g_status_y - g_body_y) / FONT8_HEIGHT;

    if (g_cols > EDIT_COLS_MAX) {
        g_cols = EDIT_COLS_MAX;
    }
    if (g_rows > EDIT_ROWS_MAX) {
        g_rows = EDIT_ROWS_MAX;
    }
    if (g_rows < 1) {
        g_rows = 1;
    }
    if (g_cols < 1) {
        g_cols = 1;
    }
}

int main(int argc, char *const argv[], char *const envp[])
{
    struct ui_rect bounds;
    struct ui_rect window;
    int index;

    (void)envp;

    if (argc < 2) {
        static const char usage[] = "usage: edit <file>\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    if (strlen(argv[1]) >= sizeof(g_path)) {
        static const char long_name[] = "edit: the name of the file is too long\n";

        (void)write(ULIBC_STDERR, long_name, sizeof(long_name) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    (void)snprintf(g_path, sizeof(g_path), "%s", argv[1]);

    if (document_load(g_path) != 0) {
        char line[160];
        int length = snprintf(line, sizeof(line), "edit: %s\n", g_message);

        if (length > 0) {
            (void)write(ULIBC_STDERR, line, (size_t)length);
        }
        return ULIBC_EXIT_FAILURE;
    }

    if (uiprog_open(&g_screen, "edit") != 0) {
        return ULIBC_EXIT_FAILURE;
    }

    ui_keyboard_init(&g_keyboard);
    layout();

    {
        struct ui_palette palette;

        (void)uiprog_palette(&palette);

        g_paper = palette.surface;
        g_ink = palette.ink;
        g_border = palette.border;
        g_focus = palette.accent;
        g_bar = palette.surface_high;
        g_title_ink = palette.title_ink;
        g_status_ink = palette.ink_dim;
    }

    g_style.paper = g_paper;
    g_style.ink = g_ink;
    g_style.border = g_border;
    g_style.focus_border = g_focus;
    g_style.title_paper = g_bar;
    g_style.title_ink = g_title_ink;
    g_style.separator = g_border;

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = (int)g_screen.info.width;
    bounds.h = (int)g_screen.info.height;
    ui_screen_init(&g_model, &bounds);

    window = bounds;
    index = ui_screen_add(&g_model, EDIT_ID_DOC, "edit", &window, 0);
    if (index == UI_WINDOW_NONE) {
        static const char no_window[] = "edit: the picture is too small for a window\n";

        (void)write(ULIBC_STDERR, no_window, sizeof(no_window) - 1);
        uiprog_close(&g_screen);
        return ULIBC_EXIT_FAILURE;
    }

    view_follow();
    status_build();

    (void)ui_draw_frame(&g_screen.target, &g_model.windows[0], uiprog_font(), &g_style, true,
                        g_screen.line);
    title_paint();
    status_paint();
    rows_paint();

    while (g_running) {
        struct input_event event;

        while (uiprog_event(&g_screen, &event) == 1) {
            if (ui_keyboard_event(&g_keyboard, &event)) {
                continue;
            }

            if (event.value != INPUT_VALUE_PRESS) {
                continue;
            }

            key_handle(event.code);
        }

        /* The server of the desktop asked the window to leave (Alt+F4 or the
         * square of the bar): it asks its own question about an unsaved file
         * and leaves when the answer is yes. */
        if (uiprog_close_requested(&g_screen)) {
            request_quit();
        }

        /* One frame of a client: the damage of the keys since the last turn
         * and nothing else. A program of the whole screen has nothing to
         * flush - the device was written as it drew. */
        (void)uiprog_flush(&g_screen);

        /* A silent keyboard is EAGAIN and a nap: the loop of every program
         * of the screen of ADR 0028. */
        (void)usleep(30000u);
    }

    uiprog_close(&g_screen);

    return ULIBC_EXIT_SUCCESS;
}
