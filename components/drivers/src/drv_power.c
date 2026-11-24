/*
 * Micro OS — deep sleep and the power latch of the StickC Plus2.
 *
 * The board keeps its own power through HOLD (G4) and has no other way to
 * know that it is still wanted: after a deep sleep the chip restarts with
 * the pin floating and, on a battery, the latch opens before the firmware
 * has said a word. So the first statement of the boot drives the pin high.
 *
 * The wake line is the one of BUTTON C (G35). The button closes the WAKEn
 * net to ground and a diode carries that low to G35; the external pull-up of
 * the net holds the line high while nobody presses. The deep sleep arms EXT1
 * with the level "all low", which is the only level an ESP32 offers for a
 * group of pins, and with one pin in the group it means exactly "the line
 * went low". The timer of the chip is the other way back.
 *
 * The INT of the RTC is not on that line: it drives the latch of the power
 * supply (D22 of the schematic), so an alarm can switch on a board that is
 * off and only sets a flag while the board is on. A sleeping board still has
 * to obey its alarm, so a sleep asks the clock for the seconds to the next
 * match and arms the timer of the chip for that moment: the chip comes back
 * when the alarm fires, and the flag of the clock in the next boot says that
 * the alarm was the reason. That is the only way this wiring allows, and the
 * log says which of the two timers woke the board.
 *
 * The driver also owns the commands that make the sleep visible:
 * `power sleep <seconds>`, `power sleep alarm` and `power off`. A board that
 * goes silent without a word is a board that looks broken.
 */
#include "drivers/board_pins.h"
#include "drivers/power.h"
#include "drivers/rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kernel/kmod.h"
#include "kernel/power.h"
#include "kshell/kshell.h"

/*
 * A line of the console is written into the FIFO of the UART and the power
 * may go before the last character leaves it: the short wait is for the
 * person who reads the log, not for the chip.
 */
#define POWER_QUIET_MS 200

/* The longest sleep the command accepts: a sanity bound, not a limit of the
 * timer, which counts the microseconds of a 64 bit number. */
#define POWER_SLEEP_MAX_S (31UL * 24UL * 60UL * 60UL)

static bool s_hold;

int drv_power_hold(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << PIN_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    /* A hold left by the deep sleep freezes the pad: release it before the
     * pin is configured and driven again. */
    (void)gpio_hold_dis(PIN_HOLD);
    gpio_deep_sleep_hold_dis();

    if (gpio_config(&config) != ESP_OK) {
        return -1;
    }

    if (gpio_set_level(PIN_HOLD, 1) != ESP_OK) {
        return -2;
    }

    s_hold = true;

    return 0;
}

const char *drv_power_wakeup_reason(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return "timer";
    case ESP_SLEEP_WAKEUP_EXT1:
        return "ext1";
    case ESP_SLEEP_WAKEUP_EXT0:
        return "ext0";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        return "touch";
    case ESP_SLEEP_WAKEUP_ULP:
        return "ulp";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "power-on";
    default:
        return "unknown";
    }
}

/*
 * The seconds until the alarm of the clock, when one is armed and its time
 * can be counted: a negative answer means that the alarm cannot be a wake
 * source and the caller should carry on with its own timer.
 */
static int alarm_wake_seconds(k_u32_t *seconds)
{
    return drv_rtc_alarm_seconds_ahead(seconds);
}

/*
 * Arm the timer for the sooner of `seconds` and the alarm, keep HOLD, and go
 * down. `seconds` of 0 means "no timer of its own": the board then sleeps
 * until the alarm, or until the button when no alarm is armed.
 */
static int power_sleep_start(k_u32_t seconds)
{
    k_u32_t alarm_s = 0;
    k_u32_t wake_s = seconds;
    esp_err_t rc;

    /*
     * The alarm is a wake source even though its INT is not on the wake
     * line: the timer of the chip is set for the moment of the match, and
     * the clock's own flag tells the next boot that the alarm was the
     * reason. It wakes the board sooner than the timer when it is sooner.
     */
    if (alarm_wake_seconds(&alarm_s) == 0 && (wake_s == 0 || alarm_s < wake_s)) {
        wake_s = alarm_s;
    }

    if (wake_s > POWER_SLEEP_MAX_S) {
        return -1;
    }

    if (wake_s > 0) {
        rc = esp_sleep_enable_timer_wakeup((uint64_t)wake_s * 1000ULL * 1000ULL);
        if (rc != ESP_OK) {
            return -2;
        }
    }

    rc = esp_sleep_enable_ext1_wakeup(1ULL << PIN_WAKE, ESP_EXT1_WAKEUP_ALL_LOW);
    if (rc != ESP_OK) {
        return -3;
    }

    /*
     * HOLD is kept by the pad itself while the chip sleeps. Without the hold
     * the pin floats, the latch of the board lets go and a board on a battery
     * switches itself off instead of sleeping — the difference between a
     * wearable that wakes at its timer and one that is dead in the morning.
     */
    if (gpio_hold_en(PIN_HOLD) != ESP_OK) {
        return -4;
    }
    gpio_deep_sleep_hold_en();

    /* Nobody will write the rest of this buffer after the power goes. */
    vTaskDelay(pdMS_TO_TICKS(POWER_QUIET_MS));

    esp_deep_sleep_start();

    /* esp_deep_sleep_start() is declared noreturn; the line keeps the
     * compiler happy when the attribute is not seen. */
    return -5;
}

