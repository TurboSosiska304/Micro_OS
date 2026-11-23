/*
 * Micro OS — `/bin/fm`, the files of a directory on the screen (ADR 0030).
 *
 * The program lists the directory of the command line (the working directory
 * when there is none), one line a name, the directories marked with a slash.
 * The arrows move the selection, Enter opens a directory or hands a file to
 * `/bin/edit`, `d` (or Delete) removes the object after asking in a modal
 * window of the model of `components/ui`, and `q` returns to the shell.
 *
 * The list is read with `readdir` of the library, which is the call of the
 * kernel for a directory and not a second walker of the filesystem. A name
 * that cannot be removed is a message of the status line and not an end of
 * the program: a file manager of a machine with one permission bit forgotten
 * still lists what is there.
 *
 * The picture is the same rule as the editor's: a row is compared with the
 * row that is on the screen and only the rows whose name or selection moved
 * are painted, so a key of the arrows costs two rectangles and not a frame.
 */
#include "uiprog.h"

#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "prog.h"
#include "ui/draw.h"
#include "ui/keyboard.h"
#include "ui/window.h"
#include "ulibc.h"

#define FM_PATH_MAX 192
#define FM_ENTRIES_MAX 128
#define FM_ROWS_MAX 64
#define FM_COLS_MAX UIPROG_COLS_MAX
#define FM_ID_MAIN 1
#define FM_ID_DELETE 2

/* The colours of the file manager: the roles of `ui/palette.h`, as the editor
 * has them, and a row of a list that has the focus in the selection of the
 * theme. The status line is the dim ink of the palette and the title the
 * bright one, so the two bars of the window do not read as one. */
static k_u16_t g_paper;
static k_u16_t g_ink;
static k_u16_t g_select_paper;
static k_u16_t g_select_ink;
static k_u16_t g_border;
static k_u16_t g_focus;
static k_u16_t g_bar;
static k_u16_t g_title_ink;
static k_u16_t g_status_ink;

struct fm_entry {
    char name[VFS_NAME_MAX + 1];
    k_u8_t type;
};

static struct uiprog g_screen;
static struct ui_screen g_model;
static struct ui_keyboard g_keyboard;
static struct ui_style g_style;

static struct fm_entry g_entries[FM_ENTRIES_MAX];
static int g_count;
static int g_cursor;
static int g_top;

static char g_path[FM_PATH_MAX];
static char g_message[FM_COLS_MAX + 1];
static char g_status[FM_COLS_MAX + 1];

static char g_shown[FM_ROWS_MAX][FM_COLS_MAX + 1];
static int g_shown_selected[FM_ROWS_MAX];
static char g_shown_status[FM_COLS_MAX + 1];
static char g_shown_title[UI_TITLE_MAX];

static int g_rows;
static int g_cols;
static int g_body_x;
static int g_body_y;
static int g_status_y;
static bool g_running = true;

/* --- the path ------------------------------------------------------------- */

static bool is_directory(const struct fm_entry *entry)
{
    return entry->type == DT_DIR;
}

/* `path` and `name` as one absolute path, the root without a doubled slash. */
static void path_join(char *out, size_t size, const char *path, const char *name)
{
    size_t length = strlen(path);

    if (length > 0 && path[length - 1] == '/') {
        (void)snprintf(out, size, "%s%s", path, name);
    } else {
        (void)snprintf(out, size, "%s/%s", path, name);
    }
}

/* The directory above `path`; the root is its own parent. */
static void path_parent(char *out, size_t size, const char *path)
{
    size_t length = strlen(path);

    while (length > 1 && path[length - 1] == '/') {
        length--;
    }

    while (length > 0 && path[length - 1] != '/') {
        length--;
    }

    if (length == 0) {
        (void)snprintf(out, size, "/");
        return;
    }

    while (length > 1 && path[length - 1] == '/') {
        length--;
    }

    (void)snprintf(out, size, "%.*s", (int)length, path);
}

/* --- the directory -------------------------------------------------------- */

static void entry_copy(struct fm_entry *entry, const struct dirent *source)
{
    size_t length = strlen(source->d_name);

    if (length >= sizeof(entry->name)) {
        length = sizeof(entry->name) - 1u;
    }

    memcpy(entry->name, source->d_name, length);
    entry->name[length] = '\0';
    entry->type = source->d_type == DT_DIR ? DT_DIR : DT_REG;
}

