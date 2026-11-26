/*
 * Micro OS — the screen a firmware of UEFI programmed.
 *
 * A machine of UEFI starts with a picture already on its glass: the firmware
 * asked the card for a mode (EFI_GRAPHICS_OUTPUT_PROTOCOL) and the loader took
 * it, so the console of this port has a frame without programming a card
 * itself (ADR 0023 and the Bochs VBE interface are the other machine, the one
 * of a BIOS). Two things come with that frame and neither is the shape the
 * kernel draws in:
 *
 *  - the frame is 32 bits per pixel, in the order of the bytes the firmware
 *    describes (blue-green-red or red-green-blue) and never the 565 of the
 *    renderer of the kernel (kernel/render.h); a 16-bit mode exists in the
 *    specification but almost no firmware offers one;
 *  - it is memory behind a bus, at an address above the identity map of the
 *    boot and not to be kept in write-back cache;
 *
 * So the console draws into a shadow of the picture in RGB565 - the picture
 * src/fb.c makes, one cell of the font at a time, and the picture `/dev/fb0`
 * gives a program - and this file converts it into the frame of the firmware
 * when a cell changes. The conversion is a rectangle and not a picture: the
 * console tells the screen which cell it wrote, the band of the picture of
 * that cell is converted, and the rest of the frame is the bytes it already
 * had. A full picture is converted when the whole picture changed: the clear
 * of the boot, a scroll, a repaint of the colours of /etc.
 *
 * The shadow is also why the format is a number of the port and not of the
 * console: a machine whose firmware gives a 565 frame has no shadow at all
 * and the console writes the frame itself, and the two machines run the same
 * console (src/console.c), the same grid and the same font.
 *
 * A program that owns the glass (`/dev/fb0`, ADR 0028) writes its picture
 * through the same shadow, and the port's device presents the rectangle it
 * blitted with port_x86_64_gop_present(): one screen, one conversion, whoever
 * drew it.
 */
#include "port_x86_64_screen.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/render.h"
#include "port_x86_64.h"
#include "port_x86_64_console.h"

/*
 * The step of a line of cells: a cell of the font is 16 lines and the gap of
 * /etc/font adds 0 to 4 pixels to it (src/fb.c, port_x86_64_font.h). The band
 * of a present does not ask the screen for the step it is in - the shadow is
 * this file's and the step is fb.c's - so it takes the widest a row of cells
 * can be: a few lines of the picture converted one time more than needed cost
 * less than a table of the geometry of a screen that belongs to another file,
 * and a present that never misses a line of a cell is what keeps the picture
 * of a machine with `font.line_gap` set whole.
 */
#define GOP_CELL_MIN 16
#define GOP_CELL_MAX 20

/* The frame the loader may hand this file: far more than a picture of a
 * laptop needs and small enough that the shadow of it is a block of the heap
 * the port can afford (the loader refuses a bigger one, loader/uefi). */
#define GOP_WIDTH_MAX 4096
#define GOP_HEIGHT_MAX 4096

/* One channel of the conversion: the shift and the width of the mask of a
 * colour in the frame of the firmware. A channel of width zero is a frame
 * that does not carry it. */
struct gop_channel {
    k_u32_t shift;
    k_u32_t width;
};

static const struct port_x86_64_screen *s_base; /* the picture of src/fb.c */
static struct port_x86_64_screen s_screen;      /* the same, with the present */
static char s_name[96];

static volatile k_u8_t *s_frame; /* the glass, in the top 2 GiB */
static k_u16_t *s_shadow;        /* the picture in the format of the kernel */
static int s_shadow_pixels;      /* the stride of the shadow, in pixels */

static int s_width;
static int s_height;
static int s_pitch;   /* bytes of a line of the frame */
static int s_bytes;   /* bytes of a pixel of the frame, 2 or 4 */
static bool s_direct; /* the frame is 565: the console writes it itself */

static struct gop_channel s_red;
static struct gop_channel s_green;
static struct gop_channel s_blue;

/* The cell of the cursor that the screen of fb.c holds, so that a present of
 * a cursor move covers the cell it leaves as well as the one it enters. */
static int s_cursor_row = -1;

/* --- the conversion -------------------------------------------------------- */

