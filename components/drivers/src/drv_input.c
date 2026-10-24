/*
 * Micro OS — buttons of the StickC Plus2.
 *
 * The three buttons are pulled up, a pressed button reads as a low level.
 * The buttons are read by whoever waits for them: drv_input_wait samples
 * the pins and puts the samples through the debounce (the pure part of it
 * lives in input_debounce.c), so there is no interrupt and no background
 * task here — while nobody waits, the driver does not run at all.
 *
 * The debounced state lives in the driver and not in the waiters: a held
 * button is one press and not one press per read, and the release of it
 * is told after the press even when the two waits are two calls of the
 * same command.
 *
 * The shell carries the events as "input [seconds]", and the system
 * carries them as the file /dev/input: one event per read, and the read
 * waits for as long as it takes.
 */
#include "drivers/board_pins.h"
#include "drivers/input.h"
#include "drivers/input_debounce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "devfs/devfs.h"
#include "driver/gpio.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kshell/kshell.h"

static const int s_buttons[] = {PIN_BUTTON_A, PIN_BUTTON_B, PIN_BUTTON_C};
static const char *const s_names[] = {"A", "B", "C"};

static bool s_initialised;

/*
 * What the buttons are believed to be. It lives between the waits: the
 * samples stop when nobody waits, so a button that is pressed and
 * released in that gap is not seen at all — the events belong to
 * somebody who listens — but everything the samples did see is kept.
 */
static struct input_debounce s_state;

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

    input_debounce_init(&s_state);

    s_initialised = true;
    return 0;
}

static int drv_input_check(void)
{
    int i;

    if (!s_initialised) {
        return -1;
    }

    for (i = 0; i < INPUT_BUTTON_COUNT; i++) {
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

/* --- the events --- */

int drv_input_wait(struct input_event *out, k_u32_t timeout_ms)
{
    k_u64_t start = k_port_uptime_ms();

    if (out == NULL || !s_initialised) {
        return -1;
    }

    for (;;) {
        k_u64_t now = k_port_uptime_ms();
        int button;
        bool found = false;

        /*
         * One sample of every button, under the critical section: two
         * waiters run this at the same time, and the state of a button
         * must change exactly once, so that exactly one of them is told.
         */
        k_port_enter_critical();

        for (button = 0; button < INPUT_BUTTON_COUNT; button++) {
            bool raw = (gpio_get_level(s_buttons[button]) == 0); /* pressed = low */
            bool pressed = false;

            if (input_debounce_sample(&s_state, button, raw, (k_u32_t)now, &pressed) == 1) {
                out->time_ms = now;
                out->button = (k_u8_t)button;
                out->pressed = pressed ? 1 : 0;
                out->reserved = 0;
                found = true;
                break;
            }
        }

        k_port_exit_critical();

        if (found) {
            return 0;
        }

        if (timeout_ms != INPUT_WAIT_FOREVER && (now - start) >= (k_u64_t)timeout_ms) {
            return -1;
        }

        k_sched_sleep_ms(INPUT_POLL_MS);
    }
}

/* --- the file /dev/input --- */

/* One event per read: the read of the system waits for the next one. */
static int input_dev_read(char *buf, k_size_t size)
{
    struct input_event event;
    k_size_t take = sizeof(event);

    if (drv_input_wait(&event, INPUT_WAIT_FOREVER) != 0) {
        return -1;
    }

    if (size < take) {
        take = size; /* a part of an event is still what was asked for */
    }

    memcpy(buf, &event, take);
    return (int)take;
}

/* --- the shell --- */

/*
 * The command "input": watch the buttons and print every press and every
 * release as it happens. Ten seconds by default, a number of seconds by
 * hand — "input 60" watches for a minute.
 */
static int cmd_input(int argc, char **argv)
{
    k_u64_t seconds = 10;
    k_u64_t start;

    if (argc > 1) {
        seconds = strtoul(argv[1], NULL, 10);
    }

    if (seconds == 0 || seconds > 3600) {
        kshell_printf("usage: input [seconds]\n");
        return KSHELL_USAGE;
    }

    if (!s_initialised) {
        kshell_printf("input: the buttons are not working\n");
        return KSHELL_ERROR;
    }

    kshell_printf("input: press A, B or C (%llu s)\n", (unsigned long long)seconds);
    start = k_port_uptime_ms();

    for (;;) {
        struct input_event event;
        k_u64_t elapsed = k_port_uptime_ms() - start;
        k_u64_t left_ms = seconds * 1000;

        if (elapsed >= left_ms) {
            break;
        }

        if (drv_input_wait(&event, (k_u32_t)(left_ms - elapsed)) != 0) {
            break; /* the time is over */
        }

        kshell_printf("%s %s at %llu ms\n", s_names[event.button], event.pressed ? "down" : "up",
                      (unsigned long long)event.time_ms);
    }

    return KSHELL_OK;
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
    int rc = kmod_register(&s_input_module);

    if (s_initialised && devfs_add("input", input_dev_read, NULL) != 0) {
        kerror("input: /dev/input cannot be added");
        rc = -1;
    }

    kshell_register("input", "the buttons: input [seconds]", cmd_input);

    return rc;
}