/* The list by name: an insertion sort of at most 128 names, and the dot
 * entries first because `.` and `..` sort there by themselves. */
static void entries_sort(void)
{
    int index;

    for (index = 1; index < g_count; index++) {
        struct fm_entry entry = g_entries[index];
        int slot = index;

        while (slot > 0 && strcmp(g_entries[slot - 1].name, entry.name) > 0) {
            g_entries[slot] = g_entries[slot - 1];
            slot--;
        }

        g_entries[slot] = entry;
    }
}

static int dir_load(const char *path)
{
    struct dirent *entry;
    DIR *dir;

    dir = opendir(path);
    if (dir == NULL) {
        (void)snprintf(g_message, sizeof(g_message), "cannot open %s: error %d", path, errno);
        return -1;
    }

    g_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (g_count >= FM_ENTRIES_MAX) {
            (void)snprintf(g_message, sizeof(g_message), "the list is cut at %d names",
                           FM_ENTRIES_MAX);
            break;
        }

        entry_copy(&g_entries[g_count], entry);
        g_count++;
    }

    (void)closedir(dir);
    entries_sort();

    (void)snprintf(g_path, sizeof(g_path), "%s", path);
    g_cursor = 0;
    g_top = 0;

    return 0;
}

/* --- the picture ---------------------------------------------------------- */

static void row_text(int row, char *out, int cells, bool *selected)
{
    int index = g_top + row;

    *selected = index == g_cursor;

    if (index >= g_count) {
        memset(out, ' ', (size_t)cells);
        out[cells] = '\0';
        return;
    }

    (void)snprintf(out, (size_t)cells + 1u, "%s%s", g_entries[index].name,
                   is_directory(&g_entries[index]) ? "/" : "");
}

static void row_paint(int row)
{
    char text[FM_COLS_MAX + 1];
    bool selected;
    int y = g_body_y + row * FONT8_HEIGHT;

    row_text(row, text, g_cols, &selected);

    if (strcmp(text, g_shown[row]) == 0 && g_shown_selected[row] == (int)selected) {
        return;
    }

    memcpy(g_shown[row], text, strlen(text) + 1u);
    g_shown_selected[row] = (int)selected;

    if (selected) {
        (void)uiprog_text(&g_screen, g_body_x, y, text, g_cols, g_select_ink, g_select_paper);
    } else {
        (void)uiprog_text(&g_screen, g_body_x, y, text, g_cols, g_ink, g_paper);
    }
}

static void rows_invalidate(void)
{
    int row;

    for (row = 0; row < g_rows; row++) {
        g_shown[row][0] = '\1';
        g_shown[row][1] = '\0';
        g_shown_selected[row] = -1;
    }
}

