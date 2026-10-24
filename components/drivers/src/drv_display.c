/*
 * Micro OS — display driver (ST7789v2, 135x240) on the StickC Plus2.
 *
 * The framebuffer lives in PSRAM and is flushed to the panel over SPI.
 * The driver draws a test pattern during the initialisation, so that a
 * broken panel or a wrong orientation is visible at once.
 */
#include "drivers/board_pins.h"
#include "drivers/drivers.h"

#include <stdio.h>
#include <string.h>

#include "kernel/types.h"

/*
 * The GRAM of the ST7789V2 is 240x320, the glass of the 1.14" panel shows
 * only a part of it. Without the offset the image is shifted and the rest
 * of the screen is garbage.
 *
 * The offsets can be changed at runtime with the "display" command of the
 * shell, so that the right values can be found without reflashing.
 */
#define LCD_X_OFFSET 53
#define LCD_Y_OFFSET 40

static int s_offset_x = LCD_X_OFFSET;
static int s_offset_y = LCD_Y_OFFSET;

/*
 * The turn of the picture for its viewer, in quarter turns clockwise.
 * The controller keeps its portrait MADCTL for ever: turning the
 * controller would move the visible area of the glass (its offset is a
 * property of the panel, not of the picture), and every quarter turn
 * would need its own pair of magic numbers. The drawing turns instead —
 * see drivers/fb_draw.h, the arithmetic of it is tested on the host.
 */
static int s_rotation;

/*
 * The mode the panel drives its colors in. Without a command at boot
 * this glass shows a negative: the background painted black comes out
 * white and the text comes out dark. INVON is sent in the init, and
 * `display invert` in the shell flips it live — the right mode for a
 * panel of another model is found with the eyes, without reflashing.
 */
static bool s_inverted;

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "drivers/display.h"
#include "drivers/fb_draw.h"
#include "drivers/internal.h"
#include "graphics/font.h"
#include "kernel/kmod.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kernel/log.h"

#define LCD_SPI_HOST SPI2_HOST
/*
 * Through the GPIO matrix the SPI master works up to 26.7 MHz, so the
 * clock is set to 20 MHz (80 MHz / 4).
 */
#define LCD_HZ (20 * 1000 * 1000)

static uint16_t *s_fb;
static spi_device_handle_t s_dev;
static bool s_initialised;

/*
 * The panel is one piece of hardware: the command, the data, the window
 * and the pixels are a sequence that no other task may step into the
 * middle of. A second task that writes its own pixels in the middle of a
 * flush changes the meaning of the commands on the wire and the panel
 * shows a picture that was never drawn. The lock is recursive because the
 * functions of the panel call each other.
 */
static SemaphoreHandle_t s_lock;

static void display_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void display_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

/* The framebuffer seen the way its viewer sees it (the caller holds the
 * lock; the size of the picture needs no pixels, the view may have none). */
static void display_view(struct fb_view *view)
{
    fb_view_init(view, s_fb, LCD_WIDTH, LCD_HEIGHT, s_rotation);
}

/*
 * The SPI DMA of the ESP32 cannot read from the PSRAM, so the framebuffer is
 * copied into this internal buffer in pieces. The buffer is in the internal
 * RAM because it is a static array.
 */
#define LCD_CHUNK_SIZE 4092
static uint8_t s_chunk[LCD_CHUNK_SIZE];

static void lcd_write_command(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);
    gpio_set_level(PIN_LCD_CS, 0);
    spi_transaction_t trans = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_dev, &trans);
    gpio_set_level(PIN_LCD_CS, 1);
}

static void lcd_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);
    spi_transaction_t trans = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_dev, &trans);
    gpio_set_level(PIN_LCD_CS, 1);
}

static void lcd_write_data8(uint8_t value)
{
    lcd_write_data(&value, 1);
}

static void lcd_init_panel(void)
{
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_write_command(0x36); /* MADCTL: portrait, BGR */
    lcd_write_data8(0x08);
    lcd_write_command(0x21); /* INVON: the colors as they are painted */
    s_inverted = true;
    lcd_write_command(0x3A); /* 16 bits per pixel */
    lcd_write_data8(0x05);
    lcd_write_command(0xB2); /* display control */
    lcd_write_data8(0x0C);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x00);
    lcd_write_data8(0x33);
    lcd_write_data8(0x33);
    lcd_write_command(0xB7); /* gate control */
    lcd_write_data8(0x35);
    lcd_write_command(0xBB); /* VCOMS */
    lcd_write_data8(0x28);
    lcd_write_command(0xC0); /* power control 1 */
    lcd_write_data8(0x2C);
    lcd_write_command(0xC2); /* power control 2 */
    lcd_write_data8(0x01);
    lcd_write_data8(0xFF);
    lcd_write_command(0xC3); /* power control 3 */
    lcd_write_data8(0x10);
    lcd_write_command(0xC4); /* power control 4 */
    lcd_write_data8(0x20);
    lcd_write_command(0xC6); /* VCOM control */
    lcd_write_data8(0x0F);
    lcd_write_command(0xD0); /* gamma */
    lcd_write_data8(0xA4);
    lcd_write_data8(0xA1);
    lcd_write_command(0xE0);
    {
        static const uint8_t gamma_pos[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                                            0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17};
        lcd_write_data(gamma_pos, sizeof(gamma_pos));
    }
    lcd_write_command(0xE1);
    {
        static const uint8_t gamma_neg[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                                            0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E};
        lcd_write_data(gamma_neg, sizeof(gamma_neg));
    }

    lcd_write_command(0x11); /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_write_command(0x29); /* display on */
}

