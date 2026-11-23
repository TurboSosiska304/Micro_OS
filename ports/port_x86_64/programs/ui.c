/*
 * Micro OS — `/bin/ui`, the first shape of an interface (ADR 0028).
 *
 * This is a program of the library and not a second kernel: it opens
 * `/dev/fb0` and `/dev/input0`, draws a panel, a list of windows and a window
 * of the log, moves the focus on Tab and leaves on `q`. When it ends, the
 * shell is the machine again: the ioctl of the screen and the close of the
 * keyboard hand both devices back.
 *
 * The picture is made of rectangles and nothing else. The panel changes when
 * the clock does, the list changes when the focus does and the window of the
 * log changes when `/proc/kmsg` grows; a frame is never painted whole after
 * the first one, which is the rule the ADR gives the first skeleton - a blit
 * over a serial of small writes is not a mapping, and a program that repaints
 * everything pays for everything.
 *
 * There is no window manager here and no widget: the list of "windows" is two
 * lines and the console is one rectangle. The skeleton proves what a UI needs
 * - a screen, a key, a frame that does not flicker and a way back.
 */
#include "ulibc.h"

#include <unistd.h>

#include "kernel/fb.h"
#include "kernel/input.h"
#include "port_x86_64_font.h"
#include "prog.h"

/* The layout of the first skeleton, in pixels of the picture of ADR 0023. */
#define UI_PANEL_HEIGHT 16
#define UI_LEFT_WIDTH 176
#define UI_ITEM_X 4
#define UI_ITEM_WIDTH 168
#define UI_ITEM_HEIGHT 18
#define UI_ITEM_Y0 24
#define UI_ITEM_STEP 22
#define UI_CONSOLE_X (UI_LEFT_WIDTH + 8)
#define UI_CONSOLE_Y (UI_PANEL_HEIGHT + 8)
#define UI_CONSOLE_TITLE 16
#define UI_TEXT_MARGIN 8

/* The colours of the skeleton, in RGB565. The panel is the dark blue of a
 * status line, the list is the black of the console and the focused row is
 * the light grey of the VGA, so the focus reads on a screenshot without a
 * legend. The console window repeats the pair of the kernel. */
#define UI_PANEL_COLOR 0x0010u
#define UI_LEFT_COLOR 0x0000u
#define UI_FOCUS_COLOR 0xAD55u
#define UI_TEXT_COLOR 0xFFFFu
#define UI_DIM_COLOR 0xAD55u
#define UI_INK_COLOR 0x0000u
#define UI_BORDER_COLOR 0x52AAu
#define UI_CONSOLE_COLOR 0x0000u
#define UI_TITLE_COLOR 0x57FFu /* the bright cyan of the palette */

/* The text of the program: a line of cells is 8x16, the font of the console
 * (Spleen 8x16 of the port), so the UI draws the same glyphs a person reads
 * in the shell and no second font travels in the image. */
#define UI_CELL_WIDTH FONT8_WIDTH
#define UI_CELL_HEIGHT FONT8_HEIGHT

/* The longest line this program lays out: the picture is 1280 pixels wide. */
#define UI_LINE_MAX 160
#define UI_TEXT_MAX (UI_LINE_MAX * UI_CELL_WIDTH * UI_CELL_HEIGHT)
#define UI_BUFFER_MAX 4096

/* The console window shows this many lines of the log. The ring of the kernel
 * keeps 32; the window has room for 32 at the height of the picture. */
#define UI_CONSOLE_ROWS 32
/* The cells of the window: the picture less the left column and the one
 * column of the border on the right. */
#define UI_CONSOLE_COLS 135

static int g_fb = -1;
static int g_input = -1;
static struct fb_info g_info;

/* One rectangle of the protocol, in the window of one transfer of the system
 * (KSYS_MAX_IO): the header, then the pixels of as many rows as fit. */
static unsigned char g_blit[KSYS_MAX_IO];

/* One line of text as pixels, before it goes out as one rectangle. */
static k_u16_t g_line[UI_TEXT_MAX];

/* The text the panel and the window of the log show; a rectangle is painted
 * when its text changed and not on every turn of the loop. */
static char g_panel_text[UI_LINE_MAX + 1];
static char g_log[UI_BUFFER_MAX];

static int g_focus; /* 0: status, 1: console */

static int frame_write(const void *buffer, size_t length)
{
    long rc;

    if (length == 0 || length > KSYS_MAX_IO) {
        return -1;
    }

    rc = write(g_fb, buffer, length);
    if (rc != (long)length) {
        return -1;
    }

    return 0;
}

/*
 * One rectangle into the picture. The write of the device carries a header
 * and the pixels of its rectangle, so a rectangle taller than one transfer is
 * cut into strips of whole rows: the caller sees one rectangle, the device
 * sees as many as the window allows.
 */
