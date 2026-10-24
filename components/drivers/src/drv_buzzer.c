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

/*
 * The red LED has a duty of its own, on a timer of its own: 0 is dark, the
 * maximum is the whole light, and everything between is the light in the
 * middle - that is what the shows shine with.
 */
#define LED_CHANNEL 1
#define LED_TIMER LEDC_TIMER_1
#define LED_FREQ 5000
#define LED_DUTY_MAX ((1 << 10) - 1)
#define LED_STEP_MS 4

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
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LED_TIMER,
        .freq_hz = LED_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (ledc_timer_config(&timer) != ESP_OK) {
        return -1;
    }

    ledc_channel_config_t channel = {
        .gpio_num = PIN_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LED_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    if (ledc_channel_config(&channel) != ESP_OK) {
        return -2;
    }

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
     * so what has to work is the duty itself: the light goes all the way up
     * and down again, and a person can see it during the check.
     */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL, LED_DUTY_MAX);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(30));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL);

    return 0;
}

/* The duty of the light: 0 is dark, the maximum is the whole light. */
static void led_duty(unsigned duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_CHANNEL);
}

/*
 * Drive the pin that the red LED and the IR emitter share. The light turns
 * on (and the IR emitter in the opposite direction); 0 is off.
 */
int drv_led_set(bool on)
{
    if (!s_led_ready) {
        return -1;
    }

    led_duty(on ? LED_DUTY_MAX : 0);
    return 0;
}

/*
 * Move the light from one brightness to another in a number of
 * milliseconds. The duty changes every LED_STEP_MS, so the eye sees the
 * light slide instead of jump.
 */
static int drv_led_fade(int from, int to, unsigned ms)
{
    unsigned count = ms / LED_STEP_MS + 1;
    unsigned i;

    if (!s_led_ready) {
        return -1;
    }

    for (i = 1; i <= count; i++) {
        int duty = from + ((to - from) * (int)i) / (int)count;

        led_duty((unsigned)duty);
        vTaskDelay(pdMS_TO_TICKS(LED_STEP_MS));
    }

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

/*
 * The show "led heart": the light of the LED grows slowly and leaves just
 * as slowly, three times in a row - the board breathes for a person. The
 * command turns the LED dark again when the show is over.
 */
static int led_heart(void)
{
    int beat;
    int rc = 0;

    for (beat = 0; beat < 3 && rc == 0; beat++) {
        rc = drv_led_fade(0, LED_DUTY_MAX, 700);
        if (rc == 0) {
            rc = drv_led_fade(LED_DUTY_MAX, 0, 700);
        }
    }

    return rc;
}

/*
 * The show "led dance": the light hops on the beat, swings between the
 * half light and the whole, and takes a bow - a slow bow down and a last
 * quick flash.
 */
static int led_dance(void)
{
    int i;
    int rc = 0;

    /* It hops: four times on and off, quick. */
    for (i = 0; i < 4 && rc == 0; i++) {
        (void)drv_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(90));
        (void)drv_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(90));
    }

    /* It swings: twice from the dark to the half light to the whole. */
    for (i = 0; i < 2 && rc == 0; i++) {
        rc = drv_led_fade(0, LED_DUTY_MAX / 2, 200);
        if (rc == 0) {
            rc = drv_led_fade(LED_DUTY_MAX / 2, LED_DUTY_MAX, 200);
        }
        if (rc == 0) {
            rc = drv_led_fade(LED_DUTY_MAX, 0, 250);
        }
    }

    /* It bows: the light goes down slowly, and the last flash closes it. */
    if (rc == 0) {
        rc = drv_led_fade(LED_DUTY_MAX, 0, 500);
    }
    if (rc == 0) {
        (void)drv_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    return rc;
}

/*
 * The shell command for the red LED: "led on" and "led off" are the light
 * itself, "led heart" and "led dance" are the shows. A show always ends
 * with the LED dark.
 */
static int cmd_led(int argc, char **argv)
{
    const char *action;
    int rc;

    if (argc < 2) {
        kshell_printf("usage: led on|off|heart|dance\n");
        return KSHELL_USAGE;
    }

    action = argv[1];

    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
        if (drv_led_set(strcmp(action, "on") == 0) != 0) {
            kshell_printf("led: the led is not working\n");
            return KSHELL_ERROR;
        }
        return KSHELL_OK;
    }

    if (strcmp(action, "heart") == 0) {
        rc = led_heart();
    } else if (strcmp(action, "dance") == 0) {
        rc = led_dance();
    } else {
        kshell_printf("usage: led on|off|heart|dance\n");
        return KSHELL_USAGE;
    }

    (void)drv_led_set(false); /* whatever happened, the show ends dark */

    if (rc != 0) {
        kshell_printf("led: the led is not working\n");
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

int drv_buzzer_register(void)
{
    int rc = kmod_register(&s_buzzer_module);

    kshell_register("beep", "sound the buzzer: beep [frequency] [ms]", cmd_beep);

    return rc;
}

int drv_led_register(void)
{
    int rc = kmod_register(&s_led_module);

    kshell_register("led", "the red led: led on|off|heart|dance", cmd_led);

    return rc;
}
