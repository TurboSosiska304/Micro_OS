/*
 * Micro OS — the device side of an interface program (uiprog.h).
 *
 * The device refuses a rectangle that leaves the picture and a write larger
 * than KSYS_MAX_IO, so every rectangle here is cut into strips of whole rows
 * that fit one transfer. The header of a write is `struct fb_rect` and the
 * pixels follow it, one row after another - the shape of ADR 0028 - and the
 * screen is taken once at the open so the console stops drawing under the
 * program.
 *
 * The second mode is the window of the desktop (1.7, docs/desktop.md). The
 * server of `/bin/wm` starts the program with `MICROOS_WM` in its environment
 * and three descriptors: the events it sends down, the requests the program
 * sends up, and the file of the pixels of the window. The two targets differ
 * in one place - a rectangle goes to the device or into the buffer - and the
 * rest of the program is the same, which is what "the editor did not change
 * when the window arrived" means.
 */
#include "uiprog.h"

#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "kernel/errno.h"
#include "prog.h"
#include "ui/wm_protocol.h"
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

/* The two calls of the target of `ui/draw.h` on the screen of the machine. */
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

/* A rectangle of the program: the window of the desktop when there is one,
 * the device of the port otherwise. */
static int program_fill(struct uiprog *program, int x, int y, int w, int h, k_u16_t color)
{
    if (program->desktop) {
        struct ui_rect rect;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;

        return wm_client_fill(&program->client, &rect, color);
    }

    return fill_strips(program, x, y, w, h, color);
}

static int program_blit(struct uiprog *program, int x, int y, int w, int h, const k_u16_t *pixels)
{
    if (program->desktop) {
        struct ui_rect rect;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;

        return wm_client_blit(&program->client, &rect, pixels);
    }

    return blit_strips(program, x, y, w, h, pixels);
}

/*
 * A word of the environment, without the allocator of the library: the
 * variable of the desktop is the one thing this file asks of it, and the
 * small runtime of a program does not carry `stdlib.c` for one lookup.
 */
