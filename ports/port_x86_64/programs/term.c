/*
 * Micro OS — `/bin/term`, the terminal window of the desktop (ADR 0054).
 *
 * A window of the desktop that runs the programs of /bin: the person types a
 * line, the line is a command, and the answer of the command is printed in the
 * window under the line. The program is two halves - this one and the grid of
 * `ui/term.h` - and the half that is here is the machine's: a child process on
 * a pair of pipes, the keys of the window, the cells of the font and the damage
 * of a row.
 *
 * There is no pty in this system (ADR 0051 gave `/dev/tty` to a task and a mode
 * to the console, and there is no pair of a master and a slave): the child of
 * this window has a pipe for its input and a pipe for its output, and the
 * window is the terminal of the pair. The person does not type into the child:
 * a line is read here, and only a command that is running gets the keys. That
 * is what a terminal of a Unix looks like from the side of the program that
 * reads a pipe - `cat` of no argument echoes what it reads, and a program with
 * a prompt of its own reads a line of this window - and it is why the screen
 * of this window is the shell's: the shell of the machine is a task of the
 * kernel and not a program, so the window has the one of its own (ADR 0054).
 *
 * The keys: a character of the keyboard is the line, Enter runs it, Backspace
 * erases, Ctrl+C stops the running command (a signal, the way an interrupt of
 * a terminal is), Ctrl+T puts the window in the large font of the desktop
 * (8x16 cells and 16x32 cells out of one table of glyphs), and Ctrl+Q leaves.
 */
#include "uiprog.h"

#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "kernel/input.h"
#include "prog.h"
#include "ui/draw.h"
#include "ui/keyboard.h"
#include "ui/term.h"
#include "ulibc.h"

#define TERM_ID 1
#define TERM_LINE_MAX 128
#define TERM_WORDS_MAX 8
#define TERM_PATH_MAX 96
#define TERM_OUT_MAX 512

/* The colours of the terminal, out of the roles of the palette: the paper of a
 * window, the ink of a body, the bar of a title and the accent of the frame. */
static k_u16_t g_paper;
static k_u16_t g_ink;
static k_u16_t g_border;
static k_u16_t g_focus;
static k_u16_t g_bar;
static k_u16_t g_title_ink;

static struct uiprog g_screen;
static struct ui_screen g_model;
static struct ui_keyboard g_keyboard;
static struct ui_style g_style;

static const struct ui_font *g_font;
static bool g_large;
static int g_rows;
static int g_cols;
static int g_body_x;
static int g_body_y;

/* The grid of the terminal and the text of the rows that are on the glass: a
 * row whose cells did not change is not painted again, which is the promise of
 * a window that does not flicker (docs/desktop.md). */
static char g_cells[UI_TERM_COLS_MAX * UI_TERM_ROWS_MAX];
static struct ui_term g_term;
static char g_shown[UI_TERM_ROWS_MAX][UI_TERM_COLS_MAX + 1];
static int g_shown_cursor[UI_TERM_ROWS_MAX];
static int g_cursor_row = -1;

/* The line a person is typing and the command that is running. */
static char g_line[TERM_LINE_MAX];
static int g_line_length;
static bool g_running = true;

/* The words of the bar of the window: the name of the terminal and, while a
 * command runs in it, the name of the command. The bar of the server and the
 * button of its panel are the same words (docs/desktop.md). */
static char g_title[UI_TITLE_MAX] = "term";

static long g_child = -1;
static int g_child_in = -1;
static int g_child_out = -1;
static char g_child_name[TERM_PATH_MAX];
static unsigned g_commands;

static const char *const s_greeting[] = {
    "Micro OS terminal. Ctrl+T larger text, Ctrl+Q leave.",
    "Type a command of /bin (help lists the ones of this window).",
};

static void say(const char *text)
{
    (void)write(ULIBC_STDOUT, text, strlen(text));
}

static void say_fmt(const char *prefix, const char *word)
{
    char line[160];
    int length = snprintf(line, sizeof(line), "%s%s\n", prefix, word);

    if (length > 0) {
        (void)write(ULIBC_STDOUT, line, (size_t)length);
    }
}

/* --- the picture ---------------------------------------------------------- */