static void channel_init(struct gop_channel *channel, k_u32_t mask)
{
    k_u32_t bits = 0;
    k_u32_t bit;

    channel->shift = 0;
    channel->width = 0;

    if (mask == 0u) {
        return;
    }

    /* The mask is a run of ones of a width of eight at most (the bits of a
     * channel of a pixel): the count is a loop and not a builtin, because the
     * machine of a laptop has popcnt and the port does not build for one. */
    while ((mask & 1u) == 0u) {
        mask >>= 1u;
        channel->shift++;
    }

    for (bit = 0; bit < 32u && (mask & (1u << bit)) != 0u; bit++) {
        bits++;
    }

    channel->width = bits > 8u ? 8u : bits;
}

/* One channel of a pixel: the eight bits of the console, cut to the width of
 * the mask and put where the mask says. */
static k_u32_t channel_pack(const struct gop_channel *channel, k_u32_t value)
{
    if (channel->width == 0u) {
        return 0u;
    }

    return (value >> (8u - channel->width)) << channel->shift;
}

/* One line of the shadow into one line of the frame. The colours of the
 * renderer are 565 (five bits, six and five), and the eight bits of each are
 * the widening fb.c uses for its palette: the low bits repeat the high ones,
 * so 0x1F of red is white and not a dark red with a step in it. Rows are built
 * in a local chunk and copied, rather than stored to the framebuffer pixel by
 * pixel; the chunk keeps the stack small on a 4096-pixel firmware frame. */
#define GOP_CONVERT_CHUNK 128u

static void convert_line(int line)
{
    const k_u16_t *source = s_shadow + (k_size_t)line * s_shadow_pixels;
    k_u32_t pixels[GOP_CONVERT_CHUNK];
    int x = 0;

    while (x < s_width) {
        int count = s_width - x;
        int index;

        if (count > (int)GOP_CONVERT_CHUNK) {
            count = (int)GOP_CONVERT_CHUNK;
        }

        if (s_bytes == 2) {
            volatile k_u16_t *destination =
                (volatile k_u16_t *)(void *)(s_frame + (k_size_t)line * s_pitch);

            for (index = 0; index < count; index++) {
                k_u16_t pixel = source[x + index];
                k_u32_t red = (k_u32_t)((pixel >> 11) & 0x1Fu);
                k_u32_t green = (k_u32_t)((pixel >> 5) & 0x3Fu);
                k_u32_t blue = (k_u32_t)(pixel & 0x1Fu);

                pixels[index] = (k_u16_t)(channel_pack(&s_red, (red << 3) | (red >> 2)) |
                                          channel_pack(&s_green, (green << 2) | (green >> 4)) |
                                          channel_pack(&s_blue, (blue << 3) | (blue >> 2)));
            }

            memcpy((void *)(uintptr_t)(destination + x), pixels, (k_size_t)count * sizeof(k_u16_t));
        } else {
            volatile k_u32_t *destination =
                (volatile k_u32_t *)(void *)(s_frame + (k_size_t)line * s_pitch);

            for (index = 0; index < count; index++) {
                k_u16_t pixel = source[x + index];
                k_u32_t red = (k_u32_t)((pixel >> 11) & 0x1Fu);
                k_u32_t green = (k_u32_t)((pixel >> 5) & 0x3Fu);
                k_u32_t blue = (k_u32_t)(pixel & 0x1Fu);

                /* The byte the format calls reserved is opaque: it is what a
                 * blitter of a firmware leaves there, and a display that reads it
                 * as alpha shows the frame of the boot underneath a picture of
                 * this one otherwise. */
                pixels[index] = channel_pack(&s_red, (red << 3) | (red >> 2)) |
                                channel_pack(&s_green, (green << 2) | (green >> 4)) |
                                channel_pack(&s_blue, (blue << 3) | (blue >> 2)) | 0xFF000000u;
            }

            memcpy((void *)(uintptr_t)(destination + x), pixels, (k_size_t)count * sizeof(k_u32_t));
        }

        x += count;
    }
}

/* The lines of the picture a band of cells covers: the first one of the first
 * row of cells and the last one the last row can reach, cut at the picture.
 */
static void present_cells(int first_row, int last_row)
{
    int first;
    int last;

    if (s_direct || s_shadow == NULL || s_frame == NULL) {
        return;
    }

    if (first_row < 0) {
        first_row = 0;
    }

    if (last_row < first_row) {
        last_row = first_row;
    }

    first = first_row * GOP_CELL_MIN;
    last = (last_row + 1) * GOP_CELL_MAX;

    if (first >= s_height) {
        return;
    }

    if (last > s_height) {
        last = s_height;
    }

    for (; first < last; first++) {
        convert_line(first);
    }
}

