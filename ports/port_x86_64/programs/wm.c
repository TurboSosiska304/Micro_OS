/*
 * Micro OS — `/bin/wm`, the server of the desktop (docs/desktop.md).
 *
 * The machine has a picture, a keyboard and a pointer, and no window system:
 * each program of the interface of ADR 0030 painted the whole screen itself.
 * This program is the one owner of the screen. It paints the wallpaper, the
 * panel and the pointer, holds the list of the windows in the order of the
 * paint, copies the buffer of every window into its rectangle, and gives a
 * key to the window that has the focus and a button to the window under the
 * pointer.
 *
 * A window is a program of ring 3 the server started with `fork` and
 * `execve`, exactly as the shell starts one: a buffer in a file of /tmp
 * (`mmap`, MAP_SHARED), two pipes (the events down, the requests up) and the
 * variable `MICROOS_WM` in its environment. The protocol is
 * `ui/wm_protocol.h` and the half of it a program uses is `ui/wm_client.h`,
 * both portable and tested on the host.
 *
 * The picture is composed, not repainted: a change of a client, a raise, a
 * move and the pointer add rectangles to the damage list, and the loop paints
 * those rectangles and nothing else. A rectangle is clipped to the picture
 * and to the window it belongs to, and the client of a window that is under
 * another one costs nothing while it is covered - the server reads the buffer
 * of a window only where the window is visible.
 *
 * What the server answers: a click raises a window and gives it the keys; a
 * drag of its title bar moves it; the square at the right of the title bar,
 * Alt+F4 and the button of the panel close it; the panel brings a window back
 * and puts it away. `q` and `Esc` are the keys of the clients, not of the
 * server: the desktop ends when its last window does.
 */
#include "uiprog.h"

#include <poll.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/errno.h"
#include "prog.h"
#include "ui/draw.h"
#include "ui/window.h"
#include "ui/wm_protocol.h"
#include "ulibc.h"

/* The windows of one desktop: three programs and the room of one more is
 * more than the screen of 1280 by 800 holds and less than the table of the
 * descriptors of a task (each window is three of them). */
#define WM_APPS 3

/* The damage of one turn of the loop: a key, a motion and the raise of two
 * windows are a handful of rectangles, and a list that is full is cleared
 * into the whole picture. */
#define WM_DAMAGE_MAX 8

/* The panel: one line of the bottom of the picture, a button a window. */
#define WM_PANEL_HEIGHT 24

/* The square of closing, in the right of the title bar of a window and at the
 * right end of the button of the panel: the same cross in a square of paint. */
#define WM_CLOSE_BOX 12
#define WM_PANEL_CLOSE 12

/* The pixels of a title bar that a drag keeps on the picture. The bar is the
 * handle of the window and the only thing a person can drag it back by, so a
 * window pulled to an edge keeps the bar on the picture and loses the rest of
 * itself, never the handle. */
#define WM_TITLE_KEEP 64

/* The margin the ring of the focus takes around a window. */
#define WM_FOCUS_RING 2

/* The first number of a window of the model: the ids of the programs (the
 * document and the dialog of the editor) are small numbers of their own. */
#define WM_ID_BASE 100

/* The colours of the desktop: two blues of the wallpaper, the bar of the
 * panel, the cyan of the focus, the red of the square of closing. */
#define WM_WALL_A 0x0861u
#define WM_WALL_B 0x1082u
#define WM_WALL_BAND 64
#define WM_PANEL_PAPER 0x0010u
#define WM_PANEL_INK 0xFFFFu
#define WM_PANEL_FOCUS 0x57FFu
#define WM_FOCUS_INK 0xFFEAu /* the yellow of the window that has the keys */
#define WM_CLOSE_PAPER 0xA800u
#define WM_CLOSE_INK 0xFFFFu
#define WM_CURSOR_INK 0xFFFFu
#define WM_CURSOR_EDGE 0x0000u

/* The arrow of the pointer: the width of its rows. A row is a line of ink
 * with one pixel of the edge under it, which is the whole of the picture of
 * a pointer of this size. */
static const k_u8_t s_arrow[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 7, 4, 3, 2};
#define WM_ARROW_ROWS ((int)K_ARRAY_SIZE(s_arrow))
#define WM_ARROW_WIDTH 12 /* the widest row and its edge */

/* One client of the server. The descriptors are the server's ends: it writes
 * the events and reads the requests; the client got the other two, plus the
 * buffer, before its execve. */
struct wm_app {
    bool used;
    int id;
    long pid;
    int events;   /* write: the events of the window */
    int requests; /* read: what the window asks */
    int buffer;   /* the file of the pixels */
    char path[40];
    char title[WM_TITLE_MAX];
    struct ui_rect rect;
    unsigned char inbox[WM_MESSAGE_BYTES];
    unsigned inbox_bytes;
};

static struct uiprog g_screen;
static struct ui_screen g_model;

static struct wm_app g_apps[WM_APPS];

static int g_width;
static int g_height;

static int g_pointer_x;
static int g_pointer_y;
static bool g_button;

/* The drag of a title bar: the window, the point of the press and the
 * rectangle the window had when the drag began. */
static bool g_dragging;
static int g_drag_app = -1;
static int g_drag_x;
static int g_drag_y;
static struct ui_rect g_drag_rect;

/* The window that took the press of the pointer: the release is its too,
 * whatever the pointer does in between. */
static int g_press_app = -1;

/*
 * The press that is waiting for its release. The panel and the square of
 * closing act when the button comes up and the pointer is still on the thing
 * that went down: a press that slides away is a press of nothing, the rule of
 * every button of every interface.
 */
static int g_panel_press = -1;   /* the window whose button of the panel is down */
static bool g_panel_press_close; /* the press was on the cross of the button */
static int g_close_press = -1;   /* the window whose square of closing is down */

/* The alt key, for Alt+F4. */
static bool g_alt;

static bool g_running = true;
static unsigned g_windows_spawned;

static struct ui_rect g_damage[WM_DAMAGE_MAX];
static int g_damage_count;

/* The pixels of a row of a window, read out of the file of the buffer: one
 * transfer of the system and no more, which is what the file and the device
 * both ask for. */