static void rows_invalidate(void)
{
    for (int row = 0; row < UI_TERM_ROWS_MAX; row++) {
        g_shown[row][0] = '\1';
        g_shown[row][1] = '\0';
        g_shown_cursor[row] = -2;
    }

    g_cursor_row = -1;
}

/* One row of the grid into the window: the cells of the row in the font of the
 * window, and the cell of the cursor in the two colours traded. A row whose
 * text and cursor are the ones already on the glass is not painted at all,
 * which is what makes a key of the terminal one row of the window. */
static void row_paint(int row)
{
    char text[UI_TERM_COLS_MAX + 1];
    int y = g_body_y + row * ui_font_cell_height(g_font);
    int cursor = (row == g_term.cursor_y) ? g_term.cursor_x : -1;

    if (row >= g_term.rows) {
        return;
    }

    for (int column = 0; column < g_cols; column++) {
        text[column] = g_term.cells[row * g_term.cols + column];
    }
    text[g_cols] = '\0';

    if (strcmp(text, g_shown[row]) == 0 && g_shown_cursor[row] == cursor) {
        return;
    }

    memcpy(g_shown[row], text, (size_t)g_cols + 1u);
    g_shown_cursor[row] = cursor;

    (void)uiprog_text_font(&g_screen, g_body_x, y, text, g_cols, g_ink, g_paper, g_font);

    if (cursor >= 0 && cursor < g_cols) {
        (void)uiprog_text_font(&g_screen, g_body_x + cursor * ui_font_cell_width(g_font), y,
                               text + cursor, 1, g_paper, g_ink, g_font);
    }
}

/* The rows of a damage rectangle of the grid; a row of the window below the
 * last row of the grid is the paper of the frame and not a row of text. */
static void rows_paint(int first, int count)
{
    for (int row = first; row < first + count && row < g_rows; row++) {
        row_paint(row);
    }
}

/*
 * The rows of the grid that changed. A row is painted only when its text or
 * the cell of its cursor moved, so the damage of a key is the row of the key:
 * the rectangle of `ui/term.h` is the rows that changed, and the row the
 * cursor left is repainted because its last cell was a cursor and is a letter
 * now (docs/desktop.md, the frame of the compositor).
 */
static void grid_paint(void)
{
    int x;
    int y;
    int w;
    int h;
    bool changed = ui_term_take_damage(&g_term, &x, &y, &w, &h);

    if (g_cursor_row >= 0 && g_cursor_row != g_term.cursor_y && g_cursor_row < g_rows) {
        row_paint(g_cursor_row);
    }

    g_cursor_row = g_term.cursor_y;

    if (changed) {
        rows_paint(y, h);
    }

    row_paint(g_cursor_row);
}

/*
 * The title of the window, in its own bar and in the panel of the server. The
 * bar of a client is the pixels of the client, so the row of the title is
 * painted here and only when the words change: a command that starts is a row
 * of 16 pixels of a window and not a frame (docs/desktop.md).
 */
static void title_set(const char *title)
{
    int index;
    int cells;
    int length;
    int cell_w = ui_font_cell_width(g_font);

    if (title == NULL || strcmp(title, g_title) == 0) {
        return;
    }

    (void)snprintf(g_title, sizeof(g_title), "%s", title);

    index = ui_screen_find(&g_model, TERM_ID);
    if (index != UI_WINDOW_NONE) {
        (void)snprintf(g_model.windows[index].title, sizeof(g_model.windows[index].title), "%s",
                       g_title);
    }

    (void)uiprog_title(&g_screen, g_title);

    cells = ((int)g_screen.info.width - 2 * UI_FRAME_PAD) / cell_w;
    length = (int)strlen(g_title);
    if (length > cells) {
        length = cells;
    }

    (void)uiprog_text_font(&g_screen, UI_FRAME_PAD, UI_FRAME_MARGIN, g_title, cells, g_title_ink,
                           g_bar, g_font);
}

static void frame_paint(void)
{
    int index = ui_screen_find(&g_model, TERM_ID);

    if (index == UI_WINDOW_NONE) {
        return;
    }

    if (ui_draw_frame(&g_screen.target, &g_model.windows[index], g_font, &g_style, true,
                      g_screen.line) != 0) {
        return;
    }

    rows_invalidate();
    rows_paint(0, g_rows);
}