int drv_power_sleep(k_u32_t seconds)
{
    if (seconds > POWER_SLEEP_MAX_S) {
        return -1;
    }

    return power_sleep_start(seconds);
}

int drv_power_sleep_until_alarm(void)
{
    k_u32_t alarm_s = 0;
    int rc;

    rc = alarm_wake_seconds(&alarm_s);
    if (rc != 0) {
        return rc;
    }

    return power_sleep_start(alarm_s);
}

/*
 * The off of the clean end (ADR 0050): the board keeps its latch through
 * HOLD and the chip sleeps until BUTTON C. There is no timer and no alarm
 * on this path - "off" means "until somebody presses the button" - and the
 * clock's alarm is cleared first for the reason drv_power_off() clears it:
 * the INT line is part of the latch, and a fired alarm would hold the power
 * supply closed while the chip slept.
 *
 * A board on a battery with no wake source would be gone; a board of this
 * one wakes on the button, which is the only switch it has.
 */
int drv_power_deep_off(void)
{
    esp_err_t rc;

    (void)drv_rtc_alarm_clear();

    rc = esp_sleep_enable_ext1_wakeup(1ULL << PIN_WAKE, ESP_EXT1_WAKEUP_ALL_LOW);
    if (rc != ESP_OK) {
        return -1;
    }

    if (gpio_hold_en(PIN_HOLD) != ESP_OK) {
        return -2;
    }
    gpio_deep_sleep_hold_en();

    vTaskDelay(pdMS_TO_TICKS(POWER_QUIET_MS));

    esp_deep_sleep_start();

    return -3;
}

int drv_power_off(void)
{
    /*
     * The INT line of the clock is part of the latch of the power supply:
     * a flag left behind by an alarm would hold the latch closed and the
     * board would stay on after HOLD drops. The alarm goes first.
     */
    (void)drv_rtc_alarm_clear();

    if (gpio_set_level(PIN_HOLD, 0) != ESP_OK) {
        return -1;
    }

    /* On a battery the board is gone here and this wait never ends. With
     * the cable in, the board keeps running and the latch has to be put
     * back, or the next cable pull would kill a board that looks on. */
    vTaskDelay(pdMS_TO_TICKS(POWER_QUIET_MS));

    if (drv_power_hold() != 0) {
        return -1;
    }

    return 0;
}

/* --- the module of the doctor and the shell --- */

static int drv_power_check(void)
{
    return s_hold ? 0 : -1;
}

static const char *drv_power_detail(void)
{
    static char buf[KMOD_INFO_MAX];
    k_u32_t alarm_s = 0;

    /*
     * The alarm is the part of the line a person cannot guess: an armed
     * alarm is a wake of the board in so many seconds, and a board that
     * sleeps "until its alarm" has to say when that is.
     */
    if (alarm_wake_seconds(&alarm_s) == 0) {
        snprintf(buf, sizeof(buf), "hold %s, wake %s, alarm in %lu s", s_hold ? "on" : "off",
                 drv_power_wakeup_reason(), (unsigned long)alarm_s);
    } else {
        snprintf(buf, sizeof(buf), "hold %s, wake %s", s_hold ? "on" : "off",
                 drv_power_wakeup_reason());
    }

    return buf;
}

static const struct kmod s_power_module = {
    .name = "power",
    .desc = "HOLD on G4, deep sleep, wake on button C",
    .init = drv_power_hold,
    .check = drv_power_check,
    .detail = drv_power_detail,
};

static const char *power_error_text(int rc)
{
    switch (rc) {
    case -1:
        return "there is no such length of a sleep";
    case -2:
        return "the timer of the wake-up is not armed";
    case -3:
        return "the wake line is not armed";
    case -4:
        return "the latch cannot be held through the sleep";
    default:
        return "the board did not go to sleep";
    }
}