static const char *environment(const char *name)
{
    size_t length;
    char **word;

    if (name == NULL || environ == NULL) {
        return NULL;
    }

    length = strlen(name);

    for (word = environ; *word != NULL; word++) {
        if (strncmp(*word, name, length) == 0 && (*word)[length] == '=') {
            return *word + length + 1u;
        }
    }

    return NULL;
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

/*
 * The three standard descriptors, before the program opens anything.
 *
 * The table of a program starts empty: the console of the kernel answers the
 * numbers 1 and 2 only while no file of the program has taken them, so the
 * first `open` of the program would take the number 0 and the second the
 * number 1 - and from then on `write(1, ...)` would be a write of the
 * framebuffer or of the keyboard. The console device of the devfs fills the
 * numbers the table does not have yet, and a number that is already in use
 * (a descriptor a server gave the program) is left alone.
 */
static void standard_descriptors(void)
{
    int tty = open("/dev/tty0", ULIBC_O_WRONLY);

    if (tty < 0) {
        return;
    }

    if (tty == 0) {
        (void)dup2(tty, 1);
        (void)dup2(tty, 2);
    } else if (tty == 1) {
        (void)dup2(tty, 2);
    } else {
        (void)close(tty);
    }
}

/* --- the transport of the window (1.7) ------------------------------------- */

/*
 * The buffer is the file the server made before the fork. It is mapped
 * `MAP_SHARED`, so a write of this program is a write of the file, and the
 * pages that changed go to the file with `msync` - the kernel of 1.7 gives
 * every mapping its own frame (kernel/src/mm.c) and the write-back is what
 * makes the frame the server reads.
 */
static int link_map(void *context, int width, int height, k_u16_t **pixels)
{
    struct uiprog *program = context;
    k_size_t bytes;
    k_size_t size;
    void *mapping;

    if (width <= 0 || height <= 0 || pixels == NULL) {
        return -1;
    }

    bytes = (k_size_t)width * (k_size_t)height * FB_PIXEL_BYTES;
    size = (bytes + KSYS_PAGE_SIZE - 1u) / KSYS_PAGE_SIZE * KSYS_PAGE_SIZE;

    mapping = mmap(NULL, size, KSYS_PROT_READ | KSYS_PROT_WRITE, KSYS_MAP_SHARED, WM_FD_BUFFER, 0);
    if (mapping == MAP_FAILED) {
        return -1;
    }

    program->mapping = mapping;
    program->mapping_size = size;
    *pixels = (k_u16_t *)mapping;

    return 0;
}

static int link_sync(void *context)
{
    struct uiprog *program = context;

    if (program->mapping == NULL || program->mapping_size == 0) {
        return -1;
    }

    return msync(program->mapping, program->mapping_size);
}

static int link_send(void *context, const struct wm_message *message)
{
    const char *bytes = (const char *)message;
    k_size_t left = sizeof(*message);

    K_UNUSED(context);

    if (message == NULL) {
        return -1;
    }

    /* A pipe of the system does not promise to take a write whole, so the
     * message is written until it is all there. The server drains its pipes
     * every turn of its loop, so the wait is short. */
    while (left > 0) {
        long put = write(WM_FD_REQUESTS, bytes, left);

        if (put <= 0) {
            return put < 0 ? (int)put : -1;
        }

        bytes += put;
        left -= (k_size_t)put;
    }

    return 0;
}

/* The bytes of the messages that a read cut short: a pipe of the system hands
 * a reader what it has, and a message is whole only when its last byte is
 * here. */
static int link_recv(void *context, struct wm_message *message, bool wait)
{
    struct uiprog *program = context;

    if (message == NULL) {
        return -1;
    }

    for (;;) {
        long got;

        if (program->inbox_bytes == WM_MESSAGE_BYTES) {
            memcpy(message, program->inbox, WM_MESSAGE_BYTES);
            program->inbox_bytes = 0;
            return 1;
        }

        if (!wait) {
            struct pollfd entry;

            entry.fd = WM_FD_EVENTS;
            entry.events = KSYS_POLLIN;
            entry.revents = 0;

            if (poll(&entry, 1, 0) <= 0) {
                return 0;
            }
        }

        got = read(WM_FD_EVENTS, program->inbox + program->inbox_bytes,
                   WM_MESSAGE_BYTES - program->inbox_bytes);

        if (got < 0) {
            /* A silent descriptor of a window whose server is there is not an
             * error: the loop of an interface asks again. */
            if (got == -KERR_AGAIN) {
                return 0;
            }
            return (int)got;
        }

        if (got == 0) {
            /* The server is gone: the program is a window of nothing. */
            return -1;
        }

        program->inbox_bytes += (unsigned)got;
    }
}

static int open_desktop(struct uiprog *program, const char *name)
{
    program->desktop = true;
    program->link.context = program;
    program->link.map = link_map;
    program->link.sync = link_sync;
    program->link.send = link_send;
    program->link.recv = link_recv;

    if (wm_client_attach(&program->client, &program->link) != 0) {
        message(name, "the window server did not give a window");
        return -1;
    }

    /* The shape the program lays out its picture by: the window is its whole
     * picture now, and the font is the cell of the console. */
    memset(&program->info, 0, sizeof(program->info));
    program->info.width = (k_u16_t)program->client.width;
    program->info.height = (k_u16_t)program->client.height;
    program->info.pitch = (k_u16_t)program->client.width;
    program->info.columns = (k_u16_t)(program->client.width / FONT8_WIDTH);
    program->info.rows = (k_u16_t)(program->client.height / FONT8_HEIGHT);
    program->info.bpp = FB_BPP_RGB565;
    program->info.format = FB_FORMAT_RGB565;
    program->info.font_width = FONT8_WIDTH;
    program->info.font_height = FONT8_HEIGHT;

    program->target = program->client.target;

    return 0;
}

int uiprog_open(struct uiprog *program, const char *name)
{
    if (program == NULL) {
        return -1;
    }

    memset(program, 0, sizeof(*program));
    program->fb = -1;
    program->input = -1;

    standard_descriptors();

    /* The desktop first: a program the server started has no screen of its
     * own and must not take the one of the master. */
    if (environment(WM_ENV) != NULL) {
        return open_desktop(program, name);
    }

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

    if (program->desktop) {
        (void)wm_client_flush(&program->client);
        (void)wm_client_request_close(&program->client);

        if (program->mapping != NULL) {
            (void)munmap(program->mapping, program->mapping_size);
            program->mapping = NULL;
        }

        (void)close(WM_FD_EVENTS);
        (void)close(WM_FD_REQUESTS);
        (void)close(WM_FD_BUFFER);
        program->desktop = false;

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
        .scale = 0,
        .glyph = font8x16_glyph,
    };

    return &s_font;
}

/* The same glyphs at twice the cell: the second size of the text of a window
 * (ADR 0054). One table of 95 glyphs serves both, and a cell of 16x32 pixels
 * is one bit of it drawn as a square of two. */
const struct ui_font *uiprog_font_large(void)
{
    static const struct ui_font s_font = {
        .width = FONT8_WIDTH,
        .height = FONT8_HEIGHT,
        .scale = 2,
        .glyph = font8x16_glyph,
    };

    return &s_font;
}

/* The text of /etc/console, or -1 when the machine has no such file: the one
 * file of the mood of the desktop, read by the palette and by the clock. */
static int console_text(char *text, k_size_t size)
{
    long got;
    int descriptor;

    if (text == NULL || size < 2u) {
        return -1;
    }

    descriptor = open("/etc/console", ULIBC_O_RDONLY);
    if (descriptor < 0) {
        return -1;
    }

    got = read(descriptor, text, size - 1u);
    (void)close(descriptor);

    if (got <= 0) {
        return -1;
    }

    text[got] = '\0';

    return (int)got;
}

void uiprog_clock(struct ui_clock *clock)
{
    char text[128];
    const char *line;

    if (clock == NULL) {
        return;
    }

    ui_clock_default(clock);

    if (console_text(text, sizeof(text)) < 0) {
        return;
    }

    line = text;

    while (*line != '\0') {
        const char *end = line;

        while (*end != '\0' && *end != '\n') {
            end++;
        }

        if (end > line) {
            char copy[64];
            k_size_t length = (k_size_t)(end - line);

            if (length >= sizeof(copy)) {
                length = sizeof(copy) - 1u;
            }

            memcpy(copy, line, length);
            copy[length] = '\0';

            (void)ui_clock_line(clock, copy);
        }

        line = *end == '\n' ? end + 1 : end;
    }
}

/* Does the text of /etc/console name the light theme? The file is one
 * `key=value` a line, so the answer is the line whose key is `theme`; the
 * first one wins, as it does for the applier of the console. */
static bool console_theme_is_light(const char *text)
{
    const char *line = text;

    while (*line != '\0') {
        const char *end = line;

        while (*end != '\0' && *end != '\n') {
            end++;
        }

        if ((size_t)(end - line) == 11u && strncmp(line, "theme=light", 11u) == 0) {
            return true;
        }

        line = *end == '\n' ? end + 1 : end;
    }

    return false;
}

const char *uiprog_palette(struct ui_palette *palette)
{
    char text[128];

    if (palette == NULL) {
        return "dark";
    }

    ui_palette_dark(palette);

    if (console_text(text, sizeof(text)) < 0) {
        return "dark";
    }

    if (!console_theme_is_light(text)) {
        return "dark";
    }

    ui_palette_light(palette);

    return "light";
}

int uiprog_fill(struct uiprog *program, int x, int y, int w, int h, k_u16_t color)
{
    return program_fill(program, x, y, w, h, color);
}

int uiprog_text_font(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                     k_u16_t paper, const struct ui_font *font)
{
    struct ui_canvas canvas;
    int cell_w;
    int cell_h;
    int width;

    if (program == NULL || font == NULL || cells <= 0 || cells > UIPROG_COLS_MAX) {
        return -1;
    }

    cell_w = ui_font_cell_width(font);
    cell_h = ui_font_cell_height(font);

    if (cell_w <= 0 || cell_h <= 0) {
        return -1;
    }

    width = cells * cell_w;

    /* The scratch of the program holds the widest row of the small font: a row
     * of the large one that does not fit is refused here, because a blit of
     * half a line is a line of the wrong letters. */
    if ((long)width * (long)cell_h > (long)UIPROG_LINE_PIXELS) {
        return -1;
    }

    /* A row of the window may be wider than the window when a program asks
     * for more cells than it has: the canvas is clipped by the target and the
     * row of the screen is refused, which is the rule of each of them. */
    if (!program->desktop && (x < 0 || x + width > (int)program->info.width)) {
        return -1;
    }

    ui_canvas_init(&canvas, width, cell_h, program->line);
    if (ui_canvas_text(&canvas, 0, 0, text, cells, ink, paper, font) != 0) {
        return -1;
    }

    return program_blit(program, x, y, width, cell_h, program->line);
}

int uiprog_text(struct uiprog *program, int x, int y, const char *text, int cells, k_u16_t ink,
                k_u16_t paper)
{
    return uiprog_text_font(program, x, y, text, cells, ink, paper, uiprog_font());
}

int uiprog_text_reverse(struct uiprog *program, int x, int y, const char *text, int cells,
                        k_u16_t ink, k_u16_t paper)
{
    return uiprog_text(program, x, y, text, cells, paper, ink);
}

int uiprog_blit(struct uiprog *program, int x, int y, int w, int h, const k_u16_t *pixels)
{
    return program_blit(program, x, y, w, h, pixels);
}

int uiprog_event(struct uiprog *program, struct input_event *event)
{
    long got;

    if (program == NULL || event == NULL) {
        return -1;
    }

    if (program->desktop) {
        int rc = wm_client_poll(&program->client, event);

        program->pointer_x = program->client.pointer_x;
        program->pointer_y = program->client.pointer_y;
        program->pointer_button = program->client.button;
        program->close_requested = wm_client_closing(&program->client);

        return rc;
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

    if (program->desktop) {
        (void)wm_client_clear(&program->client, color);
        return;
    }

    (void)fill_strips(program, 0, 0, (int)program->info.width, (int)program->info.height, color);
}

struct wm_client *uiprog_window(struct uiprog *program)
{
    if (program == NULL || !program->desktop) {
        return NULL;
    }

    return &program->client;
}

int uiprog_title(struct uiprog *program, const char *title)
{
    if (program == NULL || !program->desktop || title == NULL) {
        return 0;
    }

    return wm_client_set_title(&program->client, title);
}

int uiprog_flush(struct uiprog *program)
{
    if (program == NULL || !program->desktop) {
        return 0;
    }

    return wm_client_flush(&program->client);
}

bool uiprog_close_requested(const struct uiprog *program)
{
    return program != NULL && program->close_requested;
}

bool uiprog_pointer(const struct uiprog *program, int *x, int *y, bool *pressed)
{
    if (program == NULL || !program->desktop) {
        return false;
    }

    if (x != NULL) {
        *x = program->pointer_x;
    }
    if (y != NULL) {
        *y = program->pointer_y;
    }
    if (pressed != NULL) {
        *pressed = program->pointer_button;
    }

    return true;
}