/* The layout of the window in the font of the moment: the cells of a row of the
 * body and the rows below the title bar. A window that changes its size of text
 * is laid out again and draws its grid from nothing. */
static void layout(void)
{
    int cell_w = ui_font_cell_width(g_font);
    int cell_h = ui_font_cell_height(g_font);

    g_body_x = UI_FRAME_PAD;
    g_body_y = UI_FRAME_TITLE_HEIGHT(g_font);

    g_cols = ((int)g_screen.info.width - 2 * g_body_x) / cell_w;
    g_rows = ((int)g_screen.info.height - g_body_y) / cell_h;

    if (g_cols > UI_TERM_COLS_MAX) {
        g_cols = UI_TERM_COLS_MAX;
    }
    if (g_rows > UI_TERM_ROWS_MAX) {
        g_rows = UI_TERM_ROWS_MAX;
    }
    if (g_cols < 1) {
        g_cols = 1;
    }
    if (g_rows < 1) {
        g_rows = 1;
    }

    ui_term_init(&g_term, g_cols, g_rows, g_cells);
}

/* --- the line and the commands -------------------------------------------- */

static void prompt(void)
{
    (void)ui_term_put(&g_term, "$ ", 2);
    g_line_length = 0;
}

static void print_line(const char *text)
{
    char line[TERM_LINE_MAX + 2];

    (void)snprintf(line, sizeof(line), "%s\n", text);
    (void)ui_term_put(&g_term, line, strlen(line));
}

static bool build_path(const char *name, char *out, k_size_t size)
{
    int length;

    if (strchr(name, '/') != NULL) {
        length = snprintf(out, size, "%s", name);
    } else {
        length = snprintf(out, size, "/bin/%s", name);
    }

    return length > 0 && (k_size_t)length < size;
}

/* The words of the line: spaces separate them and a command of more words than
 * the table holds is refused whole, because a command that lost its last
 * argument is a different command. There is no quote and no operator in this
 * line: a pipe and a variable are the work of the shell of the machine, which
 * is a task of the kernel and not this window (ADR 0054). */
static int split_line(char *line, char *words[], int max)
{
    int count = 0;
    char *at = line;

    while (*at != '\0') {
        while (*at == ' ') {
            at++;
        }

        if (*at == '\0') {
            break;
        }

        if (count == max) {
            return -1;
        }

        words[count++] = at;

        while (*at != '\0' && *at != ' ') {
            at++;
        }

        if (*at == ' ') {
            *at++ = '\0';
        }
    }

    return count;
}

static void child_stop_reading(void);

/* A command of this window and not of /bin: the directory of the task, the end
 * of the window and the list of what there is. */
static bool builtin(int argc, char *argv[])
{
    if (strcmp(argv[0], "help") == 0) {
        print_line("The commands of /bin: cat, ls, echo, head, wc, grep, mkdir, rm, mv,");
        print_line("pwd, sleep, procs, zcat, lua, bc, file...; this window knows:");
        print_line("  cd <dir>   the directory of the next command");
        print_line("  clear      the grid of the window, blank");
        print_line("  exit       leave the window (Ctrl+Q is the same)");
        print_line("  help       this list");
        return true;
    }

    if (strcmp(argv[0], "exit") == 0) {
        g_running = false;
        return true;
    }

    if (strcmp(argv[0], "clear") == 0) {
        ui_term_clear(&g_term);
        return true;
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *where = argc > 1 ? argv[1] : "/";

        if (chdir(where) != 0) {
            static const char no_dir[] = "cd: the directory is not there\n";

            (void)ui_term_put(&g_term, no_dir, sizeof(no_dir) - 1u);
        }

        return true;
    }

    return false;
}

/* The child of a command: a program of /bin with the two pipes for its input
 * and its output. The descriptors of the window (3, 4 and 5 of the protocol)
 * go before the execve: the child is not a window of the desktop. */