static void rows_paint(void)
{
    int row;

    for (row = 0; row < g_rows; row++) {
        row_paint(row);
    }
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

static void status_build(void)
{
    if (g_message[0] != '\0') {
        memcpy(g_status, g_message, strlen(g_message) + 1u);
        return;
    }

    (void)snprintf(g_status, sizeof(g_status), "fm %s  %d entries  Enter open  d delete  q quit",
                   g_path, g_count);
}

static void title_paint(void)
{
    char title[UI_TITLE_MAX];
    struct ui_window *window;
    int index;
    int length;

    index = ui_screen_find(&g_model, FM_ID_MAIN);
    if (index == UI_WINDOW_NONE) {
        return;
    }

    window = &g_model.windows[index];
    (void)snprintf(title, sizeof(title), "fm - %s", g_path);

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

static void view_follow(void)
{
    if (g_cursor < g_top) {
        g_top = g_cursor;
    }
    if (g_cursor >= g_top + g_rows) {
        g_top = g_cursor - g_rows + 1;
    }

    if (g_top < 0) {
        g_top = 0;
    }
}

/* --- the modal window of the delete --------------------------------------- */

static void modal_paint(const struct ui_window *window)
{
    char text[FM_COLS_MAX + 1];
    const char *name;

    if (ui_draw_frame(&g_screen.target, window, uiprog_font(), &g_style, true, g_screen.line) !=
        0) {
        return;
    }

    name = g_cursor >= 0 && g_cursor < g_count ? g_entries[g_cursor].name : "?";

    (void)snprintf(text, sizeof(text), "Delete %s%s?", name,
                   g_cursor >= 0 && g_cursor < g_count && is_directory(&g_entries[g_cursor]) ? "/"
                                                                                             : "");

    (void)uiprog_text(&g_screen, window->rect.x + UI_FRAME_PAD,
                      window->rect.y + UI_FRAME_TITLE_HEIGHT(uiprog_font()), text,
                      (int)strlen(text), g_ink, g_paper);
    (void)uiprog_text(&g_screen, window->rect.x + UI_FRAME_PAD,
                      window->rect.y + UI_FRAME_TITLE_HEIGHT(uiprog_font()) + FONT8_HEIGHT,
                      "y deletes it, n (or Esc) keeps it.", 34, g_ink, g_paper);
}

static void modal_open(void)
{
    struct ui_rect bounds;
    struct ui_rect rect;
    int index;

    if (g_count == 0) {
        return;
    }

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = (int)g_screen.info.width;
    bounds.h = (int)g_screen.info.height;

    rect.w = 56 * FONT8_WIDTH;
    rect.h = UI_FRAME_TITLE_HEIGHT(uiprog_font()) + 2 * FONT8_HEIGHT + UI_FRAME_MARGIN;
    rect.x = (bounds.w - rect.w) / 2;
    rect.y = (bounds.h - rect.h) / 2;

    index = ui_screen_add(&g_model, FM_ID_DELETE, "delete", &rect, 0);
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
    int index = ui_screen_find(&g_model, FM_ID_DELETE);

    if (index == UI_WINDOW_NONE) {
        return;
    }

    ui_screen_clear_modal(&g_model);
    (void)ui_screen_remove(&g_model, index);

    (void)ui_draw_frame(&g_screen.target, &g_model.windows[0], uiprog_font(), &g_style, true,
                        g_screen.line);
    title_paint();
    rows_invalidate();
    rows_paint();
    g_shown_status[0] = '\1';
    status_paint();
}

/* --- the operations ------------------------------------------------------- */

static void directory_open(const char *path)
{
    if (dir_load(path) != 0) {
        /* The status line says why the list did not move; the list itself is
         * still the one of the directory that was open. */
        status_build();
        g_shown_status[0] = '\1';
        status_paint();
        return;
    }

    g_shown_title[0] = '\0';
    g_shown_status[0] = '\1';
    rows_invalidate();
    title_paint();
    status_build();
    status_paint();
    rows_paint();
}

static void delete_answer(void)
{
    char path[FM_PATH_MAX];
    int rc;

    if (g_cursor < 0 || g_cursor >= g_count) {
        modal_close();
        return;
    }

    path_join(path, sizeof(path), g_path, g_entries[g_cursor].name);
    rc = unlink(path);

    if (rc < 0) {
        (void)snprintf(g_message, sizeof(g_message), "cannot delete %s: error %d", path, -rc);
        modal_close();
        return;
    }

    (void)snprintf(g_message, sizeof(g_message), "deleted %s", path);
    modal_close();

    /* The list is read again: the program does not patch a directory it
     * cannot see whole. */
    directory_open(g_path);
}

static void file_open(void)
{
    char path[FM_PATH_MAX];
    char *argv[3];
    int rc;

    path_join(path, sizeof(path), g_path, g_entries[g_cursor].name);

    argv[0] = (char *)"/bin/edit";
    argv[1] = path;
    argv[2] = NULL;

    /* The environment travels: a window of the desktop hands the next program
     * the descriptor of the protocol it was started with (docs/desktop.md). */
    rc = execve("/bin/edit", argv, environ);
    if (rc < 0) {
        (void)snprintf(g_message, sizeof(g_message), "cannot run /bin/edit: error %d", -rc);
    } else {
        g_message[0] = '\0';
    }

    /* The editor had the screen and gave it back on its own words: the file
     * manager paints its window again, because the picture is not its own
     * any more. */
    (void)ui_draw_frame(&g_screen.target, &g_model.windows[0], uiprog_font(), &g_style, true,
                        g_screen.line);
    g_shown_title[0] = '\0';
    rows_invalidate();
    title_paint();
    status_build();
    g_shown_status[0] = '\1';
    status_paint();
    rows_paint();
}

static void selection_move(int delta)
{
    int cursor = g_cursor + delta;

    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor >= g_count) {
        cursor = g_count > 0 ? g_count - 1 : 0;
    }

    g_cursor = cursor;
    view_follow();
}

static void enter_open(void)
{
    if (g_count == 0) {
        return;
    }

    if (is_directory(&g_entries[g_cursor])) {
        char path[FM_PATH_MAX];

        if (strcmp(g_entries[g_cursor].name, ".") == 0) {
            directory_open(g_path);
            return;
        }
        if (strcmp(g_entries[g_cursor].name, "..") == 0) {
            path_parent(path, sizeof(path), g_path);
            directory_open(path);
            return;
        }

        path_join(path, sizeof(path), g_path, g_entries[g_cursor].name);
        directory_open(path);
        return;
    }

    file_open();
}

static void key_handle(k_u16_t code)
{
    if (ui_screen_find(&g_model, FM_ID_DELETE) != UI_WINDOW_NONE) {
        if (code == K_INPUT_KEY_Y) {
            delete_answer();
        } else if (code == K_INPUT_KEY_N || code == K_INPUT_KEY_ESC) {
            modal_close();
        }
        return;
    }

    g_message[0] = '\0';

    switch (code) {
    case K_INPUT_KEY_UP:
        selection_move(-1);
        break;
    case K_INPUT_KEY_DOWN:
        selection_move(1);
        break;
    case K_INPUT_KEY_PAGEUP:
        selection_move(-(g_rows - 1));
        break;
    case K_INPUT_KEY_PAGEDOWN:
        selection_move(g_rows - 1);
        break;
    case K_INPUT_KEY_HOME:
        g_cursor = 0;
        view_follow();
        break;
    case K_INPUT_KEY_END:
        g_cursor = g_count > 0 ? g_count - 1 : 0;
        view_follow();
        break;
    case K_INPUT_KEY_ENTER:
        enter_open();
        return;
    case K_INPUT_KEY_DELETE:
        modal_open();
        return;
    case K_INPUT_KEY_D:
        modal_open();
        return;
    case K_INPUT_KEY_Q:
    case K_INPUT_KEY_ESC:
        g_running = false;
        return;
    default:
        return;
    }

    status_build();
    rows_paint();
    status_paint();
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

    if (g_cols > FM_COLS_MAX) {
        g_cols = FM_COLS_MAX;
    }
    if (g_rows > FM_ROWS_MAX) {
        g_rows = FM_ROWS_MAX;
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
    char start[FM_PATH_MAX];
    int index;

    (void)envp;

    if (argc >= 2) {
        (void)snprintf(start, sizeof(start), "%s", argv[1]);
    } else if (getcwd(start, sizeof(start)) == NULL) {
        (void)snprintf(start, sizeof(start), "/");
    }

    if (uiprog_open(&g_screen, "fm") != 0) {
        return ULIBC_EXIT_FAILURE;
    }

    ui_keyboard_init(&g_keyboard);
    layout();

    {
        struct ui_palette palette;

        (void)uiprog_palette(&palette);

        g_paper = palette.surface;
        g_ink = palette.ink;
        g_select_paper = palette.selection;
        g_select_ink = palette.selection_ink;
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
    index = ui_screen_add(&g_model, FM_ID_MAIN, "fm", &window, 0);
    if (index == UI_WINDOW_NONE) {
        static const char no_window[] = "fm: the picture is too small for a window\n";

        (void)write(ULIBC_STDERR, no_window, sizeof(no_window) - 1);
        uiprog_close(&g_screen);
        return ULIBC_EXIT_FAILURE;
    }

    /* The state of the program is the directory it opens with: a directory
     * that cannot be read is a program of one message and no list. */
    if (dir_load(start) != 0) {
        char line[FM_COLS_MAX + 1];
        int length = snprintf(line, sizeof(line), "fm: %s\n", g_message);

        if (length > 0) {
            (void)write(ULIBC_STDERR, line, (size_t)length);
        }
        uiprog_close(&g_screen);
        return ULIBC_EXIT_FAILURE;
    }

    (void)ui_draw_frame(&g_screen.target, &g_model.windows[0], uiprog_font(), &g_style, true,
                        g_screen.line);
    status_build();
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
         * square of the bar): it leaves as `q` would make it. */
        if (uiprog_close_requested(&g_screen)) {
            g_running = false;
        }

        /* One frame of a client: the damage of the keys since the last turn
         * and nothing else. A program of the whole screen has nothing to
         * flush - the device was written as it drew. */
        (void)uiprog_flush(&g_screen);

        (void)usleep(30000u);
    }

    uiprog_close(&g_screen);

    return ULIBC_EXIT_SUCCESS;
}
