/*
 * Micro OS — the model of the windows of a screen (ui/window.h).
 *
 * The order of the array is the order of the paint, and the indices are the
 * only place a position lives: raising a window moves the structs after it,
 * and the focus and the modal window move with them. That is the whole of
 * the arithmetic of the model, and it is why the file has a test of its own
 * on the host: a focus that points at a window which is not there is the
 * kind of mistake a screenshot only shows as "the wrong window answered".
 */
#include "ui/window.h"

#include <string.h>

bool ui_rect_contains(const struct ui_rect *rect, int x, int y)
{
    if (rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return false;
    }

    return x >= rect->x && y >= rect->y && x < rect->x + rect->w && y < rect->y + rect->h;
}

bool ui_rect_intersect(const struct ui_rect *a, const struct ui_rect *b, struct ui_rect *out)
{
    int left;
    int top;
    int right;
    int bottom;

    if (a == NULL || b == NULL || out == NULL) {
        return false;
    }

    left = a->x > b->x ? a->x : b->x;
    top = a->y > b->y ? a->y : b->y;
    right = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    bottom = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    if (right <= left || bottom <= top) {
        return false;
    }

    out->x = left;
    out->y = top;
    out->w = right - left;
    out->h = bottom - top;
    return true;
}

bool ui_rect_fits(const struct ui_rect *rect, const struct ui_rect *bounds)
{
    if (rect == NULL || bounds == NULL || rect->w <= 0 || rect->h <= 0) {
        return false;
    }

    return rect->x >= bounds->x && rect->y >= bounds->y &&
           rect->x + rect->w <= bounds->x + bounds->w && rect->y + rect->h <= bounds->y + bounds->h;
}

bool ui_window_rect(const struct ui_window *window, const struct ui_rect *bounds,
                    struct ui_rect *out)
{
    if (window == NULL || out == NULL || !ui_rect_fits(&window->rect, bounds)) {
        return false;
    }

    *out = window->rect;
    return true;
}

void ui_screen_init(struct ui_screen *screen, const struct ui_rect *bounds)
{
    if (screen == NULL) {
        return;
    }

    memset(screen, 0, sizeof(*screen));

    if (bounds != NULL) {
        screen->bounds = *bounds;
    }

    screen->focus = UI_WINDOW_NONE;
    screen->modal = UI_WINDOW_NONE;
}

static bool screen_member(const struct ui_screen *screen, int index)
{
    return screen != NULL && index >= 0 && (unsigned)index < screen->count;
}

int ui_screen_find(const struct ui_screen *screen, int id)
{
    unsigned index;

    if (screen == NULL) {
        return UI_WINDOW_NONE;
    }

    for (index = 0; index < screen->count; index++) {
        if (screen->windows[index].id == id) {
            return (int)index;
        }
    }

    return UI_WINDOW_NONE;
}

int ui_screen_add(struct ui_screen *screen, int id, const char *title, const struct ui_rect *rect,
                  unsigned flags)
{
    struct ui_window *window;

    if (screen == NULL || title == NULL || title[0] == '\0' || rect == NULL) {
        return UI_WINDOW_NONE;
    }

    if (screen->count >= UI_WINDOW_MAX || !ui_rect_fits(rect, &screen->bounds)) {
        return UI_WINDOW_NONE;
    }

    if (ui_screen_find(screen, id) != UI_WINDOW_NONE) {
        return UI_WINDOW_NONE;
    }

    /* A window that is not modal is not above a modal one: the keys belong to
     * the dialog until the program clears it. */
    if (screen->modal != UI_WINDOW_NONE && (flags & UI_WINDOW_MODAL) == 0u) {
        return UI_WINDOW_NONE;
    }

    window = &screen->windows[screen->count];
    memset(window, 0, sizeof(*window));
    window->id = id;
    window->rect = *rect;
    window->flags = flags & UI_WINDOW_MODAL;

    /* A title that does not fit is cut, not refused: it is a label. */
    memcpy(window->title, title, strlen(title) < UI_TITLE_MAX ? strlen(title) : UI_TITLE_MAX - 1u);

    screen->count++;
    screen->focus = (int)screen->count - 1;

    if ((window->flags & UI_WINDOW_MODAL) != 0u) {
        screen->modal = screen->focus;
    }

    return screen->focus;
}