static k_u16_t g_pixels[KSYS_MAX_IO / FB_PIXEL_BYTES];

static void say(const char *text)
{
    (void)write(ULIBC_STDOUT, text, strlen(text));
}

/* A line of the log of the desktop on the serial console: the smoke test of
 * the machine reads the raises, the moves and the closes of the windows, and
 * a person at the console reads the same lines. */
static void say_title(const char *prefix, const char *title)
{
    char line[160];
    int length = snprintf(line, sizeof(line), "%s%s\n", prefix, title);

    if (length > 0) {
        (void)write(ULIBC_STDOUT, line, (size_t)length);
    }
}

/* --- the damage ----------------------------------------------------------- */

static void damage_add(int x, int y, int w, int h)
{
    struct ui_rect rect;
    int index;

    if (w <= 0 || h <= 0) {
        return;
    }

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;

    /* The picture is the whole of what the device accepts: a rectangle that
     * leaves it is cut, and the panel of the desktop is a part of it. */
    if (rect.x < 0) {
        rect.w += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.h += rect.y;
        rect.y = 0;
    }
    if (rect.x + rect.w > g_width) {
        rect.w = g_width - rect.x;
    }
    if (rect.y + rect.h > g_height) {
        rect.h = g_height - rect.y;
    }

    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    for (index = 0; index < g_damage_count; index++) {
        struct ui_rect *seen = &g_damage[index];
        int left = rect.x < seen->x ? rect.x : seen->x;
        int top = rect.y < seen->y ? rect.y : seen->y;
        int right = rect.x + rect.w > seen->x + seen->w ? rect.x + rect.w : seen->x + seen->w;
        int bottom = rect.y + rect.h > seen->y + seen->h ? rect.y + rect.h : seen->y + seen->h;

        /* A rectangle that touches one already waiting grows it: a drag of a
         * window is a hundred small rectangles and the union is one. */
        if (left <= seen->x + seen->w && seen->x <= right && top <= seen->y + seen->h &&
            seen->y <= bottom) {
            seen->x = left;
            seen->y = top;
            seen->w = right - left;
            seen->h = bottom - top;
            return;
        }
    }

    if (g_damage_count == WM_DAMAGE_MAX) {
        /* A full list is the whole picture: the correctness of a paint is not
         * worth a queue, and a desktop of this size repaints in a few
         * transfers. */
        g_damage_count = 1;
        g_damage[0].x = 0;
        g_damage[0].y = 0;
        g_damage[0].w = g_width;
        g_damage[0].h = g_height;
        return;
    }

    g_damage[g_damage_count++] = rect;
}

/* --- the picture ---------------------------------------------------------- */

static int panel_top(void)
{
    return g_height - WM_PANEL_HEIGHT;
}

static k_u16_t wallpaper_color(int y)
{
    return ((y / WM_WALL_BAND) % 2) == 0 ? WM_WALL_A : WM_WALL_B;
}

static const struct wm_app *app_of_id(int id)
{
    int index;

    for (index = 0; index < WM_APPS; index++) {
        if (g_apps[index].used && g_apps[index].id == id) {
            return &g_apps[index];
        }
    }

    return NULL;
}

/* The buffer of a window, from the file the client maps: the rectangle
 * (x, y, w, h) of the window into the picture at the point of the screen.
 *
 * The bytes are read and not mapped. The kernel of 1.7 gives every mapping
 * its own frame and writes the frame back to the file on `msync`, so a second
 * mapping of the same file would be a copy of the bytes at the moment it
 * faulted - and the server would read the frame of the client only by
 * throwing its own away. The file is the meeting point of the two, and a read
 * of it is the frame the client just wrote. */
static void app_pixels(const struct wm_app *app, int x, int y, int w, int h, int screen_x,
                       int screen_y)
{
    size_t window_row = (size_t)app->rect.w * FB_PIXEL_BYTES;
    size_t row_bytes = (size_t)w * FB_PIXEL_BYTES;
    size_t block = (size_t)sizeof(g_pixels);
    int rows = (int)(block / window_row);
    int row;

    if (rows < 1 || w > (int)app->rect.w || x < 0 || y < 0 || w <= 0 || h <= 0) {
        return;
    }

    /* A strip is whole rows of the file - the buffer is the window's width and
     * not the width of the part of it that is visible - and the visible part
     * of every row, the first one included, is moved to the front of the block
     * before the blit. A strip that read only the visible columns would read
     * the rows of the file as if they touched, and a strip that skipped the
     * first row left it at the column 0 of the row instead of the column x:
     * the old place of the pointer in the bar of a title showed the first
     * letters of the title, a fragment a cursor wide. */
    for (row = 0; row < h; row += rows) {
        int take = h - row < rows ? h - row : rows;
        size_t bytes = (size_t)take * window_row;
        long offset = (long)((size_t)(y + row) * (size_t)app->rect.w) * (long)FB_PIXEL_BYTES;
        size_t got = 0;
        int line;

        if (lseek(app->buffer, offset, ULIBC_SEEK_SET) != offset) {
            return;
        }

        while (got < bytes) {
            long part = read(app->buffer, (char *)g_pixels + got, bytes - got);

            if (part <= 0) {
                return;
            }

            got += (size_t)part;
        }

        for (line = 0; line < take; line++) {
            memmove((char *)g_pixels + (size_t)line * row_bytes,
                    (char *)g_pixels + (size_t)line * window_row + (size_t)x * FB_PIXEL_BYTES,
                    row_bytes);
        }

        (void)uiprog_blit(&g_screen, screen_x, screen_y + row, w, take, g_pixels);
    }
}

/* The square of closing: the red square and the cross of the ink that every
 * square of closing of the desktop is made of - the one of a title bar and
 * the one at the right end of the button of the panel. */
static void cross_paint(int x, int y, int size)
{
    int step;

    (void)uiprog_fill(&g_screen, x, y, size, size, WM_CLOSE_PAPER);

    for (step = 2; step < size - 2; step++) {
        (void)uiprog_fill(&g_screen, x + step, y + step, 2, 2, WM_CLOSE_INK);
        (void)uiprog_fill(&g_screen, x + size - step - 2, y + step, 2, 2, WM_CLOSE_INK);
    }
}