static void present_all(void)
{
    if (s_direct || s_shadow == NULL) {
        return;
    }

    present_cells(0, s_screen.rows - 1);
}

/* --- the screen of the console --------------------------------------------- */

static void gop_clear(k_u8_t attr)
{
    s_base->clear(attr);
    present_all();
}

static void gop_put(int row, int column, char character, k_u8_t attr)
{
    s_base->put(row, column, character, attr);
    present_cells(row, row);
}

static void gop_scroll(k_u8_t attr)
{
    s_base->scroll(attr);
    present_all();
}

static void gop_cursor(int row, int column)
{
    int first = s_cursor_row;

    s_base->cursor(row, column);

    if (first < 0 || first > row) {
        first = row;
    }

    s_cursor_row = row;
    present_cells(first, row);
}

static void gop_colors(const struct port_x86_64_pair *pair)
{
    s_base->colors(pair);
    present_all();
}

static void gop_redraw(void)
{
    s_base->redraw();
    present_all();
}

/* --- the device of ADR 0028 ------------------------------------------------ */

void port_x86_64_gop_present(const struct fb_rect *rect)
{
    if (rect == NULL) {
        present_all();
        return;
    }

    if (s_direct || s_shadow == NULL) {
        return;
    }

    /* The rectangle is in pixels and the conversion is a line at a time: the
     * band of the rows of the rectangle is the whole of what it can have
     * changed. */
    {
        int first = rect->y;
        int last = rect->y + rect->h;

        if (first < 0) {
            first = 0;
        }

        if (last > s_height) {
            last = s_height;
        }

        for (; first < last; first++) {
            convert_line(first);
        }
    }
}

/* --- the mode of the firmware --------------------------------------------- */

/* The masks of the two 32-bit formats the specification names by the order of
 * their bytes, as the bit masks of the conversion. */
static void masks_of(k_u32_t format, k_u32_t *red, k_u32_t *green, k_u32_t *blue)
{
    switch (format) {
    case PORT_X86_64_PIXEL_RGBX32:
        *red = 0x00FF0000u;
        *green = 0x0000FF00u;
        *blue = 0x000000FFu;
        return;
    case PORT_X86_64_PIXEL_BGRX32:
        *red = 0x000000FFu;
        *green = 0x0000FF00u;
        *blue = 0x00FF0000u;
        return;
    default:
        *red = 0u;
        *green = 0u;
        *blue = 0u;
        return;
    }
}

/* The name of a format, the way the boot of the loader says it. */
static const char *format_name(k_u32_t format)
{
    switch (format) {
    case PORT_X86_64_PIXEL_RGB565:
        return "rgb565";
    case PORT_X86_64_PIXEL_RGBX32:
        return "rgbx";
    case PORT_X86_64_PIXEL_BGRX32:
        return "bgrx";
    case PORT_X86_64_PIXEL_MASK16:
        return "mask16";
    case PORT_X86_64_PIXEL_MASK32:
        return "mask32";
    default:
        return "unknown";
    }
}