static int blit(int x, int y, int w, int h, const k_u16_t *pixels)
{
    struct fb_rect header;
    size_t row_bytes;
    int rows;
    int row;

    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)g_info.width ||
        y + h > (int)g_info.height) {
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

        memcpy(g_blit, &header, sizeof(header));
        memcpy(g_blit + sizeof(header), pixels + (size_t)row * (size_t)w, payload);

        if (frame_write(g_blit, sizeof(header) + payload) != 0) {
            return -1;
        }
    }

    return 0;
}

/* A rectangle of one colour: the panel, the background of the list and the
 * paper of the console window. */
static int fill(int x, int y, int w, int h, k_u16_t color)
{
    struct fb_rect header;
    size_t row_bytes;
    int rows;
    int row;
    int column;

    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)g_info.width ||
        y + h > (int)g_info.height) {
        return -1;
    }

    row_bytes = (size_t)w * FB_PIXEL_BYTES;
    rows = (int)((KSYS_MAX_IO - sizeof(header)) / row_bytes);
    if (rows < 1) {
        return -1;
    }

    for (row = 0; row < h; row += rows) {
        int take = h - row < rows ? h - row : rows;
        k_u16_t *pixels = (k_u16_t *)(void *)(g_blit + sizeof(header));
        size_t count = (size_t)take * (size_t)w;

        header.x = (k_u16_t)x;
        header.y = (k_u16_t)(y + row);
        header.w = (k_u16_t)w;
        header.h = (k_u16_t)take;

        memcpy(g_blit, &header, sizeof(header));
        for (column = 0; column < (int)count; column++) {
            pixels[column] = color;
        }

        if (frame_write(g_blit, sizeof(header) + count * FB_PIXEL_BYTES) != 0) {
            return -1;
        }
    }

    return 0;
}

/*
 * One line of text at (x, y), as one rectangle: every cell is the paper and
 * the glyph of the font in the ink. The caller gives the paper, because the
 * line of a focused row is written on the grey of the focus.
 */
static int text_line(int x, int y, const char *text, k_u16_t ink, k_u16_t paper, int cells)
{
    int index;
    int row;
    int column;

    if (cells <= 0 || cells > UI_LINE_MAX || x + cells * UI_CELL_WIDTH > (int)g_info.width) {
        return -1;
    }

    for (index = 0; index < cells * UI_CELL_WIDTH * UI_CELL_HEIGHT; index++) {
        g_line[index] = paper;
    }

    for (index = 0; index < cells && text[index] != '\0'; index++) {
        const k_u8_t *glyph = font8x16_glyph(text[index]);

        for (row = 0; row < UI_CELL_HEIGHT; row++) {
            k_u8_t bits = glyph[row];

            for (column = 0; column < UI_CELL_WIDTH; column++) {
                if ((bits & (k_u8_t)(0x80u >> column)) != 0u) {
                    g_line[row * cells * UI_CELL_WIDTH + index * UI_CELL_WIDTH + column] = ink;
                }
            }
        }
    }

    return blit(x, y, cells * UI_CELL_WIDTH, UI_CELL_HEIGHT, g_line);
}

/* A line of the picture as a C string of `cells` characters: the text is cut
 * at the room of the window and the tail is spaces, so a shorter line does
 * not leave the tail of the one before it. */
static void line_fit(char *out, const char *text, int cells)
{
    int index = 0;

    while (index < cells && text[index] != '\0' && text[index] != '\n') {
        out[index] = text[index];
        index++;
    }

    while (index < cells) {
        out[index++] = ' ';
    }

    out[cells] = '\0';
}

static int read_path(const char *path, char *buffer, size_t size)
{
    long got;
    int fd;

    if (size == 0) {
        return -1;
    }

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    got = read(fd, buffer, size - 1);
    (void)close(fd);

    if (got < 0) {
        return -1;
    }

    buffer[got] = '\0';
    return (int)got;
}

/* The number of the tasks in /proc/tasks: one line a task, a header line. */
static int count_tasks(const char *tasks)
{
    int count = 0;
    int index;

    for (index = 0; tasks[index] != '\0'; index++) {
        if (tasks[index] == '\n') {
            count++;
        }
    }

    return count > 0 ? count - 1 : 0;
}

/* The volume of /proc/meminfo: the sentence after "filesystem: ". */
static void volume_line(const char *meminfo, char *out, size_t size)
{
    const char *found = strstr(meminfo, "filesystem: ");
    size_t index = 0;

    if (found == NULL) {
        (void)snprintf(out, size, "no volume");
        return;
    }

    found += 12;

    while (found[index] != '\0' && found[index] != '\n' && index + 1 < size) {
        out[index] = found[index];
        index++;
    }

    out[index] = '\0';
}