static void lcd_set_window(int x, int y, int w, int h)
{
    uint8_t data[4];
    int first_row = y + s_offset_y;
    int last_row = first_row + h - 1;
    int first_col = x + s_offset_x;
    int last_col = first_col + w - 1;

    lcd_write_command(0x2A); /* column address */
    data[0] = (uint8_t)(first_col >> 8);
    data[1] = (uint8_t)(first_col & 0xFF);
    data[2] = (uint8_t)(last_col >> 8);
    data[3] = (uint8_t)(last_col & 0xFF);
    lcd_write_data(data, 4);

    lcd_write_command(0x2B); /* row address */
    data[0] = (uint8_t)(first_row >> 8);
    data[1] = (uint8_t)(first_row & 0xFF);
    data[2] = (uint8_t)(last_row >> 8);
    data[3] = (uint8_t)(last_row & 0xFF);
    lcd_write_data(data, 4);

    lcd_write_command(0x2C); /* memory write */
}

static void lcd_flush(void)
{
    const k_size_t total = (k_size_t)LCD_WIDTH * LCD_HEIGHT * 2;
    k_size_t offset = 0;

    if (s_fb == NULL) {
        return;
    }

    lcd_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);

    gpio_set_level(PIN_LCD_DC, 1);
    gpio_set_level(PIN_LCD_CS, 0);

    while (offset < total) {
        k_size_t chunk = LCD_CHUNK_SIZE;

        if (chunk > total - offset) {
            chunk = total - offset;
        }

        memcpy(s_chunk, ((const uint8_t *)s_fb) + offset, chunk);

        spi_transaction_t trans = {
            .length = chunk * 8,
            .tx_buffer = s_chunk,
        };

        spi_device_polling_transmit(s_dev, &trans);
        offset += chunk;
    }

    gpio_set_level(PIN_LCD_CS, 1);
}

static void lcd_draw_test_pattern(void)
{
    static const uint16_t bars[] = {
        0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF, 0xFFFF, 0x0000,
    };
    int x;
    int y;

    if (s_fb == NULL) {
        return;
    }

    for (y = 0; y < LCD_HEIGHT; y++) {
        for (x = 0; x < LCD_WIDTH; x++) {
            uint16_t color;

            if (y < 4 || y >= LCD_HEIGHT - 4 || x < 4 || x >= LCD_WIDTH - 4) {
                color = 0xFFFF; /* white border */
            } else {
                int band = (x * (int)(sizeof(bars) / sizeof(bars[0]))) / LCD_WIDTH;
                color = bars[band];
            }

            s_fb[y * LCD_WIDTH + x] = color;
        }
    }

    lcd_flush();
}

static int drv_display_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC) | (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_RST) |
                        (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };

    /*
     * The lock of the panel exists before anything is written to it: the
     * console of the board and the commands of the shell both draw.
     */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            kerror("display: the lock of the panel cannot be made");
            return -1;
        }
    }

    if (gpio_config(&outputs) != ESP_OK) {
        kerror("display: cannot configure the GPIOs");
        return -1;
    }

    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_RST, 1);
    gpio_set_level(PIN_LCD_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };

    if (spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        kerror("display: cannot initialise the SPI bus");
        return -2;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_HZ,
        .mode = 0,
        .spics_io_num = -1, /* the chip select is driven by the CPU */
        .queue_size = 1,
    };

    if (spi_bus_add_device(LCD_SPI_HOST, &dev, &s_dev) != ESP_OK) {
        kerror("display: cannot add the SPI device");
        return -3;
    }

    s_fb = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        /*
         * No PSRAM: fall back to the internal heap. MALLOC_CAP_8BIT keeps
         * the frame off the exec-only IRAM region, where the stores of the
         * drawing code would fault.
         */
        s_fb = heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        kerror("display: no memory for the framebuffer");
        return -4;
    }

    lcd_init_panel();
    lcd_draw_test_pattern();

    s_initialised = true;
    return 0;
}

static int drv_display_check(void)
{
    if (!s_initialised || s_fb == NULL) {
        return -1;
    }

    return 0;
}

