/*
 * Micro OS — the device side of an interface program (uiprog.h).
 *
 * The device refuses a rectangle that leaves the picture and a write larger
 * than KSYS_MAX_IO, so every rectangle here is cut into strips of whole rows
 * that fit one transfer. The header of a write is `struct fb_rect` and the
 * pixels follow it, one row after another - the shape of ADR 0028 - and the
 * screen is taken once at the open so the console stops drawing under the
 * program.
 */
#include "uiprog.h"

#include <string.h>

#include "kernel/errno.h"
#include "prog.h"
#include "ulibc.h"

static int blit_strips(struct uiprog *program, int x, int y, int w, int h, const k_u16_t *pixels)
{
    struct fb_rect header;
    size_t row_bytes;
    int rows;
    int row;

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > (int)program->info.width ||
        y + h > (int)program->info.height) {
        return -1;
    }

    row_bytes = (size_t)w * FB_PIXEL_BYTES;
    rows = (int)((KSYS_MAX_IO - sizeof(header)) / row_bytes);
    if (rows < 1) {
        return -1;
    }

    for (row = 0; row < h; row += rows) {
        int take = h - row < rows ? h - row : rows;
        size_t payload = (size_t)take * row_bytes;

        header.x = (k_u16_t)x;
        header.y = (k_u16_t)(y + row);
        header.w = (k_u16_t)w;
        header.h = (k_u16_t)take;

        memcpy(program->transfer, &header, sizeof(header));
        memcpy(program->transfer + sizeof(header), pixels + (size_t)row * (size_t)w, payload);

        if (write(program->fb, program->transfer, sizeof(header) + payload) !=
            (long)(sizeof(header) + payload)) {
            return -1;
        }
    }

    return 0;
}

static int fill_strips(struct uiprog *program, int x, int y, int w, int h, k_u16_t color)
{
    struct fb_rect header;
    size_t row_bytes;
    int rows;
    int row;
    int column;

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > (int)program->info.width ||
        y + h > (int)program->info.height) {
        return -1;
    }

    row_bytes = (size_t)w * FB_PIXEL_BYTES;
    rows = (int)((KSYS_MAX_IO - sizeof(header)) / row_bytes);
    if (rows < 1) {
        return -1;
    }

    for (row = 0; row < h; row += rows) {
        int take = h - row < rows ? h - row : rows;
        k_u16_t *pixels = (k_u16_t *)(void *)(program->transfer + sizeof(header));
        size_t count = (size_t)take * (size_t)w;

        header.x = (k_u16_t)x;
        header.y = (k_u16_t)(y + row);
        header.w = (k_u16_t)w;
        header.h = (k_u16_t)take;

        memcpy(program->transfer, &header, sizeof(header));
        for (column = 0; column < (int)count; column++) {
            pixels[column] = color;
        }

        if (write(program->fb, program->transfer, sizeof(header) + count * FB_PIXEL_BYTES) !=
            (long)(sizeof(header) + count * FB_PIXEL_BYTES)) {
            return -1;
        }
    }

    return 0;
}

/* The two calls of the target of `ui/draw.h`. */
static int target_fill(void *context, const struct ui_rect *rect, k_u16_t color)
{
    if (context == NULL || rect == NULL) {
        return -1;
    }

    return fill_strips(context, rect->x, rect->y, rect->w, rect->h, color);
}

static int target_blit(void *context, const struct ui_rect *rect, const k_u16_t *pixels)
{
    if (context == NULL || rect == NULL || pixels == NULL) {
        return -1;
    }

    return blit_strips(context, rect->x, rect->y, rect->w, rect->h, pixels);
}

static void message(const char *name, const char *text)
{
    char line[128];
    int length;

    length = snprintf(line, sizeof(line), "%s: %s\n", name, text);
    if (length > 0) {
        (void)write(ULIBC_STDERR, line, (size_t)length);
    }
}

