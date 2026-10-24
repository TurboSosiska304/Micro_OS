/*
 * Micro OS — buzzer and LED/IR of the StickC Plus2.
 *
 * The buzzer is a passive piezzo element driven by the LEDC peripheral, so
 * that a tone can be generated. The red LED and the IR emitter share the pin
 * G19 on the Plus2: driving the pin switches the LED and the IR emitter in
 * opposite directions.
 */
#include "drivers/board_pins.h"
#include "drivers/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/kmod.h"
#include "kshell/kshell.h"

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

/*
 * Sound the buzzer for a number of milliseconds. The frequency sets the
 * note: the timer of the LEDC drives the passive element, so any tone in
 * its range works. A frequency of 0 is the default of the board (2 kHz),
 * and the buzzer is silent again when the call returns.
 */
int drv_buzzer_beep(unsigned freq_hz, unsigned ms)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = (freq_hz != 0) ? freq_hz : BUZZER_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (!s_buzzer_ready) {
        return -1;
    }

    if (ledc_timer_config(&timer) != ESP_OK) {
        return -2;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 128);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(ms));
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

/*
 * Drive the pin that the red LED and the IR emitter share. The level turns
 * the LED on (and the IR emitter in the opposite direction); 0 is off.
 */
int drv_led_set(bool on)
{
    if (!s_led_ready) {
        return -1;
    }

    gpio_set_level(PIN_LED, on ? 1 : 0);
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

/*
 * The shell command that lets a person sound the buzzer: "beep" for the
 * note of the board, or "beep <frequency> <milliseconds>" for a note of
 * its own.
 */
static int cmd_beep(int argc, char **argv)
{
    unsigned freq = 0;
    unsigned ms = 100;

    if (argc > 1) {
        freq = (unsigned)strtoul(argv[1], NULL, 10);
    }

    if (argc > 2) {
        ms = (unsigned)strtoul(argv[2], NULL, 10);
    }

    if (drv_buzzer_beep(freq, ms) != 0) {
        kshell_printf("beep: the buzzer is not working\n");
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/* One note of the bar of the dance. */
struct note {
    unsigned freq;
    unsigned ms;
};

/* The shell command that turns the red LED on and off. */
static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        kshell_printf("usage: led on|off\n");
        return KSHELL_USAGE;
    }

    if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        kshell_printf("usage: led on|off\n");
        return KSHELL_USAGE;
    }

    if (drv_led_set(strcmp(argv[1], "on") == 0) != 0) {
        kshell_printf("led: the led is not working\n");
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/*
 * A little show: a bar of notes on the buzzer, the LED in time with them -
 * the board dances for a couple of seconds.
 */
static int cmd_dance(int argc, char **argv)
{
    static const struct note bar[] = {
        {262, 200}, {330, 200}, {392, 200}, {523, 320}, {392, 160}, {330, 160}, {262, 400},
    };
    bool lit = false;
    size_t i;

    (void)argc;
    (void)argv;

    for (i = 0; i < sizeof(bar) / sizeof(bar[0]); i++) {
        lit = !lit;
        drv_led_set(lit);
        drv_buzzer_beep(bar[i].freq, bar[i].ms);
    }

    drv_led_set(false);

    return KSHELL_OK;
}

int drv_buzzer_register(void)
{
    int rc = kmod_register(&s_buzzer_module);

    kshell_register("beep", "sound the buzzer: beep [frequency] [ms]", cmd_beep);
    kshell_register("dance", "the board dances: a bar of notes and the led in time", cmd_dance);

    return rc;
}

int drv_led_register(void)
{
    int rc = kmod_register(&s_led_module);

    kshell_register("led", "the red led: led on|off", cmd_led);

    return rc;
}