static void child_run(const char *path, char *const argv[], int input, int output)
{
    static char *const envp[] = {NULL};

    if (input != ULIBC_STDIN) {
        (void)dup2(input, ULIBC_STDIN);
    }
    if (output != ULIBC_STDOUT) {
        (void)dup2(output, ULIBC_STDOUT);
        (void)dup2(output, ULIBC_STDERR);
    }

    for (int fd = 3; fd < 16; fd++) {
        (void)close(fd);
    }

    (void)execve(path, argv, envp);

    {
        static const char missing[] = "term: the program did not start\n";

        (void)write(ULIBC_STDERR, missing, sizeof(missing) - 1u);
    }

    _exit(127);
}

static void command_run(const char *line)
{
    char copy[TERM_LINE_MAX];
    char path[TERM_PATH_MAX];
    char *words[TERM_WORDS_MAX + 1];
    int down[2];
    int up[2];
    int count;
    long pid;

    (void)snprintf(copy, sizeof(copy), "%s", line);
    count = split_line(copy, words, TERM_WORDS_MAX);

    if (count < 0) {
        print_line("term: too many words");
        return;
    }

    if (count == 0) {
        return;
    }

    words[count] = NULL;

    if (builtin(count, words)) {
        return;
    }

    if (!build_path(words[0], path, sizeof(path))) {
        print_line("term: the name of the program is too long");
        return;
    }

    if (pipe(down) != 0 || pipe(up) != 0) {
        print_line("term: no pipes for the command");
        return;
    }

    pid = fork();
    if (pid < 0) {
        (void)close(down[0]);
        (void)close(down[1]);
        (void)close(up[0]);
        (void)close(up[1]);
        print_line("term: fork refused the command");
        return;
    }

    if (pid == 0) {
        (void)close(down[1]);
        (void)close(up[0]);
        child_run(path, words, down[0], up[1]);
    }

    (void)close(down[0]);
    (void)close(up[1]);

    g_child = pid;
    g_child_in = down[1];
    g_child_out = up[0];
    g_commands++;
    (void)snprintf(g_child_name, sizeof(g_child_name), "%s", words[0]);

    say_fmt("term: the command ", g_child_name);

    /* The title of the window names the command that runs in it, so a person
     * with several terminals knows which one is busy. */
    {
        char title[UI_TITLE_MAX];

        (void)snprintf(title, sizeof(title), "term - %s", g_child_name);
        title_set(title);
    }
}

static void child_reap(void)
{
    int status = 0;
    long pid;

    if (g_child < 0) {
        return;
    }

    pid = waitpid((int)g_child, &status, KSYS_WNOHANG);

    if (pid != g_child) {
        return;
    }

    if (g_child_out >= 0) {
        (void)close(g_child_out);
    }
    if (g_child_in >= 0) {
        (void)close(g_child_in);
    }

    g_child = -1;
    g_child_out = -1;
    g_child_in = -1;

    {
        char line[120];

        (void)snprintf(line, sizeof(line), "term: %s left with %d", g_child_name, status);
        say_fmt("", line);
    }

    title_set("term");
    prompt();
}

/* The bytes of the running command: the grid of the window is its screen. */
static void child_read(void)
{
    char buffer[TERM_OUT_MAX];
    struct pollfd entry;
    long got;

    if (g_child_out < 0) {
        return;
    }

    entry.fd = g_child_out;
    entry.events = KSYS_POLLIN;
    entry.revents = 0;

    if (poll(&entry, 1, 0) <= 0) {
        return;
    }

    got = read(g_child_out, buffer, sizeof(buffer));

    if (got > 0) {
        (void)ui_term_put(&g_term, buffer, (k_size_t)got);
    }
}

/* The keys of the running command: a program of a pipe reads bytes, so the
 * arrows are the sequences of a terminal of the console and Ctrl+D is the end
 * of its input. */
