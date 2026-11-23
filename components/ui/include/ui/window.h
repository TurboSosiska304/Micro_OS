/*
 * Micro OS — the windows of a program (ADR 0030).
 *
 * A window is a rectangle of the picture, a title and a place in the order:
 * the list of the windows of a screen is the order they are painted in, the
 * last one on top. The model is a program's and not the kernel's - a kernel
 * that knows what a window is cannot be ported to a machine that wants a
 * different one (ADR 0028 §4) - so it lives in a library of the userland and
 * a test on the host holds it without a screen.
 *
 * The library answers the questions an interface has: which window has the
 * focus, which one is on top at a point, which one takes the keys while a
 * modal window is up, and what happens to the focus when a window goes away.
 * It draws nothing: `ui/draw.h` is the picture of these rectangles.
 *
 * Indices are positions in the order and move when a window is raised or
 * removed; `id` is the program's own name of a window and never moves. A
 * program that keeps a window across a raise keeps its id and finds the
 * index again with ui_screen_find().
 */
#ifndef MICRO_OS_UI_WINDOW_H
#define MICRO_OS_UI_WINDOW_H

#include <stdbool.h>

#include "kernel/types.h"

/* The windows of one screen. Sixteen is more than an interface of this size
 * has ever wanted and keeps the model a value, not an allocation. */
#define UI_WINDOW_MAX 16

/* The title with its terminator. The text is cut, never refused. */
#define UI_TITLE_MAX 40

/* "No window": the answer of a search that found nothing. */
#define UI_WINDOW_NONE (-1)

/* A rectangle of the picture, in pixels, the top left pixel inside it. */
struct ui_rect {
    int x;
    int y;
    int w;
    int h;
};

/* The flags of a window. A modal window takes the keys of the screen until
 * it is cleared: the window under it is drawn and does not answer. */
#define UI_WINDOW_MODAL 0x01u

struct ui_window {
    int id;                   /* the program's name of this window */
    char title[UI_TITLE_MAX]; /* the text of its title bar */
    struct ui_rect rect;      /* where it is, in pixels of the picture */
    unsigned flags;           /* UI_WINDOW_MODAL */
};

struct ui_screen {
    struct ui_rect bounds;                   /* the picture it lives in */
    struct ui_window windows[UI_WINDOW_MAX]; /* the order of the paint */
    unsigned count;                          /* windows in use */
    int focus;                               /* index, or UI_WINDOW_NONE */
    int modal;                               /* index, or UI_WINDOW_NONE */
};

/* --- rectangles ---------------------------------------------------------- */

/* The point is inside the rectangle: the left and the top edge belong to it,
 * the right and the bottom one do not. */
bool ui_rect_contains(const struct ui_rect *rect, int x, int y);

/* The common part of two rectangles, 0 for the area of both. A false answer
 * means they do not touch and `out` was not written. */
bool ui_rect_intersect(const struct ui_rect *a, const struct ui_rect *b, struct ui_rect *out);

/* The rectangle is whole inside `bounds`: the same rule as the device of the
 * picture, which refuses a rectangle that leaves the screen. */
bool ui_rect_fits(const struct ui_rect *rect, const struct ui_rect *bounds);

/* The window's rectangle as the area of the picture it occupies: the answer
 * is false for a window whose rectangle is not a part of the picture. */
bool ui_window_rect(const struct ui_window *window, const struct ui_rect *bounds,
                    struct ui_rect *out);

/* --- the screen ---------------------------------------------------------- */

/* An empty screen of the picture `bounds`: no window, no focus, no modal. */
void ui_screen_init(struct ui_screen *screen, const struct ui_rect *bounds);

/*
 * A window on top with the focus. The id is the caller's and must be unique;
 * a rectangle that leaves the bounds, a title of no length and a duplicate id
 * are refused with UI_WINDOW_NONE. `flags` is UI_WINDOW_MODAL or 0: a modal
 * window takes the focus as it arrives and stays until it is cleared, and a
 * window that is not modal is refused while one is up.
 *
 * The answer is the index of the new window, which is the top of the order.
 */
int ui_screen_add(struct ui_screen *screen, int id, const char *title, const struct ui_rect *rect,
                  unsigned flags);

/* The index of the window with this id, UI_WINDOW_NONE when there is none. */
int ui_screen_find(const struct ui_screen *screen, int id);

/*
 * The window goes away. The focus moves to the top of what is left, the modal
 * flag clears with it, and the indices after it move down by one. An index
 * that is not a window answers UI_WINDOW_NONE and changes nothing.
 */
int ui_screen_remove(struct ui_screen *screen, int index);

/* The window comes to the top of the order and takes the focus; the answer is
 * its new index, UI_WINDOW_NONE when it is not there or when a modal window
 * of the screen holds the keys. */
int ui_screen_raise(struct ui_screen *screen, int index);

/*
 * The focus moves `step` windows along the order (1 forward, -1 back, any
 * other number is a step of one) and wraps. The answer is the new focus, or
 * the modal window while one is up - the keys are not the screen's then.
 */
int ui_screen_focus_next(struct ui_screen *screen, int step);

/* The focus moves to this window; the window is not raised. False when the
 * index is not a window or a modal window holds the keys. */
bool ui_screen_focus(struct ui_screen *screen, int index);

/*
 * The window becomes the modal one: it takes the top of the order, the focus
 * and the flag, and the screen answers UI_WINDOW_NONE for another modal until
 * it is cleared. The answer is its index, or UI_WINDOW_NONE.
 */
int ui_screen_set_modal(struct ui_screen *screen, int index);

/* The modal window stops being modal and stops having the flag. It stays on
 * the screen and in the order, with the focus, so a program removes it after
 * this or hands the focus to another window itself. */
void ui_screen_clear_modal(struct ui_screen *screen);

/*
 * The window that takes the keys: the modal one, or the one with the focus.
 * UI_WINDOW_NONE for a screen without windows.
 */
int ui_screen_input(const struct ui_screen *screen);

/*
 * The topmost visible window at a point of the picture, UI_WINDOW_NONE when
 * the point is the paper of no window. This is a question of the picture and
 * not of the keys: a modal window is on top of the order and is the answer
 * over its own rectangle, not over the whole screen.
 */
int ui_screen_top_at(const struct ui_screen *screen, int x, int y);

#endif /* MICRO_OS_UI_WINDOW_H */