int uiprog_open(struct uiprog *program, const char *name)
{
    if (program == NULL) {
        return -1;
    }

    memset(program, 0, sizeof(*program));
    program->fb = -1;
    program->input = -1;

    program->fb = open("/dev/fb0", ULIBC_O_RDWR);
    if (program->fb < 0) {
        message(name, "/dev/fb0 is not there, the machine has no picture");
        return program->fb;
    }

    if (read(program->fb, &program->info, sizeof(program->info)) != (long)sizeof(program->info) ||
        program->info.width == 0u || program->info.height == 0u ||
        program->info.format != FB_FORMAT_RGB565) {
        message(name, "/dev/fb0 did not answer the shape of the picture");
        (void)close(program->fb);
        program->fb = -1;
        return -1;
    }

    program->input = open("/dev/input0", ULIBC_O_RDONLY);
    if (program->input < 0) {
        message(name, "/dev/input0 is not there, the keyboard is silent");
        (void)close(program->fb);
        program->fb = -1;
        return program->input;
    }

    if (prog_call(KSYS_IOCTL, program->fb, (long)FBIO_TAKE, 0, 0, 0, 0) < 0) {
        message(name, "the console did not give its screen");
        (void)close(program->input);
        (void)close(program->fb);
        program->input = -1;
        program->fb = -1;
        return -1;
    }

    program->target.context = program;
    program->target.fill = target_fill;
    program->target.blit = target_blit;

    return 0;
}

void uiprog_close(struct uiprog *program)
{
    if (program == NULL) {
        return;
    }

    if (program->input >= 0) {
        (void)close(program->input);
        program->input = -1;
    }

    if (program->fb >= 0) {
        (void)prog_call(KSYS_IOCTL, program->fb, (long)FBIO_GIVE, 0, 0, 0, 0);
        (void)close(program->fb);
        program->fb = -1;
    }
}

int uiprog_columns(const struct uiprog *program)
{
    if (program == NULL || program->info.font_width == 0u) {
        return 0;
    }

    return (int)(program->info.width / program->info.font_width);
}

int uiprog_rows(const struct uiprog *program)
{
    if (program == NULL || program->info.font_height == 0u) {
        return 0;
    }

    return (int)(program->info.height / program->info.font_height);
}

const struct ui_font *uiprog_font(void)
{
    static const struct ui_font s_font = {
        .width = FONT8_WIDTH,
        .height = FONT8_HEIGHT,
        .glyph = font8x16_glyph,
    };

    return &s_font;
}

int uiprog_fill(struct uiprog *program, int x, int y, int w, int h, k_u16_t color)
{
    return fill_strips(program, x, y, w, h, color);
}

int uiprog_text(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                k_u16_t paper)
{
    struct ui_canvas canvas;
    int width;

    if (program == NULL || cells <= 0 || cells > UIPROG_COLS_MAX) {
        return -1;
    }

    width = cells * FONT8_WIDTH;
    if (x < 0 || x + width > (int)program->info.width) {
        return -1;
    }

    ui_canvas_init(&canvas, width, FONT8_HEIGHT, program->line);
    if (ui_canvas_text(&canvas, 0, 0, text, cells, ink, paper, uiprog_font()) != 0) {
        return -1;
    }

    return blit_strips(program, x, y, width, FONT8_HEIGHT, program->line);
}

int uiprog_text_reverse(struct uiprog *program, int x, int y, const char *text, int cells,
                        k_u16_t ink, k_u16_t paper)
{
    return uiprog_text(program, x, y, text, cells, paper, ink);
}

int uiprog_event(struct uiprog *program, struct input_event *event)
{
    long got;

    if (program == NULL || event == NULL) {
        return -1;
    }

    got = read(program->input, event, sizeof(*event));
    if (got == (long)sizeof(*event)) {
        return 1;
    }
    if (got == 0) {
        return 0;
    }

    /* EAGAIN is "the user is silent" and not a failure of the device: the
     * read of a keyboard with nothing in it is the loop of an interface. */
    if (got == (long)KERR_AGAIN) {
        return 0;
    }

    return (int)got;
}

void uiprog_clear(struct uiprog *program, k_u16_t color)
{
    if (program == NULL) {
        return;
    }

    (void)fill_strips(program, 0, 0, (int)program->info.width, (int)program->info.height, color);
}
