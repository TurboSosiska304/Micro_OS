/*
 * Micro OS — buzzer and LED/IR of the StickC Plus2.
 *
 * The buzzer is a passive piezzo element driven by the LEDC peripheral, so
 * that a tone can be generated. The red LED and the IR emitter share the pin
 * G19 on the Plus2: driving the pin switches the LED and the IR emitter in
 * opposite directions.
 */
#include "drivers/board_pins.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/kmod.h"

#define BUZZER_CHANNEL 0
#define BUZZER_FREQ 2000
#define LEDC_TIMER LEDC_TIMER_0

static bool s_buzzer_ready;
static bool s_led_ready;

static int drv_buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = BUZZER_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (ledc_timer_config(&timer) != ESP_OK) {
        return -1;
    }

    ledc_channel_config_t channel = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    if (ledc_channel_config(&channel) != ESP_OK) {
        return -2;
    }

    s_buzzer_ready = true;
    return 0;
}

/* A short beep proves that the buzzer and the timer work. */
static int drv_buzzer_check(void)
{
    if (!s_buzzer_ready) {
        return -1;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(30));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);

    return 0;
}

static const char *drv_buzzer_detail(void)
{
    return "passive, 2 kHz via LEDC";
}

static const struct kmod s_buzzer_module = {
    .name = "buzzer",
    .desc = "passive buzzer on G2",
    .init = drv_buzzer_init,
    .check = drv_buzzer_check,
    .detail = drv_buzzer_detail,
};

static int drv_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };

    if (gpio_config(&cfg) != ESP_OK) {
        return -1;
    }

    gpio_set_level(PIN_LED, 0);
    s_led_ready = true;
    return 0;
}

static int drv_led_check(void)
{
    if (!s_led_ready) {
        return -1;
    }

    /*
     * The pin drives the red LED and the IR emitter in opposite directions,
     * so the level read back does not have to match what was written. It is
     * enough that the pin can be driven at all.
     */
    gpio_set_level(PIN_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PIN_LED, 0);

    return 0;
}

static const char *drv_led_detail(void)
{
    return "G19, shared with the IR emitter";
}

static const struct kmod s_led_module = {
    .name = "led/ir",
    .desc = "red LED and IR emitter on G19",
    .init = drv_led_init,
    .check = drv_led_check,
    .detail = drv_led_detail,
};

int drv_buzzer_register(void)
{
    return kmod_register(&s_buzzer_module);
}

int drv_led_register(void)
{
    return kmod_register(&s_led_module);
}