/* The square that closes a window, in the right of its title bar. */
static struct ui_rect close_box_rect(const struct ui_window *window)
{
    struct ui_rect rect;

    rect.w = WM_CLOSE_BOX;
    rect.h = WM_CLOSE_BOX;
    rect.x = window->rect.x + window->rect.w - WM_CLOSE_BOX - 4;
    rect.y = window->rect.y + 4;

    return rect;
}

static void close_box_paint(const struct ui_window *window)
{
    struct ui_rect box = close_box_rect(window);

    cross_paint(box.x, box.y, box.w);
}

/* The ring of the window that has the keys: two pixels around it, drawn with
 * the window and over what is under it, which is the order of the paint. */
static void focus_ring_paint(const struct ui_window *window)
{
    int x = window->rect.x - WM_FOCUS_RING;
    int y = window->rect.y - WM_FOCUS_RING;
    int w = window->rect.w + 2 * WM_FOCUS_RING;
    int h = window->rect.h + 2 * WM_FOCUS_RING;

    (void)uiprog_fill(&g_screen, x, y, w, WM_FOCUS_RING, WM_FOCUS_INK);
    (void)uiprog_fill(&g_screen, x, y + h - WM_FOCUS_RING, w, WM_FOCUS_RING, WM_FOCUS_INK);
    (void)uiprog_fill(&g_screen, x, y, WM_FOCUS_RING, h, WM_FOCUS_INK);
    (void)uiprog_fill(&g_screen, x + w - WM_FOCUS_RING, y, WM_FOCUS_RING, h, WM_FOCUS_INK);
}

/* One window of the model, the part of it that `area` covers. */
static void window_paint(int index, const struct ui_rect *area)
{
    const struct ui_window *window = &g_model.windows[index];
    const struct wm_app *app = app_of_id(window->id);
    struct ui_rect part;
    struct ui_rect box;
    struct ui_rect ring;

    if (app == NULL) {
        return;
    }

    if (ui_rect_intersect(&window->rect, area, &part)) {
        app_pixels(app, part.x - window->rect.x, part.y - window->rect.y, part.w, part.h, part.x,
                   part.y);
    }

    box = close_box_rect(window);
    if (ui_rect_intersect(&box, area, &part)) {
        close_box_paint(window);
    }

    if (g_model.focus == index) {
        ring.x = window->rect.x - WM_FOCUS_RING;
        ring.y = window->rect.y - WM_FOCUS_RING;
        ring.w = window->rect.w + 2 * WM_FOCUS_RING;
        ring.h = window->rect.h + 2 * WM_FOCUS_RING;

        if (ui_rect_intersect(&ring, area, &part)) {
            focus_ring_paint(window);
        }
    }
}

/* --- the panel ------------------------------------------------------------ */

/* The button of a window in the panel: the title with a margin, a button a
 * window from the left. The width is a function of the title, so the paint
 * and the hit test of the panel are one arithmetic. */
static struct ui_rect panel_button_rect(int app_index)
{
    struct ui_rect rect;
    int x = 8;
    int index;

    for (index = 0; index < app_index; index++) {
        if (!g_apps[index].used) {
            continue;
        }

        x += (int)strlen(g_apps[index].title) * FONT8_WIDTH + 16;
    }

    rect.x = x;
    rect.y = panel_top() + 2;
    rect.w = (int)strlen(g_apps[app_index].title) * FONT8_WIDTH + 12 + WM_PANEL_CLOSE + 2;
    rect.h = WM_PANEL_HEIGHT - 4;

    if (rect.w > 200) {
        rect.w = 200;
    }

    return rect;
}

/* The square of closing at the right end of the button of the panel: the same
 * cross a title bar has, so a window can be closed from the panel as well as
 * from the bar it may have dragged out of the picture. */
static struct ui_rect panel_close_rect(int app_index)
{
    struct ui_rect button = panel_button_rect(app_index);
    struct ui_rect rect;

    rect.w = WM_PANEL_CLOSE;
    rect.h = WM_PANEL_CLOSE;
    rect.x = button.x + button.w - WM_PANEL_CLOSE - 1;
    rect.y = button.y + (button.h - WM_PANEL_CLOSE) / 2;

    return rect;
}

static void panel_paint(const struct ui_rect *area)
{
    struct ui_rect panel;
    struct ui_rect part;
    int index;

    panel.x = 0;
    panel.y = panel_top();
    panel.w = g_width;
    panel.h = WM_PANEL_HEIGHT;

    if (!ui_rect_intersect(&panel, area, &part)) {
        return;
    }

    (void)uiprog_fill(&g_screen, panel.x, panel.y, panel.w, panel.h, WM_PANEL_PAPER);

    for (index = 0; index < WM_APPS; index++) {
        struct ui_rect button;
        int cells;
        char label[24];
        bool focused;

        if (!g_apps[index].used) {
            continue;
        }

        struct ui_rect visible;

        button = panel_button_rect(index);

        if (!ui_rect_intersect(&button, &part, &visible)) {
            continue;
        }

        focused = ui_screen_find(&g_model, g_apps[index].id) == g_model.focus;

        (void)uiprog_fill(&g_screen, button.x, button.y, button.w, button.h,
                          focused ? WM_PANEL_FOCUS : WM_PANEL_PAPER);

        (void)snprintf(label, sizeof(label), "%s", g_apps[index].title);

        /* The label stops before the square of closing of the button. */
        cells = (button.w - WM_PANEL_CLOSE - 8) / FONT8_WIDTH;
        if (cells > (int)sizeof(label) - 1) {
            cells = (int)sizeof(label) - 1;
        }

        (void)uiprog_text(&g_screen, button.x + 6, button.y + (button.h - FONT8_HEIGHT) / 2, label,
                          cells, focused ? WM_PANEL_PAPER : WM_PANEL_INK,
                          focused ? WM_PANEL_FOCUS : WM_PANEL_PAPER);

        {
            struct ui_rect close = panel_close_rect(index);

            cross_paint(close.x, close.y, close.w);
        }
    }
}

/* --- the scene ------------------------------------------------------------ */

/* The whole of the picture at a rectangle: the wallpaper, the windows from
 * the one at the bottom of the order, the panel. The pointer is not here: it
 * is drawn after, over whatever the rectangle holds. */