static const char *drv_display_detail(void)
{
    static char buf[48];

    if (s_fb == NULL) {
        return "no framebuffer";
    }

    snprintf(buf, sizeof(buf), "%dx%d, fb %u KB", LCD_WIDTH, LCD_HEIGHT,
             (unsigned)(LCD_WIDTH * LCD_HEIGHT * 2 / 1024));
    return buf;
}

static const struct kmod s_display_module = {
    .name = "display",
    .desc = "ST7789v2 135x240 TFT",
    .init = drv_display_init,
    .check = drv_display_check,
    .detail = drv_display_detail,
};

int drv_display_register(void)
{
    return kmod_register(&s_display_module);
}

/* --- the text on the screen --- */

int drv_display_clear(uint16_t color)
{
    k_size_t i;
    k_size_t count = (k_size_t)LCD_WIDTH * LCD_HEIGHT;

    if (s_fb == NULL) {
        return -1;
    }

    display_lock();

    for (i = 0; i < count; i++) {
        s_fb[i] = color;
    }

    display_unlock();

    return 0;
}

int drv_display_text(int x, int y, const char *text, uint16_t color)
{
    struct fb_view view;
    int cursor = x;

    if (s_fb == NULL || text == NULL) {
        return -1;
    }

    display_lock();
    display_view(&view);

    while (*text != '\0') {
        const uint8_t *glyph = font_glyph(*text);
        int column;
        int row;

        if (cursor + FONT_WIDTH > fb_view_width(&view)) {
            break; /* the line is full */
        }

        if (y + FONT_HEIGHT > fb_view_height(&view)) {
            break; /* the picture is full */
        }

        for (column = 0; column < FONT_WIDTH; column++) {
            uint8_t bits = glyph[column];

            for (row = 0; row < FONT_HEIGHT; row++) {
                if ((bits & (1u << row)) != 0) {
                    /* the set bits go through the turn of the picture */
                    fb_view_pixel(&view, cursor + column, y + row, color);
                }
            }
        }

        cursor += FONT_WIDTH + 1;
        text++;
    }

    display_unlock();

    return 0;
}

int drv_display_flush(void)
{
    if (s_fb == NULL) {
        return -1;
    }

    /*
     * The whole transfer is one hold of the lock: the window and the
     * pixels that follow it belong together, and the offset of the image
     * must not change in the middle of the two.
     */
    display_lock();
    lcd_flush();
    display_unlock();

    return 0;
}

void drv_display_set_offset(int x, int y)
{
    display_lock();
    s_offset_x = x;
    s_offset_y = y;
    display_unlock();
}

void drv_display_get_offset(int *x, int *y)
{
    display_lock();

    if (x != NULL) {
        *x = s_offset_x;
    }
    if (y != NULL) {
        *y = s_offset_y;
    }

    display_unlock();
}

/* --- the picture as its viewer sees it --- */

int drv_display_width(void)
{
    struct fb_view view;

    display_lock();
    display_view(&view);
    display_unlock();

    return fb_view_width(&view);
}

int drv_display_height(void)
{
    struct fb_view view;

    display_lock();
    display_view(&view);
    display_unlock();

    return fb_view_height(&view);
}

void drv_display_set_rotation(int rotation)
{
    display_lock();
    s_rotation = ((rotation % 4) + 4) % 4; /* -90 is the third quarter turn */
    display_unlock();
}

int drv_display_rotation(void)
{
    int rotation;

    display_lock();
    rotation = s_rotation;
    display_unlock();

    return rotation;
}

void drv_display_set_inverted(bool inverted)
{
    display_lock();
    lcd_write_command(inverted ? 0x21 : 0x20); /* INVON / INVOFF */
    s_inverted = inverted;
    display_unlock();
}

bool drv_display_inverted(void)
{
    bool inverted;

    display_lock();
    inverted = s_inverted;
    display_unlock();

    return inverted;
}

void drv_display_pixel(int x, int y, uint16_t color)
{
    struct fb_view view;

    if (s_fb == NULL) {
        return;
    }

    display_lock();
    display_view(&view);
    fb_view_pixel(&view, x, y, color);
    display_unlock();
}

void drv_display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    struct fb_view view;

    if (s_fb == NULL) {
        return;
    }

    display_lock();
    display_view(&view);
    fb_view_fill_rect(&view, x, y, w, h, color);
    display_unlock();
}

void drv_display_rect(int x, int y, int w, int h, uint16_t color)
{
    struct fb_view view;

    if (s_fb == NULL) {
        return;
    }

    display_lock();
    display_view(&view);
    fb_view_rect(&view, x, y, w, h, color);
    display_unlock();
}

void drv_display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    struct fb_view view;

    if (s_fb == NULL) {
        return;
    }

    display_lock();
    display_view(&view);
    fb_view_line(&view, x0, y0, x1, y1, color);
    display_unlock();
}