const struct port_x86_64_screen *port_x86_64_gop_screen(void)
{
    const struct port_x86_64_boot_info *boot = port_x86_64_boot();
    struct port_x86_64_screen_mode mode;
    k_u64_t offset;
    k_u64_t window;
    k_u32_t red = 0u;
    k_u32_t green = 0u;
    k_u32_t blue = 0u;
    void *base;

    if (boot == NULL || boot->fb_phys == 0u || boot->fb_size == 0u) {
        return NULL;
    }

    if (boot->fb_width == 0u || boot->fb_height == 0u || boot->fb_width > GOP_WIDTH_MAX ||
        boot->fb_height > GOP_HEIGHT_MAX || (boot->fb_bpp != 16u && boot->fb_bpp != 32u)) {
        kerror("screen: the picture of the firmware (%ux%u, %u bpp) is not one the port draws",
               (unsigned)boot->fb_width, (unsigned)boot->fb_height, (unsigned)boot->fb_bpp);
        return NULL;
    }

    memset(&mode, 0, sizeof(mode));
    mode.phys = boot->fb_phys;
    mode.bytes = (k_u32_t)boot->fb_size;
    mode.width = (k_u16_t)boot->fb_width;
    mode.height = (k_u16_t)boot->fb_height;
    mode.pitch = boot->fb_pitch;
    mode.bpp = (k_u8_t)boot->fb_bpp;
    mode.format = (k_u8_t)boot->fb_format;
    mode.mask_red = boot->fb_mask_red;
    mode.mask_green = boot->fb_mask_green;
    mode.mask_blue = boot->fb_mask_blue;

    if (mode.pitch < (k_u32_t)mode.width * (mode.bpp / 8u)) {
        kerror("screen: the frame of the firmware is %u bytes a line, less than %u pixels of %u",
               (unsigned)mode.pitch, (unsigned)mode.width, (unsigned)mode.bpp);
        return NULL;
    }

    s_bytes = mode.bpp / 8;
    s_width = mode.width;
    s_height = mode.height;
    s_pitch = (int)mode.pitch;
    s_direct = mode.format == PORT_X86_64_PIXEL_RGB565;
    s_cursor_row = -1;

    switch (mode.format) {
    case PORT_X86_64_PIXEL_RGB565:
        s_direct = true;
        break;
    case PORT_X86_64_PIXEL_MASK16:
    case PORT_X86_64_PIXEL_MASK32:
        red = mode.mask_red;
        green = mode.mask_green;
        blue = mode.mask_blue;
        break;
    default:
        masks_of(mode.format, &red, &green, &blue);
        break;
    }

    channel_init(&s_red, red);
    channel_init(&s_green, green);
    channel_init(&s_blue, blue);

    if (!s_direct && (s_red.width == 0u || s_green.width == 0u || s_blue.width == 0u)) {
        kerror("screen: the picture of the firmware has a format the port cannot convert (%s)",
               format_name(mode.format));
        return NULL;
    }

    /* The window of the frame: the 2 MiB page it starts in and its offset, the
     * way the VBE path maps the memory of the card. The framebuffer map gives
     * the stores write-combining through PAT; it does not change MTRRs or the
     * uncached windows of the controllers. */
    offset = mode.phys & (PORT_X86_64_SCREEN_PAGE - 1u);
    window = mode.phys - offset;

    if (port_x86_64_paging_map_framebuffer(PORT_X86_64_SCREEN_VIRT, window,
                                           (k_size_t)mode.bytes + (k_size_t)offset) == 0u) {
        kerror("screen: the frame of the firmware at 0x%x cannot be mapped",
               (unsigned)(mode.phys >> 16));
        return NULL;
    }

    s_frame = (volatile k_u8_t *)(uintptr_t)(PORT_X86_64_SCREEN_VIRT + offset);

    /*
     * The shadow: the picture in the format of the renderer, when the frame is
     * not in it. The block is the heap's and lives as long as the machine
     * does; a machine whose heap has no 4 MiB keeps the text mode of the VGA,
     * which is a screen and not a crash.
     */
    s_shadow = NULL;
    s_shadow_pixels = s_width;

    if (!s_direct) {
        s_shadow = k_port_alloc((k_size_t)s_width * (k_size_t)s_height * sizeof(k_u16_t), false);

        if (s_shadow == NULL) {
            kerror("screen: no memory for the shadow of a picture of %ux%u", (unsigned)s_width,
                   (unsigned)s_height);
            return NULL;
        }

        base = s_shadow;
        mode.pitch = (k_u32_t)s_width * 2u;
        mode.bpp = 16u;
    } else {
        base = (void *)(uintptr_t)s_frame;
    }

    s_base = port_x86_64_fb_screen(&mode, base);

    if (s_base == NULL) {
        kerror("screen: the picture of the firmware does not make a grid of cells");
        return NULL;
    }

    /* The screen of the console: the one of fb.c with the present of this
     * file over each operation that changes the picture. The copy is a copy
     * of the table of operations and not of the grid: the cells stay where
     * fb.c keeps them. */
    s_screen = *s_base;
    s_screen.clear = gop_clear;
    s_screen.put = gop_put;
    s_screen.scroll = gop_scroll;
    s_screen.cursor = gop_cursor;
    s_screen.colors = gop_colors;
    s_screen.redraw = gop_redraw;

    snprintf(s_name, sizeof(s_name),
             "the framebuffer of the firmware, %ux%u, %u bpp %s, %dx%d cells", (unsigned)mode.width,
             (unsigned)mode.height, (unsigned)boot->fb_bpp, format_name(mode.format),
             s_screen.columns, s_screen.rows);
    s_screen.name = s_name;

    kinfo("screen: the picture of the firmware, %ux%u and %u bpp %s, is %s", (unsigned)mode.width,
          (unsigned)mode.height, (unsigned)boot->fb_bpp, format_name(mode.format),
          s_direct ? "the frame itself" : "a shadow of the heap");

    return &s_screen;
}