static void scene_paint(const struct ui_rect *rect)
{
    int bottom = rect->y + rect->h;

    int band;

    /* The bands are counted from the top of the picture and not from the top
     * of the rectangle: a damage rectangle is fifteen pixels of a window and
     * not a multiple of the band, and a wallpaper that moved with it would
     * paint two stripes of the same colour where one is. */
    for (band = rect->y / 16 * 16; band < bottom; band += 16) {
        int top = band < rect->y ? rect->y : band;
        int end = band + 16 < bottom ? band + 16 : bottom;

        (void)uiprog_fill(&g_screen, rect->x, top, rect->w, end - top, wallpaper_color(band));
    }

    for (unsigned index = 0; index < g_model.count; index++) {
        window_paint((int)index, rect);
    }

    panel_paint(rect);
}

static struct ui_rect cursor_rect(int x, int y)
{
    struct ui_rect rect;

    rect.x = x;
    rect.y = y;
    rect.w = WM_ARROW_WIDTH;
    rect.h = WM_ARROW_ROWS;

    return rect;
}

static void cursor_paint(void)
{
    int row;

    for (row = 0; row < WM_ARROW_ROWS; row++) {
        int width = s_arrow[row];

        (void)uiprog_fill(&g_screen, g_pointer_x, g_pointer_y + row, width + 1, 1, WM_CURSOR_EDGE);
        (void)uiprog_fill(&g_screen, g_pointer_x, g_pointer_y + row, width, 1, WM_CURSOR_INK);
    }
}

/* The rectangles of the turn of the loop, and the pointer over them. */
static void flush_pending(void)
{
    int index;

    if (g_damage_count == 0) {
        return;
    }

    for (index = 0; index < g_damage_count; index++) {
        scene_paint(&g_damage[index]);
    }

    g_damage_count = 0;

    /*
     * The pointer is the last thing of the picture and the thing any damage
     * may have painted over: every turn that painted anything draws it again.
     * The bookkeeping of "did the damage touch the pointer" was one comparison
     * short of a lost cursor, and the pointer is the one thing a person needs
     * to see to know where a click will land.
     */
    cursor_paint();
}

/* --- the focus ------------------------------------------------------------ */

/*
 * The keys go to the window the model says, and a window that gains or loses
 * them is told: a client draws its title bar and its border differently and
 * does not have to guess. The id of the focused window is kept here, because
 * the index of the model moves with every raise.
 */
static int g_focus_id = -1;

static void focus_sync(void)
{
    int focus = ui_screen_input(&g_model);
    int id = focus == UI_WINDOW_NONE ? -1 : g_model.windows[focus].id;
    int index;

    if (id == g_focus_id) {
        return;
    }

    for (index = 0; index < WM_APPS; index++) {
        struct wm_message message;

        if (!g_apps[index].used || (g_apps[index].id != id && g_apps[index].id != g_focus_id)) {
            continue;
        }

        wm_message_focus(&message, g_apps[index].id == id);
        (void)write(g_apps[index].events, &message, sizeof(message));
    }

    /* The ring of the focus goes from one window to another: both repaint. */
    index = ui_screen_find(&g_model, g_focus_id);
    if (index != UI_WINDOW_NONE) {
        struct ui_rect ring = g_model.windows[index].rect;

        ring.x -= WM_FOCUS_RING;
        ring.y -= WM_FOCUS_RING;
        ring.w += 2 * WM_FOCUS_RING;
        ring.h += 2 * WM_FOCUS_RING;
        damage_add(ring.x, ring.y, ring.w, ring.h);
    }

    index = ui_screen_find(&g_model, id);
    if (index != UI_WINDOW_NONE) {
        struct ui_rect ring = g_model.windows[index].rect;

        ring.x -= WM_FOCUS_RING;
        ring.y -= WM_FOCUS_RING;
        ring.w += 2 * WM_FOCUS_RING;
        ring.h += 2 * WM_FOCUS_RING;
        damage_add(ring.x, ring.y, ring.w, ring.h);
    }

    g_focus_id = id;
}

/* --- the apps ------------------------------------------------------------- */

static void app_damage_window(const struct wm_app *app)
{
    damage_add(app->rect.x - WM_FOCUS_RING, app->rect.y - WM_FOCUS_RING,
               app->rect.w + 2 * WM_FOCUS_RING, app->rect.h + 2 * WM_FOCUS_RING);
}

static void app_raise(int index)
{
    struct wm_app *app = &g_apps[index];
    int model = ui_screen_find(&g_model, app->id);
    int raised;
    struct ui_rect all;
    int other;
    bool first = true;

    if (model == UI_WINDOW_NONE) {
        return;
    }

    /* A window that is on top and has the keys is what the click asked for:
     * the picture is not painted again for a click on the window in front. */
    if (model == (int)g_model.count - 1 && ui_screen_input(&g_model) == model) {
        return;
    }

    /* The raise moves the window and the focus index with it; the keys go to
     * the window a person clicked, which is the second half of the two. */
    raised = ui_screen_raise(&g_model, model);
    if (raised == UI_WINDOW_NONE) {
        return;
    }

    (void)ui_screen_focus(&g_model, raised);
    focus_sync();

    /* The order changed: the raised window and every window it covered are
     * painted again, which is the union of the rectangles of them all. */
    for (other = 0; other < (int)g_model.count; other++) {
        struct ui_rect rect = g_model.windows[other].rect;

        rect.x -= WM_FOCUS_RING;
        rect.y -= WM_FOCUS_RING;
        rect.w += 2 * WM_FOCUS_RING;
        rect.h += 2 * WM_FOCUS_RING;

        if (first) {
            all = rect;
            first = false;
        } else {
            int left = rect.x < all.x ? rect.x : all.x;
            int top = rect.y < all.y ? rect.y : all.y;
            int right = rect.x + rect.w > all.x + all.w ? rect.x + rect.w : all.x + all.w;
            int bottom = rect.y + rect.h > all.y + all.h ? rect.y + rect.h : all.y + all.h;

            all.x = left;
            all.y = top;
            all.w = right - left;
            all.h = bottom - top;
        }
    }

    if (!first) {
        damage_add(all.x, all.y, all.w, all.h);
    }

    say_title("wm: raise ", app->title);
}