/* Why a sleep that waits for the alarm cannot be arranged. */
static const char *alarm_error_text(int rc)
{
    switch (rc) {
    case -1:
        return "there is no clock";
    case -2:
        return "no alarm is armed: rtc alarm <HH:MM> first";
    case -3:
        return "the clock has no time to count from: date set first";
    case -4:
        return "the alarm matches no minute of the next month";
    default:
        return power_error_text(rc);
    }
}

/* `power sleep alarm`: the clock of the board decides when it comes back. */
static int sleep_until_alarm(void)
{
    k_u32_t alarm_s = 0;
    int rc;

    rc = drv_rtc_alarm_seconds_ahead(&alarm_s);
    if (rc != 0) {
        kshell_printf("power sleep: %s\n", alarm_error_text(rc));
        return KSHELL_ERROR;
    }

    kshell_printf("power sleep: the rtc alarm wakes the board in %lu s\n", (unsigned long)alarm_s);

    rc = drv_power_sleep_until_alarm();

    kshell_printf("power sleep: %s\n", alarm_error_text(rc));

    return KSHELL_ERROR;
}

/* `power sleep <seconds>`: the timer wakes the board, the button sooner. */
static int cmd_sleep(int argc, char **argv)
{
    k_u32_t alarm_s = 0;
    char *end = NULL;
    unsigned long seconds;
    int rc;

    if (argc != 3) {
        kshell_printf("usage: power sleep <seconds> | power sleep alarm\n");
        kshell_printf("       (button C wakes it sooner)\n");
        return KSHELL_USAGE;
    }

    if (strcmp(argv[2], "alarm") == 0) {
        return sleep_until_alarm();
    }

    seconds = strtoul(argv[2], &end, 10);
    if (argv[2][0] == '\0' || end == NULL || *end != '\0' || seconds == 0 ||
        seconds > POWER_SLEEP_MAX_S) {
        kshell_printf("power sleep: no such number of seconds: %s\n", argv[2]);
        kshell_printf("power sleep: 1..%lu seconds, or `alarm`\n",
                      (unsigned long)POWER_SLEEP_MAX_S);
        return KSHELL_ERROR;
    }

    kshell_printf("power sleep: %lu s, wake on the timer or button C\n", seconds);

    /*
     * A person who armed the alarm of the clock and then went to sleep waits
     * for the alarm. The chip has no way to hear the INT of the clock, so
     * the sleep sets its timer for the alarm: the line says when that is, so
     * the wait is a number and not a hope.
     */
    if (drv_rtc_alarm_seconds_ahead(&alarm_s) == 0) {
        if (alarm_s < (k_u32_t)seconds) {
            kshell_printf("power sleep: the rtc alarm wakes the board sooner, in %lu s\n",
                          (unsigned long)alarm_s);
        } else {
            kshell_printf("power sleep: the rtc alarm is armed for %lu s from now\n",
                          (unsigned long)alarm_s);
        }
    }

    rc = drv_power_sleep((k_u32_t)seconds);

    kshell_printf("power sleep: %s\n", power_error_text(rc));

    return KSHELL_ERROR;
}

/* `power off`: the clean end, then deep sleep; button C wakes the board. */
static int cmd_poweroff(void)
{
    int rc;

    kshell_printf("power off: no new session, the volumes are synced and the disks flushed\n");

    rc = k_power_shutdown(K_POWER_OFF, false);
    if (rc != 0) {
        kshell_printf("power off: refused (rc=%d), the board stays on\n", rc);
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

/* `power`: the line and the reason of the boot, then the two commands. */
static int cmd_power(int argc, char **argv)
{
    if (argc == 1) {
        kshell_printf("power: hold %s, woke from %s\n", s_hold ? "high" : "not high",
                      drv_power_wakeup_reason());

        /*
         * The alarm of the clock is the one wake a person cannot see in the
         * reason of the chip: its INT is not on the wake line, so the timer
         * of the chip was set for its moment. The flag of the clock is what
         * tells the two apart, and the line says which one it was.
         */
        if (drv_rtc_alarm_was_fired()) {
            if (strcmp(drv_power_wakeup_reason(), "timer") == 0 && drv_rtc_alarm_was_armed()) {
                kshell_printf("power: the rtc alarm woke the board\n");
            } else {
                kshell_printf("power: the rtc alarm went off before this boot\n");
            }
        }

        return KSHELL_OK;
    }

    if (strcmp(argv[1], "sleep") == 0) {
        return cmd_sleep(argc, argv);
    }

    if (strcmp(argv[1], "off") == 0 && argc == 2) {
        return cmd_poweroff();
    }

    kshell_printf("usage: power | power sleep <seconds> | power sleep alarm | power off\n");

    return KSHELL_USAGE;
}

int drv_power_register(void)
{
    kshell_register("power", "the power: power, power sleep <s>|alarm, power off", cmd_power);

    return kmod_register(&s_power_module);
}
