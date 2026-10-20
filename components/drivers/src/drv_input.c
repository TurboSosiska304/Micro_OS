/*
 * Micro OS — buttons of the StickC Plus2.
 *
 * The three buttons are pulled up, a pressed button reads as a low level.
 */
#include "drivers/board_pins.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "kernel/kmod.h"

static const int s_buttons[] = {PIN_BUTTON_A, PIN_BUTTON_B, PIN_BUTTON_C};

static bool s_initialised;

static int drv_input_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_A) | (1ULL << PIN_BUTTON_B) | (1ULL << PIN_BUTTON_C),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (gpio_config(&cfg) != ESP_OK) {
        return -1;
    }

    s_initialised = true;
    return 0;
}

static int drv_input_check(void)
{
    int i;

    if (!s_initialised) {
        return -1;
    }

    for (i = 0; i < 3; i++) {
        if (gpio_get_level(s_buttons[i]) != 1) {
            /* A button is pressed: the hardware works, but the user is
             * touching the board. This is not a problem. */
            return 0;
        }
    }

    return 0;
}

static const char *drv_input_detail(void)
{
    static char buf[48];

    snprintf(buf, sizeof(buf), "A=%s B=%s C=%s", gpio_get_level(s_buttons[0]) ? "up" : "down",
             gpio_get_level(s_buttons[1]) ? "up" : "down",
             gpio_get_level(s_buttons[2]) ? "up" : "down");
    return buf;
}

static const struct kmod s_input_module = {
    .name = "input",
    .desc = "buttons A, B and C",
    .init = drv_input_init,
    .check = drv_input_check,
    .detail = drv_input_detail,
};

int drv_input_register(void)
{
    return kmod_register(&s_input_module);
}