/* Put a window away, or bring it back: the model holds the windows that are
 * on the screen, and a window of the panel is not in it. */
static void app_minimize(int index)
{
    struct wm_app *app = &g_apps[index];
    int model = ui_screen_find(&g_model, app->id);

    if (model == UI_WINDOW_NONE) {
        return;
    }

    app_damage_window(app);
    (void)ui_screen_remove(&g_model, model);
    focus_sync();
    damage_add(0, panel_top(), g_width, WM_PANEL_HEIGHT);
    say_title("wm: put away ", app->title);
}

static void app_restore(int index)
{
    struct wm_app *app = &g_apps[index];
    int model;

    if (ui_screen_find(&g_model, app->id) != UI_WINDOW_NONE) {
        return;
    }

    /*
     * A window that was dragged past the bottom of the picture and put away
     * from there comes back *inside* the picture: the model refuses a window
     * whose rectangle is not a part of the screen (ui_screen_add), so the
     * rectangle of the server is moved to the nearest place that fits and the
     * client is not asked to draw anything again - its buffer is its own and
     * the size of it did not change.
     */
    if (app->rect.w <= g_width) {
        if (app->rect.x > g_width - app->rect.w) {
            app->rect.x = g_width - app->rect.w;
        }
        if (app->rect.x < 0) {
            app->rect.x = 0;
        }
    }
    if (app->rect.h <= panel_top()) {
        if (app->rect.y > panel_top() - app->rect.h) {
            app->rect.y = panel_top() - app->rect.h;
        }
        if (app->rect.y < 0) {
            app->rect.y = 0;
        }
    }

    model = ui_screen_add(&g_model, app->id, app->title, &app->rect, 0);
    if (model == UI_WINDOW_NONE) {
        say("wm: the window did not come back\n");
        return;
    }

    focus_sync();
    app_damage_window(app);
    damage_add(0, panel_top(), g_width, WM_PANEL_HEIGHT);
    say_title("wm: restore ", app->title);
}

static void app_toggle(int index)
{
    if (ui_screen_find(&g_model, g_apps[index].id) == UI_WINDOW_NONE) {
        app_restore(index);
    } else {
        app_minimize(index);
    }
}

static void app_move(int index, int x, int y)
{
    struct wm_app *app = &g_apps[index];
    int model = ui_screen_find(&g_model, app->id);
    struct ui_rect old = app->rect;

    if (model == UI_WINDOW_NONE) {
        return;
    }

    app->rect.x = x;
    app->rect.y = y;
    g_model.windows[model].rect.x = x;
    g_model.windows[model].rect.y = y;

    damage_add(old.x - WM_FOCUS_RING, old.y - WM_FOCUS_RING, old.w + 2 * WM_FOCUS_RING,
               old.h + 2 * WM_FOCUS_RING);
    app_damage_window(app);

    {
        char line[160];
        int length = snprintf(line, sizeof(line), "wm: move %s to %d,%d\n", app->title, x, y);

        if (length > 0) {
            (void)write(ULIBC_STDOUT, line, (size_t)length);
        }
    }
}

/* The window is gone: its rectangle is paper again, its descriptors are
 * closed and the file of its pixels is removed. */
static void app_remove(int index)
{
    struct wm_app *app = &g_apps[index];
    int model;

    if (!app->used) {
        return;
    }

    model = ui_screen_find(&g_model, app->id);

    if (model != UI_WINDOW_NONE) {
        app_damage_window(app);
        (void)ui_screen_remove(&g_model, model);
    }

    damage_add(0, panel_top(), g_width, WM_PANEL_HEIGHT);

    if (app->events >= 0) {
        (void)close(app->events);
    }
    if (app->requests >= 0) {
        (void)close(app->requests);
    }
    if (app->buffer >= 0) {
        (void)close(app->buffer);
    }

    (void)unlink(app->path);

    say_title("wm: close ", app->title);

    app->used = false;
    app->events = -1;
    app->requests = -1;
    app->buffer = -1;

    focus_sync();
}

static void app_request_close(int index)
{
    struct wm_message message;
    struct wm_app *app = &g_apps[index];

    if (!app->used) {
        return;
    }

    wm_message_close(&message);
    (void)write(app->events, &message, sizeof(message));

    say_title("wm: ask to leave ", app->title);
}

/* The messages of one window: the damage of a frame, a title, or bye.
 *
 * One read per turn of the loop and only after the `poll` said that the
 * descriptor has bytes: a pipe of this system waits when it is empty instead
 * of answering EAGAIN (kernel/src/kfile.c, the reader of a pipeline), so a
 * loop that read until an error would wait for the next frame of a window
 * that has nothing to say. The client writes whole messages, so a read takes
 * a whole one and the next message is the next turn of the loop. */
static void app_requests(int index)
{
    struct wm_app *app = &g_apps[index];
    struct wm_message message;
    long got;

    got = read(app->requests, app->inbox + app->inbox_bytes, WM_MESSAGE_BYTES - app->inbox_bytes);

    if (got <= 0) {
        return;
    }

    app->inbox_bytes += (unsigned)got;

    if (app->inbox_bytes < WM_MESSAGE_BYTES) {
        return;
    }

    memcpy(&message, app->inbox, WM_MESSAGE_BYTES);
    app->inbox_bytes = 0;

    if (!wm_message_known(&message)) {
        return;
    }

    switch (message.type) {
    case WM_MSG_DAMAGE:
        damage_add(app->rect.x + message.x, app->rect.y + message.y, message.w, message.h);
        break;
    case WM_MSG_TITLE: {
        char title[WM_TITLE_MAX];
        int model;

        (void)snprintf(title, sizeof(title), "%s", message.text);

        if (strcmp(title, app->title) == 0) {
            break;
        }

        (void)snprintf(app->title, sizeof(app->title), "%s", title);

        model = ui_screen_find(&g_model, app->id);
        if (model != UI_WINDOW_NONE) {
            (void)snprintf(g_model.windows[model].title, sizeof(g_model.windows[model].title), "%s",
                           title);
        }

        damage_add(0, panel_top(), g_width, WM_PANEL_HEIGHT);
        break;
    }
    case WM_MSG_CLOSE:
        app_remove(index);
        break;
    default:
        break;
    }
}

