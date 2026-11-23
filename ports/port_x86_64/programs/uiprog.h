/*
 * Micro OS — the screen and the keyboard of a program of the interface.
 *
 * `/bin/ui`, `/bin/edit` and `/bin/fm` are three programs and one machine:
 * they open `/dev/fb0` and `/dev/input0`, take the screen with FBIO_TAKE,
 * compose a rectangle in a buffer and hand it to the device one transfer at a
 * time, and give the picture back when they end. That code is the port's and
 * not the library's - a write of `/dev/fb0` is the ABI of this machine - so
 * it lives here, in one file the three programs link, and `components/ui`
 * stays portable and testable on the host.
 *
 * A text row is a canvas of `ui/draw.h` in the buffer of this struct: the
 * cell is the 8x16 font of the console (`port_x86_64_font.h`), so the letters
 * of an interface are the letters of the shell and a screenshot is read with
 * one table. Nothing here allocates: the two buffers are part of the bss of
 * the program, which the kernel clears at the start of the image.
 */
#ifndef MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H
#define MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H

#include "kernel/fb.h"
#include "kernel/input.h"
#include "kernel/ksys.h"
#include "kernel/types.h"
#include "port_x86_64_font.h"
#include "ui/draw.h"
#include "ui/window.h"

/* The widest text row of a program: the picture of ADR 0023 is 160 cells. */
#define UIPROG_COLS_MAX 160

/* The pixels of one row of text: the widest line at the cell of the font. */
#define UIPROG_LINE_PIXELS (UIPROG_COLS_MAX * FONT8_WIDTH * FONT8_HEIGHT)

struct uiprog {
    int fb;
    int input;
    struct fb_info info;
    struct ui_target target; /* the two calls of the device, context = this */
    k_u16_t line[UIPROG_LINE_PIXELS];
    unsigned char transfer[KSYS_MAX_IO];
};

/*
 * The two devices and the screen. The program says its name for the line of
 * a failure. The answer is 0, or a negative number of the ABI: a machine
 * without a picture (`/dev/fb0` is absent in the text mode of the VGA) or
 * without the keyboard is a machine where an interface cannot run, and the
 * reason is on the console.
 */
int uiprog_open(struct uiprog *program, const char *name);

/*
 * The reverse and the explicit end: the keyboard goes back to the console,
 * then the picture to the kernel, which repaints the grid of the shell. A
 * close does the same for a program that never gets here.
 */
void uiprog_close(struct uiprog *program);

/* The columns and the rows of the picture in cells of the font. */
int uiprog_columns(const struct uiprog *program);
int uiprog_rows(const struct uiprog *program);

/* The font of the console: the glyph function of the port and its cell. */
const struct ui_font *uiprog_font(void);

/* A rectangle of one colour, and a row of text as one rectangle of the
 * device. Both cut a rectangle that is too wide for one transfer of the
 * system into strips of whole rows. */
int uiprog_fill(struct uiprog *program, int x, int y, int w, int h, k_u16_t color);
int uiprog_text(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                k_u16_t paper);

/* A row of text in the two colours traded: the row of a list that has the
 * focus. */
int uiprog_text_reverse(struct uiprog *program, int x, int y, const char *text, int cells,
                        k_u16_t ink, k_u16_t paper);

/*
 * One event of the keyboard: 1 when it was read, 0 when the user is silent,
 * a negative number when the device failed. The caller naps on 0; a loop
 * that treats "nothing yet" as an error spins.
 */
int uiprog_event(struct uiprog *program, struct input_event *event);

/* Fill the whole picture with one colour: the frame a program paints first. */
void uiprog_clear(struct uiprog *program, k_u16_t color);

#endif /* MICRO_OS_PORT_X86_64_PROGRAMS_UIPROG_H */
