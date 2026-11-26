/*
 * Micro OS — the screen and the keyboard of a program of the interface.
 *
 * `/bin/ui`, `/bin/edit` and `/bin/fm` are three programs and one machine:
 * they open `/dev/fb0` and `/dev/input0`, take the screen with FBIO_TAKE,
 * compose a rectangle in a buffer and hand it to the device one transfer at a
 * time, and give the picture back when they end. That code is the port's and
 * not the library's - a write of `/dev/fb0` is the ABI of this machine - so
 * it lives here, in one file the programs link, and `components/ui` stays
 * portable and testable on the host.
 *
 * Since 1.7 the same program is also a window of the desktop (docs/desktop.md).
 * `/bin/wm` starts it with the variable `MICROOS_WM` in its environment, three
 * descriptors (the events, the requests and the buffer) and the first message
 * of the protocol; the program draws into the buffer of its window instead of
 * the screen and gets the keys of its own window instead of the machine. The
 * drawing, the layout and the keys of the program do not change: the two
 * modes are the same functions of this struct over two targets.
 *
 * A text row is a canvas of `ui/draw.h` in the buffer of this struct: the
 * cell is the 8x16 font of the console (`port_x86_64_font.h`), so the letters
 * of an interface are the letters of the shell and a screenshot is read with
 * one table. Nothing here allocates: the buffers are part of the bss of the
 * program, which the kernel clears at the start of the image, and the buffer
 * of a window is a mapping of the file the server made.
 */
#ifndef MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H
#define MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H

#include "kernel/fb.h"
#include "kernel/input.h"
#include "kernel/ksys.h"
#include "kernel/types.h"
#include "port_x86_64_font.h"
#include "ui/clock.h"
#include "ui/draw.h"
#include "ui/palette.h"
#include "ui/window.h"
#include "ui/wm_client.h"

/* The widest text row of a program: the picture of ADR 0023 is 160 cells. */
#define UIPROG_COLS_MAX 160

/* The pixels of one row of text: the widest line at the cell of the font. */
#define UIPROG_LINE_PIXELS (UIPROG_COLS_MAX * FONT8_WIDTH * FONT8_HEIGHT)

struct uiprog {
    int fb;
    int input;
    struct fb_info info;
    struct ui_target target; /* fill and blit, context = this */
    k_u16_t line[UIPROG_LINE_PIXELS];
    unsigned char transfer[KSYS_MAX_IO];

    /* The window of the desktop (1.7): the mode, the client of the protocol
     * and the transport of its three descriptors. A program of the whole
     * screen leaves `desktop` false and every field below unused. */
    bool desktop;
    bool close_requested;
    struct wm_client client;
    struct wm_link link;
    void *mapping;
    k_size_t mapping_size;
    unsigned char inbox[WM_MESSAGE_BYTES];
    unsigned inbox_bytes;
    int pointer_x;
    int pointer_y;
    bool pointer_button;
};

/*
 * The two devices and the screen, or the window of the desktop when the
 * program was started by `/bin/wm` (the variable `MICROOS_WM` in its
 * environment). The program says its name for the line of a failure. The
 * answer is 0, or a negative number of the ABI: a machine without a picture
 * (`/dev/fb0` is absent in the text mode of the VGA) or without the keyboard
 * is a machine where an interface cannot run, and the reason is on the
 * console.
 */
int uiprog_open(struct uiprog *program, const char *name);

/*
 * The reverse and the explicit end: in the desktop the window says it is
 * leaving and the buffer goes back; on the screen the keyboard goes back to
 * the console, then the picture to the kernel, which repaints the grid of the
 * shell. A close does the same for a program that never gets here.
 */
void uiprog_close(struct uiprog *program);

/* The columns and the rows of the picture (or of the window) in cells. */
int uiprog_columns(const struct uiprog *program);
int uiprog_rows(const struct uiprog *program);

/* The font of the console: the glyph function of the port and its cell. */
const struct ui_font *uiprog_font(void);

/*
 * The same glyphs at twice the cell: the second size of a program of the
 * desktop (ADR 0054). A machine has one table of glyphs - 95 rows of eight
 * bits - and a 16x32 cell is that table drawn two pixels a bit, which is what
 * the terminal of the window of the shell uses for its large text.
 */
const struct ui_font *uiprog_font_large(void);

/*
 * The palette of the desktop: the roles of `ui/palette.h` filled from the
 * `theme` of `/etc/console`, so one word of the file colours the console and
 * the desktop of the machine alike. The answer is the name of the theme,
 * "light" or "dark", which is what a program writes in its log; a machine
 * without the file, or with a value this version does not know, gets the dark
 * theme and the default of the file.
 */
const char *uiprog_palette(struct ui_palette *palette);

/*
 * The clock of the panel: the `clock` and `utc_offset` keys of the same
 * `/etc/console`, read with `ui/clock.h`. A file without them is the
 * twenty-four hours of UTC, which is the clock of the console log.
 */
void uiprog_clock(struct ui_clock *clock);

/* A rectangle of one colour, and a row of text as one rectangle of the device
 * or of the window. Both cut a rectangle that is too wide for one transfer of
 * the system into strips of whole rows. */
int uiprog_fill(struct uiprog *program, int x, int y, int w, int h, k_u16_t color);
int uiprog_text(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                k_u16_t paper);

/* A row of text in the font of the caller, which may be the large one: the
 * rectangle is `cells` cells of that font, and a row wider than the scratch of
 * the program is refused (-1) rather than drawn in pieces. */
int uiprog_text_font(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                     k_u16_t paper, const struct ui_font *font);

/* A row of text in the two colours traded: the row of a list that has the
 * focus. */
int uiprog_text_reverse(struct uiprog *program, int x, int y, const char *text, int cells,
                        k_u16_t ink, k_u16_t paper);

/* A rectangle of pixels: the picture of a client of the desktop (the server
 * is the one program that blits somebody else's pixels into a window). */
int uiprog_blit(struct uiprog *program, int x, int y, int w, int h, const k_u16_t *pixels);

/*
 * One event of the keyboard: 1 when it was read, 0 when the user is silent, a
 * negative number when the device failed. The caller naps on 0; a loop that
 * treats "nothing yet" as an error spins.
 */
int uiprog_event(struct uiprog *program, struct input_event *event);

/* Fill the whole picture with one colour: the frame a program paints first. */
void uiprog_clear(struct uiprog *program, k_u16_t color);

/*
 * The window of the desktop, or NULL for a program of the whole screen. A
 * program asks it to put a title in the bar of the server, to send the damage
 * of a frame (the caller flushes once per loop, after its keys), and to know
 * that the server asked it to leave.
 */
struct wm_client *uiprog_window(struct uiprog *program);
int uiprog_title(struct uiprog *program, const char *title);
int uiprog_flush(struct uiprog *program);
bool uiprog_close_requested(const struct uiprog *program);

/* The pointer of the desktop in pixels of the window, and the left button:
 * false when the program is not a window (the machine has no pointer of its
 * own). The programs of 1.7 do not draw a pointer; a client of the next
 * version does. */
bool uiprog_pointer(const struct uiprog *program, int *x, int *y, bool *pressed);

#endif /* MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H */