/* --- the keys and the pointer --------------------------------------------- */

static void key_to_focus(k_u16_t code, k_u16_t value)
{
    int focus = ui_screen_input(&g_model);
    struct wm_message message;

    if (focus == UI_WINDOW_NONE) {
        return;
    }

    for (int index = 0; index < WM_APPS; index++) {
        if (g_apps[index].used && g_apps[index].id == g_model.windows[focus].id) {
            wm_message_key(&message, code, value == INPUT_VALUE_PRESS);
            (void)write(g_apps[index].events, &message, sizeof(message));
            return;
        }
    }
}

static void pointer_move(int dx, int dy)
{
    struct ui_rect old = cursor_rect(g_pointer_x, g_pointer_y);
    int x = g_pointer_x + dx;
    int y = g_pointer_y + dy;

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x > g_width - WM_ARROW_WIDTH) {
        x = g_width - WM_ARROW_WIDTH;
    }
    if (y > g_height - WM_ARROW_ROWS) {
        y = g_height - WM_ARROW_ROWS;
    }

    if (x == g_pointer_x && y == g_pointer_y) {
        return;
    }

    damage_add(old.x, old.y, old.w, old.h);

    g_pointer_x = x;
    g_pointer_y = y;

    damage_add(x, y, WM_ARROW_WIDTH, WM_ARROW_ROWS);

    /* The window under the pointer hears about the motion, in its own
     * pixels: a client that draws a pointer of its own gets it from here. */
    {
        int top = ui_screen_top_at(&g_model, x, y);

        if (top != UI_WINDOW_NONE) {
            for (int index = 0; index < WM_APPS; index++) {
                struct wm_message message;

                if (!g_apps[index].used || g_apps[index].id != g_model.windows[top].id) {
                    continue;
                }

                wm_message_motion(&message, x - g_apps[index].rect.x, y - g_apps[index].rect.y);
                (void)write(g_apps[index].events, &message, sizeof(message));
                break;
            }
        }
    }

    if (g_dragging && g_drag_app >= 0 && g_apps[g_drag_app].used) {
        int new_x = g_drag_rect.x + (x - g_drag_x);
        int new_y = g_drag_rect.y + (y - g_drag_y);
        int title = UI_FRAME_TITLE_HEIGHT(uiprog_font());

        /*
         * The handle of a window is its title bar, so the bar stays on the
         * picture: the whole height of it above the panel and at least
         * WM_TITLE_KEEP pixels of its width at either side. A window dragged
         * to an edge loses the part of itself that is past the edge, and never
         * the bar that can bring it back.
         */
        if (new_y > panel_top() - title) {
            new_y = panel_top() - title;
        }
        if (new_y < 0) {
            new_y = 0;
        }
        if (new_x > g_width - WM_TITLE_KEEP) {
            new_x = g_width - WM_TITLE_KEEP;
        }
        if (new_x < WM_TITLE_KEEP - g_drag_rect.w) {
            new_x = WM_TITLE_KEEP - g_drag_rect.w;
        }

        if (new_x != g_apps[g_drag_app].rect.x || new_y != g_apps[g_drag_app].rect.y) {
            app_move(g_drag_app, new_x, new_y);
        }
    }
}

/* The button of the pointer: a press finds what is under it - the panel, the
 * square of a window, its bar, or its body - and a release ends what the
 * press began. */
static void pointer_press(void)
{
    int top;

    g_button = true;

    if (g_pointer_y >= panel_top()) {
        for (int index = 0; index < WM_APPS; index++) {
            struct ui_rect button;
            struct ui_rect cross;

            if (!g_apps[index].used) {
                continue;
            }

            cross = panel_close_rect(index);
            if (ui_rect_contains(&cross, g_pointer_x, g_pointer_y)) {
                g_panel_press = index;
                g_panel_press_close = true;
                return;
            }

            button = panel_button_rect(index);

            if (ui_rect_contains(&button, g_pointer_x, g_pointer_y)) {
                g_panel_press = index;
                g_panel_press_close = false;
                return;
            }
        }

        return;
    }

    top = ui_screen_top_at(&g_model, g_pointer_x, g_pointer_y);

    if (top == UI_WINDOW_NONE) {
        return;
    }

    {
        const struct ui_window *window = &g_model.windows[top];
        struct ui_rect box = close_box_rect(window);
        int index;
        struct wm_app *app = NULL;

        for (index = 0; index < WM_APPS; index++) {
            if (g_apps[index].used && g_apps[index].id == window->id) {
                app = &g_apps[index];
                break;
            }
        }

        if (app == NULL) {
            return;
        }

        app_raise(index);

        /* The raise may have moved the index of the model: the question of
         * the square and of the bar is about the window, not the index. */
        top = ui_screen_find(&g_model, app->id);
        if (top == UI_WINDOW_NONE) {
            return;
        }

        window = &g_model.windows[top];

        if (ui_rect_contains(&box, g_pointer_x, g_pointer_y)) {
            /* The square of the bar closes the window on the release, as the
             * button of the panel does: a press that slides away is nothing. */
            g_close_press = index;
            return;
        }

        if (g_pointer_y < window->rect.y + UI_FRAME_TITLE_HEIGHT(uiprog_font())) {
            g_dragging = true;
            g_drag_app = index;
            g_drag_x = g_pointer_x;
            g_drag_y = g_pointer_y;
            g_drag_rect = app->rect;
            return;
        }

        g_press_app = index;

        {
            struct wm_message message;

            wm_message_button(&message, g_pointer_x - app->rect.x, g_pointer_y - app->rect.y, true);
            (void)write(app->events, &message, sizeof(message));
        }
    }
}

/* The thing a press went down on, if the release came back to it. */
static void panel_release(void)
{
    int index = g_panel_press;
    bool close = g_panel_press_close;

    g_panel_press = -1;

    if (index < 0 || index >= WM_APPS || !g_apps[index].used) {
        return;
    }

    if (close) {
        struct ui_rect cross = panel_close_rect(index);

        if (ui_rect_contains(&cross, g_pointer_x, g_pointer_y)) {
            app_request_close(index);
        }

        return;
    }

    {
        struct ui_rect button = panel_button_rect(index);

        if (ui_rect_contains(&button, g_pointer_x, g_pointer_y)) {
            app_toggle(index);
        }
    }
}