/* --- the three rectangles the program owns --------------------------------- */

static void panel_update(long seconds)
{
    char tasks[512];
    char meminfo[1024];
    char volume[128];
    char text[UI_LINE_MAX + 1];
    int count;

    if (read_path("/proc/tasks", tasks, sizeof(tasks)) < 0) {
        tasks[0] = '\0';
    }
    if (read_path("/proc/meminfo", meminfo, sizeof(meminfo)) < 0) {
        meminfo[0] = '\0';
    }

    count = count_tasks(tasks);
    volume_line(meminfo, volume, sizeof(volume));

    (void)snprintf(text, sizeof(text), "up %ld s | tasks %d | %s | Tab focus  q quit", seconds,
                   count, volume);

    if (strcmp(text, g_panel_text) == 0) {
        return;
    }

    memcpy(g_panel_text, text, strlen(text) + 1);

    if (fill(0, 0, (int)g_info.width, UI_PANEL_HEIGHT, UI_PANEL_COLOR) != 0) {
        return;
    }

    (void)text_line(UI_TEXT_MARGIN, 0, g_panel_text, UI_TEXT_COLOR, UI_PANEL_COLOR,
                    ((int)g_info.width - UI_TEXT_MARGIN) / UI_CELL_WIDTH);
}

static void item_paint(int index, const char *name, bool focused)
{
    int y = UI_ITEM_Y0 + index * UI_ITEM_STEP;
    k_u16_t background = focused ? UI_FOCUS_COLOR : UI_LEFT_COLOR;
    k_u16_t ink = focused ? UI_INK_COLOR : UI_DIM_COLOR;
    char text[32];

    (void)fill(UI_ITEM_X, y, UI_ITEM_WIDTH, UI_ITEM_HEIGHT, background);

    (void)snprintf(text, sizeof(text), "%s %s", focused ? ">" : " ", name);
    (void)text_line(UI_ITEM_X + 6, y + 1, text, ink, background, 20);
}

static void list_update(void)
{
    item_paint(0, "status", g_focus == 0);
    item_paint(1, "console", g_focus == 1);
}

/* The last lines of the ring of the log, one line a row of the window. */
static void console_update(void)
{
    char buffer[UI_BUFFER_MAX];
    char line[UI_LINE_MAX + 1];
    const char *lines[UI_CONSOLE_ROWS];
    int count = 0;
    int start = 0;
    int index;
    int y;
    char title[UI_LINE_MAX + 1];

    if (read_path("/proc/kmsg", buffer, sizeof(buffer)) < 0) {
        buffer[0] = '\0';
    }

    if (strcmp(buffer, g_log) == 0) {
        return;
    }

    memcpy(g_log, buffer, strlen(buffer) + 1);

    /* The lines of the ring, the newest kept when there are more than the
     * window holds. */
    for (index = 0; buffer[index] != '\0'; index++) {
        if (buffer[index] == '\n') {
            if (count < UI_CONSOLE_ROWS) {
                lines[count++] = &buffer[start];
            } else {
                int slot;

                for (slot = 1; slot < UI_CONSOLE_ROWS; slot++) {
                    lines[slot - 1] = lines[slot];
                }
                lines[UI_CONSOLE_ROWS - 1] = &buffer[start];
            }
            buffer[index] = '\0';
            start = index + 1;
        }
    }

    /* The window is painted again because the log changed: the picture of a
     * text window is its text, and the area is smaller than the window of a
     * transfer only by rows. */
    if (fill(UI_CONSOLE_X, UI_CONSOLE_Y, (int)g_info.width - UI_CONSOLE_X - 1,
             (int)g_info.height - UI_CONSOLE_Y - 1, UI_CONSOLE_COLOR) != 0) {
        return;
    }

    (void)fill(UI_CONSOLE_X, UI_CONSOLE_Y, (int)g_info.width - UI_CONSOLE_X - 1, 1,
               UI_BORDER_COLOR);
    (void)snprintf(title, sizeof(title), "console - /proc/kmsg");
    (void)text_line(UI_CONSOLE_X + UI_TEXT_MARGIN, UI_CONSOLE_Y + 2, title, UI_TITLE_COLOR,
                    UI_CONSOLE_COLOR, (int)UI_CONSOLE_COLS);

    y = UI_CONSOLE_Y + UI_CONSOLE_TITLE + 2;

    for (index = 0; index < count; index++) {
        line_fit(line, lines[index], (int)UI_CONSOLE_COLS);
        (void)text_line(UI_CONSOLE_X + UI_TEXT_MARGIN, y, line, UI_DIM_COLOR, UI_CONSOLE_COLOR,
                        (int)UI_CONSOLE_COLS);
        y += UI_CELL_HEIGHT;
    }
}