static void child_key(k_u16_t code, const struct ui_keyboard *keyboard)
{
    char bytes[8];
    k_size_t length = 0;

    switch (code) {
    case K_INPUT_KEY_ENTER:
        bytes[length++] = '\n';
        break;
    case K_INPUT_KEY_BACKSPACE:
        bytes[length++] = 127;
        break;
    case K_INPUT_KEY_TAB:
        bytes[length++] = '\t';
        break;
    case K_INPUT_KEY_UP:
        memcpy(bytes, "\033[A", 3);
        length = 3;
        break;
    case K_INPUT_KEY_DOWN:
        memcpy(bytes, "\033[B", 3);
        length = 3;
        break;
    case K_INPUT_KEY_RIGHT:
        memcpy(bytes, "\033[C", 3);
        length = 3;
        break;
    case K_INPUT_KEY_LEFT:
        memcpy(bytes, "\033[D", 3);
        length = 3;
        break;
    case K_INPUT_KEY_HOME:
        memcpy(bytes, "\033[H", 3);
        length = 3;
        break;
    case K_INPUT_KEY_END:
        memcpy(bytes, "\033[F", 3);
        length = 3;
        break;
    case K_INPUT_KEY_DELETE:
        memcpy(bytes, "\033[3~", 4);
        length = 4;
        break;
    default: {
        char character = ui_keyboard_char(keyboard, code);

        if (character >= 0x20 && character < 0x7f) {
            bytes[length++] = character;
        } else if (keyboard->ctrl && character >= 0 && character < 0x20) {
            /* Ctrl+D of the input of a program that reads a line is the byte
             * the console sends, and Ctrl+A the start of the line of a shell
             * that reads one. */
            bytes[length++] = character;
        }
        break;
    }
    }

    if (length > 0 && g_child_in >= 0) {
        (void)write(g_child_in, bytes, length);
    }
}

/* The line of the prompt: the characters are the grid, Enter is the command,
 * and the Ctrl combinations are the keys of this window. */
static void line_key(k_u16_t code, const struct ui_keyboard *keyboard)
{
    if (keyboard->ctrl) {
        switch (code) {
        case K_INPUT_KEY_T:
            g_large = !g_large;
            g_font = g_large ? uiprog_font_large() : uiprog_font();
            layout();
            frame_paint();
            print_line(g_large ? "term: the text is 16x32" : "term: the text is 8x16");
            prompt();
            return;
        case K_INPUT_KEY_Q:
            g_running = false;
            return;
        case K_INPUT_KEY_C:
            (void)ui_term_put(&g_term, "^C\n", 3);
            g_line_length = 0;
            prompt();
            return;
        case K_INPUT_KEY_U:
            while (g_line_length > 0) {
                (void)ui_term_put(&g_term, "\b \b", 3);
                g_line_length--;
            }
            return;
        default:
            return;
        }
    }

    switch (code) {
    case K_INPUT_KEY_ENTER:
        (void)ui_term_put(&g_term, "\n", 1);
        g_line[g_line_length] = '\0';
        if (g_line_length > 0) {
            command_run(g_line);
        }
        if (g_child < 0) {
            prompt();
        }
        g_line_length = 0;
        return;
    case K_INPUT_KEY_BACKSPACE:
        if (g_line_length > 0) {
            g_line_length--;
            (void)ui_term_put(&g_term, "\b \b", 3);
        }
        return;
    case K_INPUT_KEY_TAB: {
        char spaces[5] = "    ";

        if (g_line_length + 4 < TERM_LINE_MAX) {
            memcpy(g_line + g_line_length, spaces, 4);
            g_line_length += 4;
            (void)ui_term_put(&g_term, spaces, 4);
        }
        return;
    }
    default:
        break;
    }

    {
        char character = ui_keyboard_char(keyboard, code);

        if (character >= 0x20 && character < 0x7f && g_line_length + 1 < TERM_LINE_MAX) {
            g_line[g_line_length++] = character;
            (void)ui_term_put(&g_term, &character, 1);
        }
    }
}

/* --- the program ---------------------------------------------------------- */

static void palette_load(void)
{
    struct ui_palette palette;

    (void)uiprog_palette(&palette);

    g_paper = palette.surface;
    g_ink = palette.ink;
    g_border = palette.border;
    g_focus = palette.accent;
    g_bar = palette.surface_high;
    g_title_ink = palette.title_ink;

    g_style.paper = g_paper;
    g_style.ink = g_ink;
    g_style.border = g_border;
    g_style.focus_border = g_focus;
    g_style.title_paper = g_bar;
    g_style.title_ink = g_title_ink;
    g_style.separator = g_border;
}