/* The square of the title bar: the release closes the window when it comes
 * back to the same square of the same window. */
static void close_box_release(void)
{
    int index = g_close_press;

    g_close_press = -1;

    if (index < 0 || index >= WM_APPS || !g_apps[index].used) {
        return;
    }

    {
        int model = ui_screen_find(&g_model, g_apps[index].id);

        if (model == UI_WINDOW_NONE) {
            return;
        }

        {
            struct ui_rect box = close_box_rect(&g_model.windows[model]);

            if (ui_rect_contains(&box, g_pointer_x, g_pointer_y)) {
                app_request_close(index);
            }
        }
    }
}

static void pointer_release(void)
{
    g_button = false;

    if (g_dragging) {
        g_dragging = false;
        g_drag_app = -1;
    }

    panel_release();
    close_box_release();

    if (g_press_app >= 0) {
        struct wm_app *app = &g_apps[g_press_app];

        if (app->used) {
            struct wm_message message;

            wm_message_button(&message, g_pointer_x - app->rect.x, g_pointer_y - app->rect.y,
                              false);
            (void)write(app->events, &message, sizeof(message));
        }

        g_press_app = -1;
    }
}

static void close_focused(void)
{
    int focus = ui_screen_input(&g_model);

    if (focus == UI_WINDOW_NONE) {
        return;
    }

    for (int index = 0; index < WM_APPS; index++) {
        if (g_apps[index].used && g_apps[index].id == g_model.windows[focus].id) {
            app_request_close(index);
            return;
        }
    }
}

static void event_handle(const struct input_event *event)
{

    if (event->code == K_INPUT_KEY_POINTER_X) {
        pointer_move((int)(k_i16_t)event->value, 0);
        return;
    }

    if (event->code == K_INPUT_KEY_POINTER_Y) {
        pointer_move(0, (int)(k_i16_t)event->value);
        return;
    }

    if (event->code == K_INPUT_KEY_BUTTON_LEFT) {
        if (event->value == INPUT_VALUE_PRESS) {
            pointer_press();
        } else {
            pointer_release();
        }
        return;
    }

    if (event->code == K_INPUT_KEY_ALT) {
        g_alt = event->value == INPUT_VALUE_PRESS;
        key_to_focus(event->code, event->value);
        return;
    }

    /* Alt+F4 is the key of the server: it closes the window that has the
     * focus and is not sent to it. */
    if (g_alt && event->code == K_INPUT_KEY_F4 && event->value == INPUT_VALUE_PRESS) {
        close_focused();
        return;
    }

    key_to_focus(event->code, event->value);
}

/* --- the windows the server starts ---------------------------------------- */

/* A file of /tmp with the pixels of a window: the client maps it, the server
 * reads it. The file is page sized, so that the last page of the mapping is
 * a page of the file and the write-back of a frame has somewhere to go. */
static int app_buffer_new(const char *path, int width, int height)
{
    size_t bytes = (size_t)width * (size_t)height * FB_PIXEL_BYTES;
    size_t size = (bytes + KSYS_PAGE_SIZE - 1u) / KSYS_PAGE_SIZE * KSYS_PAGE_SIZE;
    int fd;

    (void)unlink(path);

    fd = open(path, ULIBC_O_RDWR | ULIBC_O_CREAT | ULIBC_O_TRUNC);
    if (fd < 0) {
        return -1;
    }

    if (ftruncate(fd, (long long)size) != 0) {
        (void)close(fd);
        return -1;
    }

    return fd;
}

static void app_spawn(int index, const char *path, char *const argv[], const char *title, int x,
                      int y, int w, int h)
{
    struct wm_app *app = &g_apps[index];
    int down[2]; /* the server writes, the client reads */
    int up[2];   /* the client writes, the server reads */
    int buffer;
    long pid;
    char name[40];

    (void)snprintf(name, sizeof(name), "/tmp/wm%d.buf", index);

    buffer = app_buffer_new(name, w, h);
    if (buffer < 0) {
        say("wm: no file for the pixels of a window\n");
        return;
    }

    if (pipe(down) != 0 || pipe(up) != 0) {
        say("wm: no pipes for a window\n");
        (void)close(buffer);
        return;
    }

    pid = fork();

    if (pid < 0) {
        say("wm: fork refused the window\n");
        (void)close(down[0]);
        (void)close(down[1]);
        (void)close(up[0]);
        (void)close(up[1]);
        (void)close(buffer);
        return;
    }

    if (pid == 0) {
        char *const envp[] = {(char *)WM_ENV "="
                                             "1",
                              NULL};

        /* The three descriptors of the protocol, in the numbers of the
         * contract. The two ends of the other side are closed here: a client
         * that kept the write end of its own requests would never end. */
        if (dup2(down[0], WM_FD_EVENTS) < 0 || dup2(up[1], WM_FD_REQUESTS) < 0 ||
            dup2(buffer, WM_FD_BUFFER) < 0) {
            static const char no_fds[] = "wm: the descriptors of the window\n";

            (void)write(ULIBC_STDERR, no_fds, sizeof(no_fds) - 1u);
            _exit(1);
        }

        if (down[0] > WM_FD_BUFFER) {
            (void)close(down[0]);
        }
        if (up[1] > WM_FD_BUFFER) {
            (void)close(up[1]);
        }
        if (buffer > WM_FD_BUFFER) {
            (void)close(buffer);
        }

        (void)close(down[1]);
        (void)close(up[0]);

        /*
         * The child is a program of its own: the descriptors of the server go
         * - the screen it took, the keyboard it reads and the pipes of the
         * other windows - and the three numbers of the standard descriptors
         * are the console, so that the message of a program that fails is a
         * line of the serial and not a write into somebody else's pipe.
         */
        (void)close(ULIBC_STDIN);
        (void)close(ULIBC_STDOUT);
        (void)close(ULIBC_STDERR);

        for (int fd = WM_FD_BUFFER + 1; fd < 16; fd++) {
            (void)close(fd);
        }

        {
            int tty = open("/dev/tty0", ULIBC_O_WRONLY);

            if (tty == ULIBC_STDIN) {
                (void)dup2(tty, ULIBC_STDOUT);
                (void)dup2(tty, ULIBC_STDERR);
            }
        }

        (void)execve(path, argv, envp);

        {
            static const char no_start[] = "wm: the window did not start\n";

            (void)write(ULIBC_STDERR, no_start, sizeof(no_start) - 1u);
        }
        _exit(1);
    }

    (void)close(down[0]);
    (void)close(up[1]);

    memset(app, 0, sizeof(*app));
    app->used = true;
    app->id = WM_ID_BASE + index;
    app->pid = pid;
    app->events = down[1];
    app->requests = up[0];
    app->buffer = buffer;
    app->rect.x = x;
    app->rect.y = y;
    app->rect.w = w;
    app->rect.h = h;
    (void)snprintf(app->path, sizeof(app->path), "%s", name);
    (void)snprintf(app->title, sizeof(app->title), "%s", title);

    /* The hello is written before the first frame of the client: the window
     * it lays its picture out in, and the title of its bar. */
    {
        struct wm_message hello;

        wm_message_hello(&hello, x, y, w, h, title);
        (void)write(app->events, &hello, sizeof(hello));
    }

    if (ui_screen_add(&g_model, app->id, app->title, &app->rect, 0) == UI_WINDOW_NONE) {
        say("wm: the picture is too small for the window\n");
    }

    app_damage_window(app);
    damage_add(0, panel_top(), g_width, WM_PANEL_HEIGHT);
    g_windows_spawned++;

    /* The window that arrives last has the focus, and its client is told:
     * the ring of the focus and the bar of the panel follow the model. */
    focus_sync();

    say_title("wm: window ", title);
}