/* The first frame: the panel, the list and the window of the log, each one
 * as the rectangle it is. There is no whole-screen blit even here: the
 * background of the list is the two rectangles of the layout. */
static void frame_first(long seconds)
{
    (void)fill(0, UI_PANEL_HEIGHT, UI_LEFT_WIDTH, (int)g_info.height - UI_PANEL_HEIGHT,
               UI_LEFT_COLOR);
    (void)fill(UI_LEFT_WIDTH, UI_PANEL_HEIGHT, (int)g_info.width - UI_LEFT_WIDTH,
               (int)g_info.height - UI_PANEL_HEIGHT, UI_CONSOLE_COLOR);

    /* The border of the console window: the four edges of the rectangle. */
    (void)fill(UI_CONSOLE_X - 2, UI_CONSOLE_Y - 2, (int)g_info.width - UI_CONSOLE_X + 2, 1,
               UI_BORDER_COLOR);
    (void)fill(UI_CONSOLE_X - 2, (int)g_info.height - 1, (int)g_info.width - UI_CONSOLE_X + 2, 1,
               UI_BORDER_COLOR);
    (void)fill(UI_CONSOLE_X - 2, UI_CONSOLE_Y - 2, 1, (int)g_info.height - UI_CONSOLE_Y + 2,
               UI_BORDER_COLOR);
    (void)fill((int)g_info.width - 1, UI_CONSOLE_Y - 2, 1, (int)g_info.height - UI_CONSOLE_Y + 2,
               UI_BORDER_COLOR);

    (void)text_line(UI_TEXT_MARGIN, UI_PANEL_HEIGHT + 8, "windows", UI_DIM_COLOR, UI_LEFT_COLOR,
                    12);

    list_update();
    panel_update(seconds);
    console_update();
}

int main(int argc, char *const argv[], char *const envp[])
{
    long last_second = -1;
    long last_log = -1;

    (void)argc;
    (void)argv;
    (void)envp;

    g_fb = open("/dev/fb0", ULIBC_O_RDWR);
    if (g_fb < 0) {
        static const char message[] = "ui: /dev/fb0 is not there, the machine has no picture\n";

        (void)write(ULIBC_STDERR, message, sizeof(message) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    if (read(g_fb, &g_info, sizeof(g_info)) != (long)sizeof(g_info) || g_info.width == 0u ||
        g_info.height == 0u || g_info.format != FB_FORMAT_RGB565) {
        static const char message[] = "ui: /dev/fb0 did not answer the shape of the picture\n";

        (void)write(ULIBC_STDERR, message, sizeof(message) - 1);
        (void)close(g_fb);
        return ULIBC_EXIT_FAILURE;
    }

    g_input = open("/dev/input0", ULIBC_O_RDONLY);
    if (g_input < 0) {
        static const char message[] = "ui: /dev/input0 is not there, the keyboard is silent\n";

        (void)write(ULIBC_STDERR, message, sizeof(message) - 1);
        (void)close(g_fb);
        return ULIBC_EXIT_FAILURE;
    }

    /* The screen is the program's from here: the kernel keeps its grid and
     * writes no pixel until the device is closed or the ioctl gives it back. */
    if (prog_call(KSYS_IOCTL, g_fb, (long)FBIO_TAKE, 0, 0, 0, 0) < 0) {
        static const char message[] = "ui: the console did not give its screen\n";

        (void)write(ULIBC_STDERR, message, sizeof(message) - 1);
        (void)close(g_input);
        (void)close(g_fb);
        return ULIBC_EXIT_FAILURE;
    }

    frame_first((long)(uptime() / 1000));

    for (;;) {
        struct input_event event;
        long second = (long)(uptime() / 1000);

        if (second != last_second) {
            last_second = second;
            panel_update(second);
        }

        if (second != last_log) {
            last_log = second;
            console_update();
        }

        while (read(g_input, &event, sizeof(event)) == (long)sizeof(event)) {
            if (event.value != INPUT_VALUE_PRESS) {
                continue;
            }

            if (event.code == K_INPUT_KEY_TAB) {
                g_focus = (g_focus + 1) % 2;
                list_update();
            } else if (event.code == K_INPUT_KEY_Q) {
                goto done;
            }
        }

        /* A read of a keyboard with nothing in it is EAGAIN; the nap keeps
         * the program from spinning in a call the kernel answers at once. */
        usleep(50000u);
    }

done:
    /* The devices are given back in the reverse order they were taken: the
     * keyboard to the console, the picture to the kernel, which repaints the
     * shell it kept. A close does the same for a program that never gets
     * here; the explicit calls say what the program means. */
    (void)close(g_input);
    (void)prog_call(KSYS_IOCTL, g_fb, (long)FBIO_GIVE, 0, 0, 0, 0);
    (void)close(g_fb);

    return ULIBC_EXIT_SUCCESS;
}