int ui_screen_remove(struct ui_screen *screen, int index)
{
    if (!screen_member(screen, index)) {
        return UI_WINDOW_NONE;
    }

    if (screen->count > 1u) {
        memmove(&screen->windows[index], &screen->windows[index + 1],
                (screen->count - (unsigned)index - 1u) * sizeof(screen->windows[0]));
    }

    screen->count--;
    memset(&screen->windows[screen->count], 0, sizeof(screen->windows[0]));

    if (screen->modal == index) {
        screen->modal = UI_WINDOW_NONE;
    } else if (screen->modal > index) {
        screen->modal--;
    }

    /* The window that took the keys is gone: the top of what is left has
     * them, which is the window a person sees first. */
    if (screen->focus == index) {
        screen->focus = screen->count > 0u ? (int)screen->count - 1 : UI_WINDOW_NONE;
    } else if (screen->focus > index) {
        screen->focus--;
    }

    return 0;
}

int ui_screen_raise(struct ui_screen *screen, int index)
{
    struct ui_window window;
    unsigned last;

    if (!screen_member(screen, index)) {
        return UI_WINDOW_NONE;
    }

    if (screen->modal != UI_WINDOW_NONE && index != screen->modal) {
        return UI_WINDOW_NONE;
    }

    if ((unsigned)index == screen->count - 1u) {
        return index; /* already on top */
    }

    last = screen->count - 1u;
    window = screen->windows[index];
    memmove(&screen->windows[index], &screen->windows[index + 1],
            (last - (unsigned)index) * sizeof(screen->windows[0]));
    screen->windows[last] = window;

    /* The structs after the raised one moved down by one; it is the last. */
    if (screen->focus == index) {
        screen->focus = (int)last;
    } else if (screen->focus > index) {
        screen->focus--;
    }
    if (screen->modal == index) {
        screen->modal = (int)last;
    } else if (screen->modal > index) {
        screen->modal--;
    }

    return (int)last;
}

int ui_screen_focus_next(struct ui_screen *screen, int step)
{
    int count;

    if (screen == NULL || screen->count == 0u) {
        return UI_WINDOW_NONE;
    }

    if (screen->modal != UI_WINDOW_NONE) {
        screen->focus = screen->modal;
        return screen->focus;
    }

    count = (int)screen->count;

    if (screen->focus < 0 || screen->focus >= count) {
        screen->focus = step >= 0 ? 0 : count - 1;
        return screen->focus;
    }

    screen->focus = (screen->focus + (step >= 0 ? 1 : -1) + count) % count;

    return screen->focus;
}

bool ui_screen_focus(struct ui_screen *screen, int index)
{
    if (!screen_member(screen, index) || screen->modal != UI_WINDOW_NONE) {
        return false;
    }

    screen->focus = index;
    return true;
}

int ui_screen_set_modal(struct ui_screen *screen, int index)
{
    int raised;

    if (!screen_member(screen, index) || screen->modal != UI_WINDOW_NONE) {
        return UI_WINDOW_NONE;
    }

    raised = ui_screen_raise(screen, index);
    if (raised == UI_WINDOW_NONE) {
        return UI_WINDOW_NONE;
    }

    screen->windows[raised].flags |= UI_WINDOW_MODAL;
    screen->modal = raised;
    screen->focus = raised;

    return raised;
}

void ui_screen_clear_modal(struct ui_screen *screen)
{
    if (screen == NULL || !screen_member(screen, screen->modal)) {
        return;
    }

    screen->windows[screen->modal].flags &= ~(unsigned)UI_WINDOW_MODAL;
    screen->modal = UI_WINDOW_NONE;
}

int ui_screen_input(const struct ui_screen *screen)
{
    if (screen == NULL) {
        return UI_WINDOW_NONE;
    }

    if (screen_member(screen, screen->modal)) {
        return screen->modal;
    }

    return screen_member(screen, screen->focus) ? screen->focus : UI_WINDOW_NONE;
}

int ui_screen_top_at(const struct ui_screen *screen, int x, int y)
{
    int index;

    if (screen == NULL) {
        return UI_WINDOW_NONE;
    }

    for (index = (int)screen->count - 1; index >= 0; index--) {
        if (ui_rect_contains(&screen->windows[index].rect, x, y)) {
            return index;
        }
    }

    return UI_WINDOW_NONE;
}