/* --- the loop ------------------------------------------------------------- */

/* The windows of the default desktop: the editor on a document and the file
 * manager on the root. Two programs the server owns, which the shell could
 * not start side by side. */
static void desktop_open(void)
{
    static char *const edit_argv[] = {(char *)"/bin/edit", (char *)"/doc.txt", NULL};
    static char *const fm_argv[] = {(char *)"/bin/fm", (char *)"/", NULL};

    /*
     * The two windows of the first desktop stand beside each other and not
     * over each other: a window that covers the corner of another one hides
     * the whole text of it - the list of the file manager is four columns
     * wide - and a raise would then look like a window that went blank. The
     * stacking is there for a person who puts one window over the other, and
     * `make x86-vga-smoke` puts one over the other before it checks the order
     * of the paint (docs/desktop.md).
     */
    app_spawn(0, "/bin/edit", edit_argv, "edit - /doc.txt", 40, 40, 640, 420);
    app_spawn(1, "/bin/fm", fm_argv, "fm - /", 700, 200, 520, 380);
}

/* The children that are gone: a window whose program exited without saying
 * so is a window of the server that is not there any more. */
static void children_reap(void)
{
    for (int index = 0; index < WM_APPS; index++) {
        int status = 0;

        if (!g_apps[index].used) {
            continue;
        }

        if (waitpid((int)g_apps[index].pid, &status, KSYS_WNOHANG) == (int)g_apps[index].pid) {
            app_remove(index);
        }
    }
}

static int live_windows(void)
{
    int count = 0;

    for (int index = 0; index < WM_APPS; index++) {
        if (g_apps[index].used) {
            count++;
        }
    }

    return count;
}

int main(int argc, char *const argv[], char *const envp[])
{
    struct ui_rect bounds;

    (void)argc;
    (void)argv;
    (void)envp;

    say("wm: start\n");

    if (uiprog_open(&g_screen, "wm") != 0) {
        return ULIBC_EXIT_FAILURE;
    }

    say("wm: the screen is mine\n");

    g_width = (int)g_screen.info.width;
    g_height = (int)g_screen.info.height;

    for (int index = 0; index < WM_APPS; index++) {
        g_apps[index].events = -1;
        g_apps[index].requests = -1;
        g_apps[index].buffer = -1;
    }

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = g_width;
    bounds.h = g_height;
    ui_screen_init(&g_model, &bounds);

    /* The pointer starts in the middle of the picture, which is a place a
     * person finds it in and a test can count from. */
    g_pointer_x = g_width / 2;
    g_pointer_y = g_height / 2;

    scene_paint(&(struct ui_rect){0, 0, g_width, g_height});
    cursor_paint();

    desktop_open();

    say("wm: the desktop is up\n");

    while (g_running) {
        struct pollfd entries[WM_APPS];
        int indices[WM_APPS];
        int count = 0;
        int ready;
        struct input_event event;

        /* The requests of the windows: one wait for all of them, so a silent
         * desktop costs one call and not one a window. The index of a window
         * travels with the entry of the poll: the handler of one window may
         * close another (a client that leaves while the loop is in the list),
         * and a slot counted again over `used` would then be the entry of the
         * wrong window. */
        for (int index = 0; index < WM_APPS; index++) {
            if (!g_apps[index].used) {
                continue;
            }

            entries[count].fd = g_apps[index].requests;
            entries[count].events = KSYS_POLLIN;
            entries[count].revents = 0;
            indices[count] = index;
            count++;
        }

        ready = poll(entries, (nfds_t)count, 8);

        if (ready > 0) {
            for (int slot = 0; slot < count; slot++) {
                if (entries[slot].revents != 0) {
                    app_requests(indices[slot]);
                }
            }
        }

        while (uiprog_event(&g_screen, &event) == 1) {
            event_handle(&event);
        }

        children_reap();
        flush_pending();

        /* The desktop ends with its last window: the screen goes back to the
         * grid of the shell, which is where a person started from. */
        if (g_windows_spawned > 0 && live_windows() == 0) {
            g_running = false;
        }
    }

    for (int index = 0; index < WM_APPS; index++) {
        if (g_apps[index].used) {
            app_remove(index);
        }
    }

    uiprog_close(&g_screen);

    say("wm: the desktop is closed, the screen goes back\n");

    return ULIBC_EXIT_SUCCESS;
}
