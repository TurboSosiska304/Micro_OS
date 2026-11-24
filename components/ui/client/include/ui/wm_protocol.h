/*
 * Micro OS — the protocol of the window server (docs/desktop.md).
 *
 * A window is a program of ring 3 that owns a buffer in the memory of the
 * system and a pipe to `/bin/wm`. The server writes the picture of the buffer
 * into the screen; the program writes the picture of its window into the
 * buffer and tells the server which part of it changed. The two speak the
 * messages below and nothing else: a fixed struct per message, one write per
 * message, so a reader that reads a whole message never sees half of one.
 *
 * The struct is the shape of the ABI (`struct input_event`, `struct fb_rect`):
 * native layout of one machine, no serialisation. Both sides are programs of
 * the same machine and of the same compiler, so a second encoding would be a
 * second thing to test for nothing.
 *
 * The direction of a message is the type of it: a request goes up to the
 * server, an event comes down to the window. `WM_MSG_CLOSE` is the one type
 * of both directions - the program says it is leaving, the server asks it to
 * leave - and the direction is which of the two wrote it.
 */
#ifndef MICRO_OS_UI_WM_PROTOCOL_H
#define MICRO_OS_UI_WM_PROTOCOL_H

#include "kernel/types.h"

/* The title with its terminator, the same length the window model keeps
 * (`UI_TITLE_MAX`): a title of /bin/wm is a line of a panel and of a bar. */
#define WM_TITLE_MAX 40

/* The bytes of one message, fixed for the life of the protocol: a pipe
 * delivers a message whole and a reader that reads less than this keeps the
 * bytes for the next call (src/wm_client.c). */
#define WM_MESSAGE_BYTES 60u

/* The descriptions of the three descriptors a client is given (the server
 * dup2()s them before the execve, so the numbers are the interface). */
#define WM_FD_EVENTS 3   /* server -> client: the events of the window */
#define WM_FD_REQUESTS 4 /* client -> server: its damage, title and close */
#define WM_FD_BUFFER 5   /* the pixels of the window, mmap(2) MAP_SHARED */

/* The variable that says a program was started as a window: its value is the
 * number of the protocol version the server speaks. A program without it is
 * a program of the whole screen (ADR 0028) and opens /dev/fb0. */
#define WM_ENV "MICROOS_WM"
#define WM_PROTOCOL_VERSION 1

/* The messages. A number is never reused: a client of an older server is a
 * client that does not know a type and ignores it. */
#define WM_MSG_NONE 0u
/* Up: the pixels of a rectangle of the buffer changed. */
#define WM_MSG_DAMAGE 1u
/* Up: the text of the title bar (and of the button of the panel). */
#define WM_MSG_TITLE 2u
/* Up and down: this window is leaving, or is asked to leave. */
#define WM_MSG_CLOSE 3u
/* Down, first: the window the server gave the program - rect and title. */
#define WM_MSG_HELLO 4u
/* Down: the keyboard went to (1) or left (0) the window. */
#define WM_MSG_FOCUS 5u
/* Down: a key of the machine, `code` and `x` = 1 pressed, 0 released. */
#define WM_MSG_KEY 6u
/* Down: the pointer, in pixels of the window (x, y). */
#define WM_MSG_MOTION 7u
/* Down: the left button of the pointer, `x` = 1 pressed, 0 released. */
#define WM_MSG_BUTTON 8u
#define WM_MSG_TYPE_MAX 8u

/*
 * One message. `code` is the key of a `WM_MSG_KEY`; `x`..`h` are a rectangle
 * for a damage or the hello, the position of the pointer for a motion, and
 * the edge of a key or of a button; `text` is the title. A type that needs
 * two things in one field (the edge of the button and its point) uses `x`
 * for the edge and `y`/`w` for the point, which the builders below document.
 */
struct wm_message {
    k_u16_t type;
    k_u16_t code;
    k_i32_t x;
    k_i32_t y;
    k_i32_t w;
    k_i32_t h;
    char text[WM_TITLE_MAX];
};

/*
 * A text of a message into a buffer of `size` bytes, cut to fit and always
 * terminated: the title of a window is a line of a bar and a text too long for
 * it is a title a person reads the beginning of, never a write past the end.
 */
void wm_text_copy(char *destination, k_size_t size, const char *source);

/* The message of a damage: the rectangle of the buffer that changed. */
void wm_message_damage(struct wm_message *message, int x, int y, int w, int h);

/* The message of a title; a text longer than the field is cut. */
void wm_message_title(struct wm_message *message, const char *title);

/* The message of a closing window. */
void wm_message_close(struct wm_message *message);

/* The hello of the server: the rectangle of the window in the picture and the
 * title the server knows the program by. */
void wm_message_hello(struct wm_message *message, int x, int y, int w, int h, const char *title);

/* The focus of the window: true when it now takes the keys. */
void wm_message_focus(struct wm_message *message, bool focused);

/* A key of the machine: the number of `kernel/input.h` and its edge. */
void wm_message_key(struct wm_message *message, k_u16_t code, bool pressed);

/* The pointer at a point of the window and the edge of its left button. */
void wm_message_motion(struct wm_message *message, int x, int y);
void wm_message_button(struct wm_message *message, int x, int y, bool pressed);

/* True when the type is one of the protocol: a message of a number this
 * generation does not know is ignored, not refused. */
bool wm_message_known(const struct wm_message *message);

/* The direction of a type: true for the events the server sends down. */
bool wm_message_is_event(k_u16_t type);

#endif /* MICRO_OS_UI_WM_PROTOCOL_H */