int main(int argc, char *const argv[], char *const envp[])
{
    struct ui_rect bounds;
    struct ui_rect window;
    int index;

    (void)argc;
    (void)argv;
    (void)envp;

    if (uiprog_open(&g_screen, "term") != 0) {
        return ULIBC_EXIT_FAILURE;
    }

    ui_keyboard_init(&g_keyboard);
    palette_load();

    g_font = uiprog_font();
    g_large = false;

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = (int)g_screen.info.width;
    bounds.h = (int)g_screen.info.height;
    ui_screen_init(&g_model, &bounds);

    /* The window does not draw a ring of its own: the server paints the ring
     * of the focus around the window that has the keys and the frame here is
     * the border of a window of the desktop. */
    g_style.focus_border = g_focus;

    window = bounds;
    index = ui_screen_add(&g_model, TERM_ID, "term", &window, 0);
    if (index == UI_WINDOW_NONE) {
        static const char no_window[] = "term: the picture is too small for a window\n";

        (void)write(ULIBC_STDERR, no_window, sizeof(no_window) - 1u);
        uiprog_close(&g_screen);
        return ULIBC_EXIT_FAILURE;
    }

    layout();

    for (k_size_t line = 0; line < sizeof(s_greeting) / sizeof(s_greeting[0]); line++) {
        print_line(s_greeting[line]);
    }

    prompt();
    frame_paint();
    (void)uiprog_title(&g_screen, g_title);
    say("term: the window of the terminal is up\n");

    while (g_running) {
        struct input_event event;

        /* The bytes of the command that runs: its output is the grid, and the
         * window is painted once a turn with the rows that changed. */
        child_read();

        while (uiprog_event(&g_screen, &event) == 1) {
            if (ui_keyboard_event(&g_keyboard, &event)) {
                continue;
            }

            if (event.value != INPUT_VALUE_PRESS) {
                continue;
            }

            if (g_child >= 0) {
                /* Ctrl+C of a running command is the interrupt of a terminal:
                 * the command stops and the window comes back to its prompt. */
                if (g_keyboard.ctrl && event.code == K_INPUT_KEY_C) {
                    char line[120];

                    (void)snprintf(line, sizeof(line), "term: Ctrl+C, %s is told to stop",
                                   g_child_name);
                    say_fmt("", line);
                    (void)ui_term_put(&g_term, "^C\n", 3);
                    (void)prog_kill(g_child, KSYS_SIGINT);
                    continue;
                }

                if (g_keyboard.ctrl && event.code == K_INPUT_KEY_Q) {
                    g_running = false;
                    break;
                }

                child_key(event.code, &g_keyboard);
                continue;
            }

            line_key(event.code, &g_keyboard);
        }

        if (uiprog_close_requested(&g_screen)) {
            g_running = false;
        }

        child_reap();
        grid_paint();
        (void)uiprog_flush(&g_screen);

        /* A terminal waits for a program and for a person at once: the wait is
         * a short one, so the output of the command is a line of the window and
         * not a screen of it after the command ended. */
        {
            struct pollfd entry;

            entry.fd = g_child_out;
            entry.events = KSYS_POLLIN;
            entry.revents = 0;

            if (g_child_out >= 0 && poll(&entry, 1, 20) > 0) {
                continue;
            }
        }

        (void)usleep(g_child_out >= 0 ? 5000u : 30000u);
    }

    if (g_child >= 0) {
        (void)prog_kill(g_child, KSYS_SIGTERM);
    }

    child_stop_reading();
    uiprog_close(&g_screen);

    say("term: the window of the terminal is closed\n");

    return ULIBC_EXIT_SUCCESS;
}

/* The descriptors of the command of a window that is going: the child is told
 * to stop and its pipes are closed, so a program that reads a line sees the
 * end of its input and not a descriptor of a window that is gone. */
static void child_stop_reading(void)
{
    if (g_child_in >= 0) {
        (void)close(g_child_in);
        g_child_in = -1;
    }

    if (g_child_out >= 0) {
        (void)close(g_child_out);
        g_child_out = -1;
    }

    if (g_child >= 0) {
        (void)waitpid((int)g_child, NULL, 0);
        g_child = -1;
    }
}
