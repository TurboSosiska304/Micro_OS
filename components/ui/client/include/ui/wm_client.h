/*
 * Micro OS — the window of a program (ADR 0030, docs/desktop.md).
 *
 * A program of the desktop draws into a buffer of its own - a rectangle of
 * pixels of the window - and the server of the desktop (`/bin/wm`) copies
 * that buffer into its picture. The program never touches `/dev/fb0` and
 * never sees a key of the machine: it gets the events of its window from a
 * pipe, and it says what changed through the same pipe.
 *
 * This file is the half of that contract that is portable: the messages, the
 * clipping of a rectangle into the buffer, the damage that a paint produces
 * and the state of the events. The transport is the caller's (`struct
 * wm_link`): on the machine it is three descriptors of the ABI and a mapping
 * of a file, in a test of the host it is two arrays. Nothing here allocates
 * and nothing here knows what a pipe is, so the whole of the client is held
 * by `tests/cases/test_ui_wm.c`.
 *
 * The buffer is the `MAP_SHARED` mapping of a file of the server: a write of
 * the program reaches the file, and the server reads the file. The kernel of
 * 1.7 gives every mapping its own frame and writes it back on `msync`, so the
 * link's `sync` is what makes a frame visible and the client calls it before
 * it says that the buffer changed.
 */
#ifndef MICRO_OS_UI_WM_CLIENT_H
#define MICRO_OS_UI_WM_CLIENT_H

#include <stdbool.h>

#include "kernel/input.h"
#include "kernel/types.h"
#include "ui/draw.h"
#include "ui/window.h"
#include "ui/wm_protocol.h"

/*
 * The transport of a client. `map` makes the buffer of the window (the pixels
 * of `width` by `height` of the window, a row after another) and answers 0;
 * `sync` makes the bytes of the buffer visible to the server; `send` and
 * `recv` move one message. A `recv` with `wait` false answers 1 for a
 * message, 0 when there is none and a negative number of the ABI on failure;
 * with `wait` true it waits for one.
 */
struct wm_link {
    void *context;
    int (*map)(void *context, int width, int height, k_u16_t **pixels);
    int (*sync)(void *context);
    int (*send)(void *context, const struct wm_message *message);
    int (*recv)(void *context, struct wm_message *message, bool wait);
};

/* The client of one window. The buffer is the link's and the pixels are its
 * pointer; the target is what `ui/draw.h` draws into and the damage is the
 * bounding box of what changed since the last flush. */
struct wm_client {
    struct wm_link *link;
    struct ui_target target;
    k_u16_t *pixels;
    int width;
    int height;
    struct ui_rect geometry; /* the window in the picture of the screen */
    char title[WM_TITLE_MAX];
    struct ui_rect damage;
    bool damaged;
    bool focused;
    bool closing;  /* the server asked this window to leave */
    int pointer_x; /* the pointer of the last motion, in the window */
    int pointer_y;
    bool button;     /* the left button of the last event of it */
    unsigned frames; /* the damage messages sent, for a test and a report */
    unsigned events; /* the events received from the server */
};

/*
 * The window the server gave this program: the hello of the pipe, the buffer
 * through the link, the geometry. The answer is 0, or a negative number of
 * the ABI when the hello did not arrive or the buffer did not map.
 */
int wm_client_attach(struct wm_client *client, struct wm_link *link);

/*
 * The two calls of the target: a rectangle of one colour and a rectangle of
 * pixels, both clipped to the window (a drawing that composes clips, the
 * device of ADR 0028 refuses). The damage grows with every call.
 */
int wm_client_fill(void *context, const struct ui_rect *rect, k_u16_t color);
int wm_client_blit(void *context, const struct ui_rect *rect, const k_u16_t *pixels);

/* A rectangle of `color` over the whole window, the frame of a program that
 * paints its picture from nothing. */
int wm_client_clear(struct wm_client *client, k_u16_t color);

/*
 * Tell the server what changed: the mapping is made visible and one message
 * of the damage is sent. Nothing is sent when nothing changed, which is what
 * keeps a client of a still picture off the pipe. The answer is the number of
 * messages sent (0 or 1) and a negative number of the ABI on failure.
 */
int wm_client_flush(struct wm_client *client);

/* The text of the title bar, and the button of the panel. */
int wm_client_set_title(struct wm_client *client, const char *title);

/* This window is leaving: the server removes it and the focus moves. */
int wm_client_request_close(struct wm_client *client);

/*
 * One event, as the program of a keyboard sees it: 1 when `event` was filled,
 * 0 when the server is silent. The keys arrive as the keys of the machine,
 * with the press and the release; the motion and the button only change the
 * state of the client (`pointer_x`, `pointer_y`, `button`), and a close is
 * `closing`.
 */
int wm_client_poll(struct wm_client *client, struct input_event *event);

/* True when the server asked this window to leave (Alt+F4, the button of the
 * title bar, the panel). The program answers it the way it answers its own
 * key of leaving. */
bool wm_client_closing(const struct wm_client *client);

#endif /* MICRO_OS_UI_WM_CLIENT_H */
